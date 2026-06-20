#include "llm.h"
#include "crypto.h"
#include "error.h"
#include "tools.h"
#include "config.h"
#include "provider.h"
#include "workspace.h"
#include "mcp_names.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <signal.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>
#include <curl/curl.h>

#define CLAUDE_CODE_BILLING_SALT       "59cf53e54c78"
#define CLAUDE_CODE_VERSION_FALLBACK   "2.1.37"
#define CLAUDE_CODE_OAUTH_BETA         "oauth-2025-04-20"

/* Global interrupt flag — set by SIGINT handler in agent.c.
   Declared extern here so the streaming code can check it. */
extern volatile int g_interrupted;

/* Stream heartbeat — when non-NULL, the write callback feeds byte counts
   into this so the heartbeat thread can track receive activity. */
#include "tui.h"
tui_stream_heartbeat_t *g_stream_heartbeat = NULL;

/* ── Session state ─────────────────────────────────────────────────────── */

const char *session_trust_tier_to_string(dsco_trust_tier_t tier) {
    switch (tier) {
    case DSCO_TRUST_TRUSTED: return "trusted";
    case DSCO_TRUST_UNTRUSTED: return "untrusted";
    case DSCO_TRUST_STANDARD:
    default:
        return "standard";
    }
}

dsco_trust_tier_t session_trust_tier_from_string(const char *s, bool *ok) {
    if (!s || !s[0]) {
        if (ok) *ok = false;
        return DSCO_TRUST_STANDARD;
    }
    if (strcasecmp(s, "trusted") == 0) {
        if (ok) *ok = true;
        return DSCO_TRUST_TRUSTED;
    }
    if (strcasecmp(s, "untrusted") == 0) {
        if (ok) *ok = true;
        return DSCO_TRUST_UNTRUSTED;
    }
    if (strcasecmp(s, "standard") == 0) {
        if (ok) *ok = true;
        return DSCO_TRUST_STANDARD;
    }
    if (ok) *ok = false;
    return DSCO_TRUST_STANDARD;
}

static bool llm_env_truthy(const char *val) {
    return val && (val[0] == '1' || strcasecmp(val, "true") == 0 ||
                   strcasecmp(val, "yes") == 0);
}

void session_state_init(session_state_t *s, const char *model) {
    memset(s, 0, sizeof(*s));
    const char *resolved = model_resolve_alias(model);
    snprintf(s->model, sizeof(s->model), "%s", resolved);
    snprintf(s->effort, sizeof(s->effort), "%s", EFFORT_HIGH);
    s->trust_tier = DSCO_TRUST_STANDARD;
    s->web_search = true;
    s->code_execution = true;
    s->context_window = model_context_window(resolved);
    s->compact_enabled = true;
    s->temperature = -1.0;
    s->top_p = -1.0;
    s->top_k = -1;
    s->thinking_budget = 0;
    s->active_topology[0] = '\0';
    s->topology_auto = false;
    s->tool_budget_ratio = 1.0f;

    if (!llm_env_truthy(getenv("DSCO_DISABLE_DEFAULT_FALLBACKS"))) {
        s->fallback_count = provider_build_default_fallback_models(
            resolved, s->fallback_models,
            (int)(sizeof(s->fallback_models) / sizeof(s->fallback_models[0])));
    }
}

/* ── Per-tool metrics ──────────────────────────────────────────────────── */

void tool_metrics_init(tool_metrics_t *m) { memset(m, 0, sizeof(*m)); }

void tool_metrics_record(tool_metrics_t *m, const char *name,
                           bool success, double latency_ms) {
    tool_metric_t *e = NULL;
    for (int i = 0; i < m->count; i++) {
        if (strcmp(m->entries[i].name, name) == 0) { e = &m->entries[i]; break; }
    }
    if (!e && m->count < TOOL_METRICS_MAX) {
        e = &m->entries[m->count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", name);
        e->min_latency_ms = 1e9;
    }
    if (!e) return;
    e->calls++;
    if (success) e->successes++; else e->failures++;
    e->total_latency_ms += latency_ms;
    if (latency_ms > e->max_latency_ms) e->max_latency_ms = latency_ms;
    if (latency_ms < e->min_latency_ms) e->min_latency_ms = latency_ms;
}

const tool_metric_t *tool_metrics_get(tool_metrics_t *m, const char *name) {
    for (int i = 0; i < m->count; i++)
        if (strcmp(m->entries[i].name, name) == 0) return &m->entries[i];
    return NULL;
}

/* ── Tool result cache ─────────────────────────────────────────────────── */

static unsigned cache_fnv(const char *s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

void tool_cache_init(tool_cache_t *c) { memset(c, 0, sizeof(*c)); }

void tool_cache_free(tool_cache_t *c) {
    for (int i = 0; i < c->count; i++) free(c->entries[i].result);
    memset(c, 0, sizeof(*c));
}

static double cache_now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

bool tool_cache_get(tool_cache_t *c, const char *tool, const char *input,
                      char *result, size_t rlen, bool *success) {
    char key[256];
    snprintf(key, sizeof(key), "%s:%u", tool, cache_fnv(input ? input : ""));
    double now = cache_now_sec();
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->entries[i].key, key) == 0) {
            if (c->entries[i].ttl > 0 && (now - c->entries[i].timestamp) > c->entries[i].ttl) {
                free(c->entries[i].result);
                c->entries[i] = c->entries[--c->count];
                c->misses++;
                return false;
            }
            snprintf(result, rlen, "%s", c->entries[i].result ? c->entries[i].result : "");
            *success = c->entries[i].success;
            c->hits++;
            return true;
        }
    }
    c->misses++;
    return false;
}

void tool_cache_put(tool_cache_t *c, const char *tool, const char *input,
                      const char *result, bool success, double ttl) {
    char key[256];
    snprintf(key, sizeof(key), "%s:%u", tool, cache_fnv(input ? input : ""));
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->entries[i].key, key) == 0) {
            free(c->entries[i].result);
            c->entries[i].result = safe_strdup(result);
            c->entries[i].success = success;
            c->entries[i].timestamp = cache_now_sec();
            c->entries[i].ttl = ttl;
            return;
        }
    }
    if (c->count >= TOOL_CACHE_SIZE) {
        int oldest = 0;
        for (int i = 1; i < c->count; i++)
            if (c->entries[i].timestamp < c->entries[oldest].timestamp) oldest = i;
        free(c->entries[oldest].result);
        c->entries[oldest] = c->entries[--c->count];
    }
    tool_cache_entry_t *e = &c->entries[c->count++];
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->result = safe_strdup(result);
    e->success = success;
    e->timestamp = cache_now_sec();
    e->ttl = ttl;
}

/* ── Stream checkpoint (retry resilience) ──────────────────────────────── */

void stream_checkpoint_init(stream_checkpoint_t *cp) {
    memset(cp, 0, sizeof(*cp));
}

void stream_checkpoint_save(stream_checkpoint_t *cp,
                            const content_block_t *blocks, int block_count,
                            const char *partial_text, const char *partial_input,
                            const usage_t *usage, const stream_telemetry_t *telemetry) {
    stream_checkpoint_free(cp);

    if (block_count > 0 && blocks) {
        cp->saved_blocks = safe_malloc(block_count * sizeof(content_block_t));
        for (int i = 0; i < block_count; i++) {
            cp->saved_blocks[i].type = safe_strdup(blocks[i].type);
            cp->saved_blocks[i].text = safe_strdup(blocks[i].text);
            cp->saved_blocks[i].tool_name = safe_strdup(blocks[i].tool_name);
            cp->saved_blocks[i].tool_id = safe_strdup(blocks[i].tool_id);
            cp->saved_blocks[i].tool_input = safe_strdup(blocks[i].tool_input);
        }
        cp->saved_count = block_count;
    }
    cp->partial_text = safe_strdup(partial_text);
    cp->partial_input = safe_strdup(partial_input);
    if (usage) cp->saved_usage = *usage;
    if (telemetry) cp->saved_telemetry = *telemetry;
}

void stream_checkpoint_free(stream_checkpoint_t *cp) {
    for (int i = 0; i < cp->saved_count; i++) {
        free(cp->saved_blocks[i].type);
        free(cp->saved_blocks[i].text);
        free(cp->saved_blocks[i].tool_name);
        free(cp->saved_blocks[i].tool_id);
        free(cp->saved_blocks[i].tool_input);
    }
    free(cp->saved_blocks);
    free(cp->partial_text);
    free(cp->partial_input);
    memset(cp, 0, sizeof(*cp));
}

/* ── Prompt injection detection ────────────────────────────────────────── */

static const char *s_injection_patterns[] = {
    "ignore previous instructions", "ignore all instructions",
    "disregard previous", "forget your instructions",
    "you are now", "new system prompt", "override system",
    "jailbreak", "DAN mode", "developer mode",
    "ignore safety", "bypass restrictions",
    "pretend you are", "act as if you have no",
    "from now on you will",
    "\\n\\nHuman:", "\\n\\nAssistant:",
    "<|im_start|>", "<|endoftext|>", "SYSTEM:",
    NULL
};

static bool contains_case_insensitive(const char *haystack, const char *needle) {
    size_t nlen = needle ? strlen(needle) : 0;
    if (!haystack || !needle || nlen == 0) return false;

    for (const char *p = haystack; *p; p++) {
        size_t j = 0;
        while (j < nlen && p[j]) {
            unsigned char a = (unsigned char)p[j];
            unsigned char b = (unsigned char)needle[j];
            if (tolower(a) != tolower(b)) break;
            j++;
        }
        if (j == nlen) return true;
    }
    return false;
}

injection_level_t detect_prompt_injection(const char *text) {
    if (!text || !text[0]) return INJECTION_NONE;
    int hits = 0;
    for (int i = 0; s_injection_patterns[i]; i++) {
        if (contains_case_insensitive(text, s_injection_patterns[i])) hits++;
    }
    if (hits >= 3) return INJECTION_HIGH;
    if (hits >= 2) return INJECTION_MED;
    if (hits >= 1) return INJECTION_LOW;
    return INJECTION_NONE;
}

const char *llm_get_custom_system_prompt(void) {
    if (g_cheap_mode) return NULL;  /* --cheap: skip workspace prompt */
    return dsco_workspace_prompt();
}

/* ── Debug logging for failed requests ─────────────────────────────────── */

void llm_debug_save_request(const char *request_json, int http_status) {
    char dir_path[512], file_path[560];
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(dir_path, sizeof(dir_path), "%s/.dsco/debug", home);
    mkdir(dir_path, 0755);
    snprintf(file_path, sizeof(file_path), "%s/last_failed_request.json", dir_path);

    FILE *f = fopen(file_path, "w");
    if (!f) return;
    fprintf(f, "%s", request_json);
    fclose(f);
    fprintf(stderr, "  %sdebug: request saved to %s (HTTP %d)%s\n",
            "\033[2m", file_path, http_status, "\033[0m");
}

/* ── Conversation management ───────────────────────────────────────────── */

void conv_init(conversation_t *c) {
    c->cap = 32;
    c->count = 0;
    c->msgs = safe_malloc(c->cap * sizeof(message_t));
    memset(c->msgs, 0, c->cap * sizeof(message_t));
}

void conv_free(conversation_t *c) {
    for (int i = 0; i < c->count; i++) {
        for (int j = 0; j < c->msgs[i].content_count; j++) {
            free(c->msgs[i].content[j].type);
            free(c->msgs[i].content[j].text);
            free(c->msgs[i].content[j].tool_name);
            free(c->msgs[i].content[j].tool_id);
            free(c->msgs[i].content[j].tool_input);
            free(c->msgs[i].content[j].image_media_type);
            free(c->msgs[i].content[j].image_data);
            free(c->msgs[i].content[j].image_url);
            free(c->msgs[i].content[j].doc_media_type);
            free(c->msgs[i].content[j].doc_data);
            free(c->msgs[i].content[j].doc_title);
        }
        free(c->msgs[i].content);
    }
    free(c->msgs);
    c->msgs = NULL;
    c->count = c->cap = 0;
}

static message_t *conv_add(conversation_t *c, msg_role_t role) {
    if (c->count >= c->cap) {
        int new_cap = c->cap > 0 ? c->cap * 2 : 32;
        if (new_cap <= c->cap) new_cap = c->cap + 32;
        c->cap = new_cap;
        c->msgs = safe_realloc(c->msgs, c->cap * sizeof(message_t));
    }
    message_t *m = &c->msgs[c->count++];
    memset(m, 0, sizeof(*m));
    m->role = role;
    return m;
}

static msg_content_t *msg_add_content(message_t *m) {
    m->content = safe_realloc(m->content, (m->content_count + 1) * sizeof(msg_content_t));
    msg_content_t *mc = &m->content[m->content_count++];
    memset(mc, 0, sizeof(*mc));
    return mc;
}

static bool tool_name_is(const msg_content_t *mc, const char *name);
static int tool_result_trim_budget(const msg_content_t *mc, int max_chars);
static char *trim_context_get_batch_result(const char *text, int max_chars);
static bool message_has_tool_use_local(const message_t *m);
static bool message_has_tool_result_local(const message_t *m);
static bool message_is_tool_result_only(const message_t *m);
static int message_sendable_count(const message_t *m);

void conv_pop_last(conversation_t *c) {
    if (c->count <= 0) return;
    c->count--;
    message_t *m = &c->msgs[c->count];
    for (int j = 0; j < m->content_count; j++) {
        free(m->content[j].type);
        free(m->content[j].text);
        free(m->content[j].tool_name);
        free(m->content[j].tool_id);
        free(m->content[j].tool_input);
        free(m->content[j].image_media_type);
        free(m->content[j].image_data);
        free(m->content[j].image_url);
        free(m->content[j].doc_media_type);
        free(m->content[j].doc_data);
        free(m->content[j].doc_title);
    }
    free(m->content);
    memset(m, 0, sizeof(*m));
}

void conv_trim_old_results(conversation_t *c, int keep_recent, int max_chars) {
    if (max_chars <= 0) max_chars = 512;
    int cutoff = c->count - keep_recent;
    if (cutoff <= 0) return;

    for (int i = 0; i < cutoff; i++) {
        message_t *m = &c->msgs[i];
        for (int j = 0; j < m->content_count; j++) {
            msg_content_t *mc = &m->content[j];
            /* Only truncate tool_result text content */
            if (mc->type && strcmp(mc->type, "tool_result") == 0 && mc->text) {
                int effective_max = tool_result_trim_budget(mc, max_chars);
                int len = (int)strlen(mc->text);
                if (len > effective_max) {
                    if (tool_name_is(mc, "context_get_batch")) {
                        char *trimmed = trim_context_get_batch_result(mc->text, effective_max);
                        free(mc->text);
                        mc->text = trimmed;
                        continue;
                    }
                    /* Breadcrumb preservation: keep first line (file path,
                       URL, chunk_id, tool name) before truncating body.
                       Research (Factory.ai 2026) shows losing breadcrumbs
                       forces agents to re-fetch, wasting more tokens. */
                    const char *first_nl = strchr(mc->text, '\n');
                    int first_line_len = first_nl
                        ? (int)(first_nl - mc->text)
                        : (len < 120 ? len : 120);
                    if (first_line_len > 200) first_line_len = 200;

                    /* VFS key preservation: if result has a [key=X] footer,
                       extract and preserve it so model can re-fetch via
                       context_recall without re-executing the tool. */
                    char vfs_key_line[256];
                    vfs_key_line[0] = '\0';
                    const char *key_marker = strstr(mc->text, "key=");
                    if (key_marker) {
                        const char *key_start = key_marker;
                        /* Walk back to find '[' */
                        while (key_start > mc->text && *(key_start - 1) != '[' && *(key_start - 1) != '\n')
                            key_start--;
                        const char *key_end = strchr(key_marker, ']');
                        if (key_end && (key_end - key_start) < 200) {
                            int klen = (int)(key_end - key_start + 1);
                            snprintf(vfs_key_line, sizeof(vfs_key_line),
                                     "\n%.*s", klen, key_start);
                        }
                    }

                    int body_budget = effective_max - first_line_len - 40 - (int)strlen(vfs_key_line);
                    if (body_budget < 80) body_budget = 80;

                    size_t alloc = (size_t)(first_line_len + body_budget + 256);
                    char *trimmed = safe_malloc(alloc);
                    snprintf(trimmed, alloc,
                             "%.*s\n[trimmed %d→%d chars] %.*s%s",
                             first_line_len, mc->text,
                             len, (int)(first_line_len + body_budget),
                             body_budget,
                             mc->text + (first_nl ? first_line_len + 1 : 0),
                             vfs_key_line);
                    free(mc->text);
                    mc->text = trimmed;
                }
            }
        }
    }
}

static bool message_has_tool_use_local(const message_t *m) {
    if (!m) return false;
    for (int j = 0; j < m->content_count; j++) {
        if (m->content[j].type && strcmp(m->content[j].type, "tool_use") == 0)
            return true;
    }
    return false;
}

static bool message_has_tool_result_local(const message_t *m) {
    if (!m) return false;
    for (int j = 0; j < m->content_count; j++) {
        if (m->content[j].type && strcmp(m->content[j].type, "tool_result") == 0)
            return true;
    }
    return false;
}

static bool message_is_tool_result_only(const message_t *m) {
    if (!m || m->role != ROLE_USER || m->content_count <= 0) return false;

    bool saw_tool_result = false;
    for (int j = 0; j < m->content_count; j++) {
        const msg_content_t *mc = &m->content[j];
        if (!mc->type || strcmp(mc->type, "tool_result") != 0) return false;
        saw_tool_result = true;
    }
    return saw_tool_result;
}

bool conv_pop_last_turn(conversation_t *c) {
    if (!c || c->count <= 0) return false;

    bool popped = false;
    while (c->count > 0) {
        message_t *m = &c->msgs[c->count - 1];
        if (m->role == ROLE_ASSISTANT || message_is_tool_result_only(m)) {
            conv_pop_last(c);
            popped = true;
            continue;
        }
        conv_pop_last(c);
        popped = true;
        break;
    }
    return popped;
}

static void msg_clear_content(message_t *m) {
    if (!m) return;
    for (int j = 0; j < m->content_count; j++) {
        free(m->content[j].type);
        free(m->content[j].text);
        free(m->content[j].tool_name);
        free(m->content[j].tool_id);
        free(m->content[j].tool_input);
        free(m->content[j].image_media_type);
        free(m->content[j].image_data);
        free(m->content[j].image_url);
        free(m->content[j].doc_media_type);
        free(m->content[j].doc_data);
        free(m->content[j].doc_title);
    }
    free(m->content);
    m->content = NULL;
    m->content_count = 0;
}

static char *tool_result_compact_preview(const msg_content_t *mc, int max_chars) {
    const char *text = (mc && mc->text) ? mc->text : "";
    int effective_max = tool_result_trim_budget(mc, max_chars > 0 ? max_chars : 512);
    int len = (int)strlen(text);
    if (len <= effective_max) return safe_strdup(text);

    if (tool_name_is(mc, "context_get_batch")) {
        return trim_context_get_batch_result(text, effective_max);
    }

    const char *first_nl = strchr(text, '\n');
    int first_line_len = first_nl ? (int)(first_nl - text) : (len < 120 ? len : 120);
    if (first_line_len > 200) first_line_len = 200;

    int body_budget = effective_max - first_line_len - 40;
    if (body_budget < 80) body_budget = 80;

    size_t alloc = (size_t)(first_line_len + body_budget + 128);
    char *trimmed = safe_malloc(alloc);
    snprintf(trimmed, alloc,
             "%.*s\n[trimmed %d→%d chars] %.*s",
             first_line_len, text,
             len, (int)(first_line_len + body_budget),
             body_budget,
             text + (first_nl ? first_line_len + 1 : 0));
    return trimmed;
}

bool conv_compact_recent_tool_turn(conversation_t *c, int max_chars) {
    if (!c || c->count <= 0) return false;

    int assistant_idx = -1;
    for (int i = c->count - 1; i >= 0; i--) {
        message_t *m = &c->msgs[i];
        if (m->role != ROLE_ASSISTANT) continue;
        if (message_has_tool_use_local(m)) {
            assistant_idx = i;
            break;
        }
        if (message_sendable_count(m) > 0) break;
    }
    if (assistant_idx < 0) return false;

    int result_idx = -1;
    for (int i = assistant_idx + 1; i < c->count; i++) {
        message_t *m = &c->msgs[i];
        if (m->role != ROLE_USER) break;
        if (message_has_tool_result_local(m)) {
            result_idx = i;
            break;
        }
        if (message_sendable_count(m) > 0) break;
    }
    if (result_idx < 0) return false;

    message_t *assistant = &c->msgs[assistant_idx];
    message_t *user = &c->msgs[result_idx];

    jbuf_t assistant_summary;
    jbuf_t user_summary;
    jbuf_init(&assistant_summary, 512);
    jbuf_init(&user_summary, 2048);

    int tool_count = 0;
    for (int j = 0; j < assistant->content_count; j++) {
        msg_content_t *mc = &assistant->content[j];
        if (mc->type && strcmp(mc->type, "text") == 0 && mc->text && mc->text[0]) {
            if (assistant_summary.len > 0) jbuf_append(&assistant_summary, "\n");
            jbuf_append(&assistant_summary, mc->text);
        } else if (mc->type && strcmp(mc->type, "tool_use") == 0) {
            if (tool_count == 0) {
                if (assistant_summary.len > 0) jbuf_append(&assistant_summary, "\n\n");
                jbuf_append(&assistant_summary, "Used tools: ");
            } else {
                jbuf_append(&assistant_summary, ", ");
            }
            jbuf_append(&assistant_summary, mc->tool_name ? mc->tool_name : "tool");
            tool_count++;
        }
    }
    if (assistant_summary.len == 0) {
        if (tool_count > 0) {
            jbuf_append(&assistant_summary, "Used tools to continue the task.");
        } else {
            jbuf_append(&assistant_summary, "Continued the task.");
        }
    }

    bool wrote_result = false;
    for (int j = 0; j < user->content_count; j++) {
        msg_content_t *mc = &user->content[j];
        if (!mc->type) continue;
        if (strcmp(mc->type, "tool_result") == 0) {
            char *preview = tool_result_compact_preview(mc, max_chars > 0 ? max_chars : 768);
            if (wrote_result) jbuf_append(&user_summary, "\n\n");
            jbuf_append(&user_summary, mc->is_error ? "Tool error" : "Tool result");
            if (mc->tool_name && mc->tool_name[0]) {
                jbuf_append(&user_summary, " (");
                jbuf_append(&user_summary, mc->tool_name);
                jbuf_append(&user_summary, ")");
            }
            jbuf_append(&user_summary, ":\n");
            jbuf_append(&user_summary, preview ? preview : "");
            free(preview);
            wrote_result = true;
        } else if (strcmp(mc->type, "text") == 0 && mc->text && mc->text[0]) {
            if (user_summary.len > 0) jbuf_append(&user_summary, "\n\n");
            jbuf_append(&user_summary, mc->text);
        } else {
            jbuf_free(&assistant_summary);
            jbuf_free(&user_summary);
            return false;
        }
    }
    if (user_summary.len == 0) {
        jbuf_free(&assistant_summary);
        jbuf_free(&user_summary);
        return false;
    }

    msg_clear_content(assistant);
    msg_content_t *amc = msg_add_content(assistant);
    amc->type = safe_strdup("text");
    amc->text = assistant_summary.data;

    msg_clear_content(user);
    msg_content_t *umc = msg_add_content(user);
    umc->type = safe_strdup("text");
    umc->text = user_summary.data;

    return true;
}


/* ── Multi-tier compaction system ───────────────────────────────────────
 * Inspired by Claude Code's 4-tier approach:
 *   Micro → Snip → Session → Full
 * with circuit breaker, output-aware thresholds, image stripping,
 * API round grouping, and post-compact file restoration.
 * ─────────────────────────────────────────────────────────────────────── */

void compact_config_init(compact_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->tier = COMPACT_MICRO;
    cfg->max_result_chars = 256;
    cfg->snip_keep_head = SNIP_KEEP_HEAD;
    cfg->snip_keep_tail = SNIP_KEEP_TAIL;
    cfg->session_min_tokens = 10000;
    cfg->session_max_tokens = 40000;
    cfg->full_restore_files = POST_COMPACT_MAX_FILES;
    cfg->full_restore_budget = POST_COMPACT_TOKEN_BUDGET;
    cfg->consecutive_failures = 0;
    cfg->max_failures = COMPACT_CIRCUIT_BREAKER;
}

/* ── Output-aware threshold calculations ────────────────────────────── */

int effective_context_window(session_state_t *s) {
    const model_info_t *mi = model_lookup(s->model);
    int max_output = mi ? mi->max_output : MAX_TOKENS;
    int ctx = s->context_window > 0 ? s->context_window : CONTEXT_WINDOW_TOKENS;
    return ctx - max_output;
}

int auto_compact_threshold(session_state_t *s) {
    return effective_context_window(s) - AUTOCOMPACT_BUFFER_TOKENS;
}

/* ── Token estimation without API call ──────────────────────────────── */

static int rough_token_estimate_content(const msg_content_t *mc) {
    int tokens = 0;
    if (mc->text) tokens += rough_token_estimate(mc->text);
    if (mc->tool_name) tokens += rough_token_estimate(mc->tool_name);
    if (mc->tool_input) tokens += rough_token_estimate(mc->tool_input);
    if (mc->image_data) tokens += IMAGE_TOKEN_ESTIMATE; /* dimension-based, not base64 bytes */
    if (mc->doc_data) tokens += (int)(strlen(mc->doc_data)) / 6;
    return tokens + 10; /* overhead for JSON structure */
}

int conv_rough_estimate(conversation_t *c) {
    if (!c) return 0;
    int tokens = 0;
    for (int i = 0; i < c->count; i++) {
        message_t *m = &c->msgs[i];
        for (int j = 0; j < m->content_count; j++) {
            tokens += rough_token_estimate_content(&m->content[j]);
        }
    }
    return tokens;
}

int conv_token_estimate(conversation_t *c, session_state_t *s) {
    /* IMPORTANT: must reflect what is CURRENTLY in `c`, not cumulative
     * API billing. A previous implementation returned
     * s->total_input_tokens + s->total_output_tokens (lifetime counter) —
     * after compaction this returned the same value as before, producing
     * silent /compact no-ops that still rendered a success checkmark.
     *
     * We rough-estimate `c` directly and add the non-conv overhead
     * (system prompt + tool schemas + cache prefix) measured from the
     * last API response, so threshold checks see the true context size
     * while pre/post deltas still reflect real conversation shrinkage. */
    int rough = conv_rough_estimate(c);
    int overhead = (s && s->non_conv_overhead_tokens > 0)
        ? s->non_conv_overhead_tokens : 0;
    return rough + overhead;
}

/* ── Image/binary stripping ─────────────────────────────────────────── */

void conv_strip_binaries(conversation_t *c, int keep_recent) {
    int cutoff = c->count - keep_recent;
    if (cutoff <= 0) return;

    /* When stripping image/document payloads from old turns we MUST also
     * change the block type — otherwise append_content_block() will emit
     * "{type:image, source:{base64, data:''}}" and every provider rejects
     * it ("image cannot be empty", "Invalid image data-url", etc.) which
     * also kills the fallback chain because each fallback re-sends the
     * same corrupt conversation. Convert stripped blocks into tiny text
     * placeholders so the turn structure survives. */
    for (int i = 0; i < cutoff; i++) {
        message_t *m = &c->msgs[i];
        for (int j = 0; j < m->content_count; j++) {
            msg_content_t *mc = &m->content[j];
            bool stripped = false;
            if (mc->image_data)     { free(mc->image_data);       mc->image_data = NULL;       stripped = true; }
            if (mc->image_url)      { free(mc->image_url);        mc->image_url = NULL;        stripped = true; }
            if (mc->doc_data)       { free(mc->doc_data);         mc->doc_data = NULL;         stripped = true; }
            if (mc->image_media_type){ free(mc->image_media_type); mc->image_media_type = NULL; }
            if (mc->doc_media_type) { free(mc->doc_media_type);   mc->doc_media_type = NULL;   }
            if (mc->doc_title)      { free(mc->doc_title);        mc->doc_title = NULL;        }

            if (stripped && mc->type &&
                (strcmp(mc->type, "image") == 0 ||
                 strcmp(mc->type, "document") == 0)) {
                const char *was = mc->type;
                const char *placeholder = (strcmp(was, "image") == 0)
                    ? "[image elided: older than binary-retention window]"
                    : "[document elided: older than binary-retention window]";
                free(mc->type);
                mc->type = safe_strdup("text");
                free(mc->text);
                mc->text = safe_strdup(placeholder);
            }
        }
    }
}

/* ── API round grouping ─────────────────────────────────────────────── */

int conv_build_rounds(conversation_t *c, api_round_t *rounds, int max_rounds) {
    if (!c || c->count == 0 || !rounds) return 0;

    int round_count = 0;
    int i = 0;

    while (i < c->count && round_count < max_rounds) {
        api_round_t *r = &rounds[round_count];
        memset(r, 0, sizeof(*r));
        r->start_idx = i;
        r->has_tool_use = false;
        r->token_estimate = 0;

        /* A round = user message(s) + assistant response + tool results.
         * It ends when we hit the next user text message (non-tool-result). */

        /* Consume user message */
        if (c->msgs[i].role == ROLE_USER) {
            for (int j = 0; j < c->msgs[i].content_count; j++)
                r->token_estimate += rough_token_estimate_content(&c->msgs[i].content[j]);
            i++;
        }

        /* Consume assistant + tool result pairs */
        while (i < c->count) {
            message_t *m = &c->msgs[i];
            if (m->role == ROLE_ASSISTANT) {
                for (int j = 0; j < m->content_count; j++) {
                    r->token_estimate += rough_token_estimate_content(&m->content[j]);
                    if (m->content[j].type && strcmp(m->content[j].type, "tool_use") == 0)
                        r->has_tool_use = true;
                }
                i++;
            } else if (m->role == ROLE_USER && message_has_tool_result_local(m)) {
                /* Tool result message — part of this round */
                for (int j = 0; j < m->content_count; j++)
                    r->token_estimate += rough_token_estimate_content(&m->content[j]);
                i++;
            } else {
                /* Next user text message — starts a new round */
                break;
            }
        }

        r->end_idx = i - 1;
        if (r->end_idx < r->start_idx) r->end_idx = r->start_idx;
        round_count++;
    }
    return round_count;
}

void conv_drop_rounds(conversation_t *c, api_round_t *rounds,
                      int n_drop, int total_rounds) {
    if (n_drop <= 0 || n_drop >= total_rounds || !c || !rounds) return;

    /* Calculate the first message index to keep */
    int keep_from = rounds[n_drop].start_idx;
    if (keep_from <= 0 || keep_from >= c->count) return;

    /* Free dropped messages */
    for (int i = 0; i < keep_from; i++) {
        message_t *m = &c->msgs[i];
        for (int j = 0; j < m->content_count; j++) {
            free(m->content[j].type);
            free(m->content[j].text);
            free(m->content[j].tool_name);
            free(m->content[j].tool_id);
            free(m->content[j].tool_input);
            free(m->content[j].image_media_type);
            free(m->content[j].image_data);
            free(m->content[j].image_url);
            free(m->content[j].doc_media_type);
            free(m->content[j].doc_data);
            free(m->content[j].doc_title);
            free(m->content[j].cited_text);
        }
        free(m->content);
    }

    /* Shift remaining messages */
    int remaining = c->count - keep_from;
    memmove(&c->msgs[0], &c->msgs[keep_from], remaining * sizeof(message_t));
    c->count = remaining;

    /* Ensure first message is user role (API requirement).
     * If not, insert a synthetic user message. */
    if (c->count > 0 && c->msgs[0].role != ROLE_USER) {
        /* Make room */
        if (c->count + 1 > c->cap) {
            c->cap *= 2;
            c->msgs = safe_realloc(c->msgs, c->cap * sizeof(message_t));
        }
        memmove(&c->msgs[1], &c->msgs[0], c->count * sizeof(message_t));
        c->count++;

        message_t *syn = &c->msgs[0];
        memset(syn, 0, sizeof(*syn));
        syn->role = ROLE_USER;
        syn->content = safe_malloc(sizeof(msg_content_t));
        memset(syn->content, 0, sizeof(msg_content_t));
        syn->content[0].type = safe_strdup("text");
        syn->content[0].text = safe_strdup("[conversation history compacted]");
        syn->content_count = 1;
    }
}

/* ── Post-compact file restoration ──────────────────────────────────── */

void post_compact_restore_init(post_compact_restore_t *r) {
    memset(r, 0, sizeof(*r));
}

void post_compact_restore_free(post_compact_restore_t *r) {
    for (int i = 0; i < r->count; i++) {
        free(r->files[i].content);
    }
    memset(r, 0, sizeof(*r));
}

void post_compact_restore_track(post_compact_restore_t *r,
                                 const char *path, const char *content) {
    if (!r || !path || !content) return;

    /* Check if already tracked — update if so */
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->files[i].path, path) == 0) {
            free(r->files[i].content);
            int chars = (int)strlen(content);
            int cap = POST_COMPACT_PER_FILE_CAP * 4; /* ~4 chars/token */
            if (chars > cap) chars = cap;
            r->files[i].content = safe_malloc(chars + 1);
            memcpy(r->files[i].content, content, chars);
            r->files[i].content[chars] = '\0';
            r->files[i].tokens = rough_token_estimate(r->files[i].content);
            r->files[i].last_read_time = cache_now_sec();
            return;
        }
    }

    /* Evict oldest if at capacity */
    if (r->count >= POST_COMPACT_MAX_FILES) {
        int oldest = 0;
        for (int i = 1; i < r->count; i++) {
            if (r->files[i].last_read_time < r->files[oldest].last_read_time)
                oldest = i;
        }
        free(r->files[oldest].content);
        r->total_tokens -= r->files[oldest].tokens;
        r->files[oldest] = r->files[--r->count];
    }

    /* Add new entry */
    restored_file_t *f = &r->files[r->count];
    snprintf(f->path, sizeof(f->path), "%s", path);
    int chars = (int)strlen(content);
    int cap = POST_COMPACT_PER_FILE_CAP * 4;
    if (chars > cap) chars = cap;
    f->content = safe_malloc(chars + 1);
    memcpy(f->content, content, chars);
    f->content[chars] = '\0';
    f->tokens = rough_token_estimate(f->content);
    f->last_read_time = cache_now_sec();
    r->total_tokens += f->tokens;
    r->count++;
}

void post_compact_restore_inject(post_compact_restore_t *r,
                                  conversation_t *c) {
    if (!r || r->count == 0 || !c) return;

    /* Build a synthetic message with restored file contents */
    jbuf_t buf;
    jbuf_init(&buf, 4096);
    jbuf_append(&buf, "[Context restored after compaction]\n");

    int budget = POST_COMPACT_TOKEN_BUDGET;
    int injected = 0;

    for (int i = 0; i < r->count && budget > 0; i++) {
        restored_file_t *f = &r->files[i];
        if (f->tokens > budget) continue;

        jbuf_appendf(&buf, "\n--- %s ---\n", f->path);
        jbuf_append(&buf, f->content);
        jbuf_append(&buf, "\n");
        budget -= f->tokens;
        injected++;
    }

    if (injected > 0) {
        /* Add as a user message so it appears in context */
        conv_add_user_text(c, buf.data);
    }
    jbuf_free(&buf);
}

/* ── Deferred tool catalog ──────────────────────────────────────────── */

char *tools_build_deferred_catalog(const char **paged_names, int paged_count,
                                    int *out_deferred_count) {
    int total_count = 0;
    const tool_def_t *all_tools = tools_get_all(&total_count);
    if (!all_tools || total_count == 0) {
        if (out_deferred_count) *out_deferred_count = 0;
        return safe_strdup("");
    }

    jbuf_t buf;
    jbuf_init(&buf, 2048);
    jbuf_append(&buf, "Additional tools available (use discover_tools to load):\n");
    int deferred = 0;

    for (int i = 0; i < total_count; i++) {
        const tool_def_t *t = &all_tools[i];
        if (!t->name) continue;

        /* Skip if already paged in */
        bool is_paged = false;
        for (int j = 0; j < paged_count; j++) {
            if (paged_names[j] && strcmp(paged_names[j], t->name) == 0) {
                is_paged = true;
                break;
            }
        }
        if (is_paged) continue;

        /* Emit name + first sentence of description only */
        jbuf_appendf(&buf, "  - %s", t->name);
        if (t->description) {
            /* Extract first sentence (up to first period or 80 chars) */
            const char *desc = t->description;
            int dlen = (int)strlen(desc);
            const char *dot = strchr(desc, '.');
            int slen = dot ? (int)(dot - desc + 1) : (dlen < 80 ? dlen : 80);
            jbuf_appendf(&buf, ": %.*s", slen, desc);
        }
        jbuf_append(&buf, "\n");
        deferred++;
    }

    if (out_deferred_count) *out_deferred_count = deferred;
    return buf.data;
}

/* ── Tiered auto-compact pipeline ───────────────────────────────────── */

compact_result_t conv_auto_compact(conversation_t *c, session_state_t *s,
                                    compact_config_t *cfg) {
    compact_result_t result;
    memset(&result, 0, sizeof(result));

    double start = cache_now_sec();
    int threshold = auto_compact_threshold(s);
    int current_tokens = conv_token_estimate(c, s);
    result.pre_token_count = current_tokens;

    /* Circuit breaker: stop trying after N consecutive failures */
    if (cfg->consecutive_failures >= cfg->max_failures) {
        result.tier_used = COMPACT_MICRO;
        result.post_token_count = current_tokens;
        return result;
    }

    /* Below threshold — no compaction needed */
    if (current_tokens < threshold) {
        result.post_token_count = current_tokens;
        return result;
    }

    /* ── Tier 0: Micro-compact (strip binaries + trim results) ───── */
    result.tier_used = COMPACT_MICRO;
    int before_count = c->count;

    /* Always strip binaries from old messages first */
    conv_strip_binaries(c, cfg->snip_keep_tail);

    /* Trim old tool results */
    conv_trim_old_results(c, cfg->snip_keep_tail, cfg->max_result_chars);

    /* Re-estimate */
    int post_micro = conv_token_estimate(c, s);
    if (post_micro < threshold) {
        result.post_token_count = post_micro;
        result.messages_removed = before_count - c->count;
        result.messages_kept = c->count;
        result.duration_ms = (cache_now_sec() - start) * 1000.0;
        cfg->consecutive_failures = 0;
        return result;
    }

    /* ── Tier 1: Snip-compact (drop middle API rounds) ───────────── */
    result.tier_used = COMPACT_SNIP;
    api_round_t rounds[MAX_API_ROUNDS];
    int round_count = conv_build_rounds(c, rounds, MAX_API_ROUNDS);

    /* Adapt reserve windows: when we are above threshold and the static
     * keep_head + keep_tail leaves nothing to drop, shrink the tail first
     * (most recent context is most valuable, but we'd rather lose some
     * mid-recency than fail entirely) then the head, until at least one
     * round is droppable. This keeps /compact from being a silent no-op
     * on shorter conversations. Floors: head>=1 (the seed user turn
     * usually anchors the system prompt's frame), tail>=2 (last user
     * message + assistant response). */
    int keep_head = cfg->snip_keep_head;
    int keep_tail = cfg->snip_keep_tail;
    while (round_count <= keep_head + keep_tail && (keep_tail > 2 || keep_head > 1)) {
        if (keep_tail > 2) keep_tail--;
        else if (keep_head > 1) keep_head--;
    }

    if (round_count > (keep_head + keep_tail)) {
        /* How many rounds to drop? Target: get below threshold.
         * Drop from the middle, keeping head and tail. */
        int droppable = round_count - keep_head - keep_tail;
        int tokens_to_shed = post_micro - (threshold * 3 / 4); /* target 75% */

        int to_drop = 0;
        int tokens_dropped = 0;
        for (int i = keep_head;
             i < round_count - keep_tail && tokens_dropped < tokens_to_shed;
             i++) {
            tokens_dropped += rounds[i].token_estimate;
            to_drop++;
        }

        /* Guarantee progress: if we're above threshold we MUST drop at
         * least one round, even if the token-target math comes out to 0
         * (small rounds, large overhead). */
        if (to_drop == 0 && droppable > 0) {
            tokens_dropped = rounds[keep_head].token_estimate;
            to_drop = 1;
        }

        if (to_drop > 0 && to_drop <= droppable) {
            /* Rewrite: keep head rounds, skip middle, keep tail rounds.
             * We do this by freeing middle messages and shifting. */
            int drop_start = rounds[keep_head].start_idx;
            int drop_end = rounds[keep_head + to_drop - 1].end_idx;

            /* Boundary requirements: keep_head>=1 guarantees drop_start>0
             * (the first user turn is preserved); keep_tail>=2 guarantees
             * the tail extends to c->count-1 so drop_end < c->count-1. */
            if (drop_start > 0 && drop_end < c->count - 1) {
                /* Free middle messages */
                for (int i = drop_start; i <= drop_end; i++) {
                    message_t *m = &c->msgs[i];
                    for (int j = 0; j < m->content_count; j++) {
                        free(m->content[j].type);
                        free(m->content[j].text);
                        free(m->content[j].tool_name);
                        free(m->content[j].tool_id);
                        free(m->content[j].tool_input);
                        free(m->content[j].image_media_type);
                        free(m->content[j].image_data);
                        free(m->content[j].image_url);
                        free(m->content[j].doc_media_type);
                        free(m->content[j].doc_data);
                        free(m->content[j].doc_title);
                        free(m->content[j].cited_text);
                    }
                    free(m->content);
                }

                /* Insert a synthetic marker at drop_start */
                int tail_count = c->count - drop_end - 1;
                int new_count = drop_start + 1 + tail_count; /* +1 for marker */

                /* Shift tail messages to right after marker */
                memmove(&c->msgs[drop_start + 1],
                        &c->msgs[drop_end + 1],
                        tail_count * sizeof(message_t));
                c->count = new_count;

                /* Write synthetic marker */
                message_t *marker = &c->msgs[drop_start];
                memset(marker, 0, sizeof(*marker));

                /* Determine correct role: must alternate with previous */
                if (drop_start > 0 && c->msgs[drop_start - 1].role == ROLE_USER)
                    marker->role = ROLE_ASSISTANT;
                else
                    marker->role = ROLE_USER;

                marker->content = safe_malloc(sizeof(msg_content_t));
                memset(marker->content, 0, sizeof(msg_content_t));
                marker->content[0].type = safe_strdup("text");

                char snip_msg[256];
                snprintf(snip_msg, sizeof(snip_msg),
                         "[%d conversation rounds compacted — %dk tokens freed]",
                         to_drop, tokens_dropped / 1000);
                marker->content[0].text = safe_strdup(snip_msg);
                marker->content_count = 1;

                /* Ensure role alternation after marker */
                if (drop_start + 1 < c->count &&
                    c->msgs[drop_start + 1].role == marker->role) {
                    /* Need another synthetic message to bridge */
                    if (c->count + 1 <= c->cap ||
                        (c->cap *= 2, c->msgs = safe_realloc(c->msgs, c->cap * sizeof(message_t)), 1)) {
                        memmove(&c->msgs[drop_start + 2],
                                &c->msgs[drop_start + 1],
                                (c->count - drop_start - 1) * sizeof(message_t));
                        c->count++;

                        message_t *bridge = &c->msgs[drop_start + 1];
                        memset(bridge, 0, sizeof(*bridge));
                        bridge->role = (marker->role == ROLE_USER) ? ROLE_ASSISTANT : ROLE_USER;
                        bridge->content = safe_malloc(sizeof(msg_content_t));
                        memset(bridge->content, 0, sizeof(msg_content_t));
                        bridge->content[0].type = safe_strdup("text");
                        bridge->content[0].text = safe_strdup("Understood, continuing.");
                        bridge->content_count = 1;
                    }
                }
            }
        }
    }

    int post_snip = conv_token_estimate(c, s);
    result.post_token_count = post_snip;
    result.messages_removed = before_count - c->count;
    result.messages_kept = c->count;
    result.duration_ms = (cache_now_sec() - start) * 1000.0;

    if (post_snip < threshold) {
        cfg->consecutive_failures = 0;
    } else {
        cfg->consecutive_failures++;
    }

    return result;
}

/* ── Session save/load ─────────────────────────────────────────────────── */

bool conv_save_ex(conversation_t *c, const session_state_t *session, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "{");
    if (session) {
        jbuf_t sb;
        jbuf_init(&sb, 512);
        jbuf_append(&sb, "\"session\":{");
        jbuf_append(&sb, "\"trust_tier\":");
        jbuf_append_json_str(&sb, session_trust_tier_to_string(session->trust_tier));
        if (session->active_skill[0]) {
            jbuf_append(&sb, ",\"active_skill\":");
            jbuf_append_json_str(&sb, session->active_skill);
        }
        if (session->active_topology[0]) {
            jbuf_append(&sb, ",\"active_topology\":");
            jbuf_append_json_str(&sb, session->active_topology);
        }
        jbuf_append(&sb, ",\"topology_auto\":");
        jbuf_append(&sb, session->topology_auto ? "true" : "false");
        if (session->turn_count > 0) {
            jbuf_appendf(&sb, ",\"turn_count\":%d", session->turn_count);
        }
        if (fabs(session->tool_budget_ratio - 1.0f) > 0.0001f) {
            jbuf_appendf(&sb, ",\"tool_budget_ratio\":%.6f", session->tool_budget_ratio);
        }
        if (session->pin_text[0]) {
            jbuf_append(&sb, ",\"pin_text\":");
            jbuf_append_json_str(&sb, session->pin_text);
        }
        if (session->model[0]) {
            jbuf_append(&sb, ",\"model\":");
            jbuf_append_json_str(&sb, session->model);
        }
        if (session->slot_name[0]) {
            jbuf_append(&sb, ",\"slot_name\":");
            jbuf_append_json_str(&sb, session->slot_name);
        }
        jbuf_append(&sb, "},");
        fwrite(sb.data, 1, sb.len, f);
        jbuf_free(&sb);
    }
    fprintf(f, "\"messages\":[\n");
    for (int i = 0; i < c->count; i++) {
        if (i > 0) fprintf(f, ",\n");
        message_t *m = &c->msgs[i];
        fprintf(f, "{\"role\":\"%s\",\"content\":[",
                m->role == ROLE_USER ? "user" : "assistant");
        for (int j = 0; j < m->content_count; j++) {
            if (j > 0) fprintf(f, ",");
            msg_content_t *mc = &m->content[j];
            /* Use jbuf for proper JSON escaping */
            jbuf_t b;
            jbuf_init(&b, 1024);
            jbuf_append(&b, "{\"type\":");
            jbuf_append_json_str(&b, mc->type ? mc->type : "text");
            if (mc->text) {
                jbuf_append(&b, ",\"text\":");
                jbuf_append_json_str(&b, mc->text);
            }
            if (mc->tool_name) {
                jbuf_append(&b, ",\"tool_name\":");
                jbuf_append_json_str(&b, mc->tool_name);
            }
            if (mc->tool_id) {
                jbuf_append(&b, ",\"tool_id\":");
                jbuf_append_json_str(&b, mc->tool_id);
            }
            if (mc->tool_input) {
                jbuf_append(&b, ",\"tool_input\":");
                jbuf_append_json_str(&b, mc->tool_input);
            }
            if (mc->is_error) {
                jbuf_append(&b, ",\"is_error\":true");
            }
            jbuf_append(&b, "}");
            fwrite(b.data, 1, b.len, f);
            jbuf_free(&b);
        }
        fprintf(f, "]}");
    }
    fprintf(f, "\n]}\n");
    fclose(f);
    return true;
}

bool conv_save(conversation_t *c, const char *path) {
    return conv_save_ex(c, NULL, path);
}

bool conv_load_ex(conversation_t *c, session_state_t *session, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > 10 * 1024 * 1024) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    char *data = safe_malloc(fsize + 1);
    size_t nread = fread(data, 1, fsize, f);
    data[nread] = '\0';
    fclose(f);

    if (session) {
        char *session_raw = json_get_raw(data, "session");
        if (session_raw) {
            char *tier = json_get_str(session_raw, "trust_tier");
            if (tier) {
                bool ok = false;
                dsco_trust_tier_t parsed = session_trust_tier_from_string(tier, &ok);
                if (ok) session->trust_tier = parsed;
                free(tier);
            }
            char *skill = json_get_str(session_raw, "active_skill");
            if (skill) {
                snprintf(session->active_skill, sizeof(session->active_skill), "%s", skill);
                free(skill);
            }
            char *topology = json_get_str(session_raw, "active_topology");
            if (topology) {
                snprintf(session->active_topology, sizeof(session->active_topology), "%s", topology);
                free(topology);
            }
            session->topology_auto = json_get_bool(session_raw, "topology_auto", false);
            int saved_turn_count = json_get_int(session_raw, "turn_count", session->turn_count);
            if (saved_turn_count >= 0) session->turn_count = saved_turn_count;
            double saved_budget_ratio = json_get_double(session_raw, "tool_budget_ratio",
                                                        session->tool_budget_ratio);
            if (saved_budget_ratio >= 0.0 && saved_budget_ratio <= 1.0)
                session->tool_budget_ratio = (float)saved_budget_ratio;
            char *pin_text = json_get_str(session_raw, "pin_text");
            if (pin_text) {
                snprintf(session->pin_text, sizeof(session->pin_text), "%s", pin_text);
                free(pin_text);
            }
            char *saved_model = json_get_str(session_raw, "model");
            if (saved_model && saved_model[0])
                snprintf(session->model, sizeof(session->model), "%s", saved_model);
            free(saved_model);
            char *saved_slot = json_get_str(session_raw, "slot_name");
            if (saved_slot && saved_slot[0])
                snprintf(session->slot_name, sizeof(session->slot_name), "%s", saved_slot);
            free(saved_slot);
            free(session_raw);
        }
    }

    /* Clear existing conversation */
    conv_free(c);
    conv_init(c);

    /* Simple JSON parser: find each message object.
       We look for {"role":"...", "content":[...]} patterns.
       For robustness we parse role and content blocks manually. */
    char *messages_raw = json_get_raw(data, "messages");
    if (!messages_raw) { free(data); return false; }
    const char *p = messages_raw;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '[') {
        free(messages_raw);
        free(data);
        return false;
    }
    p++; /* skip '[' */

    /* Skip a quoted JSON string, honoring escapes and never stepping
       past the terminating NUL on malformed input. */
    #define SKIP_JSON_STRING(ptr)                                \
        do {                                                     \
            if (*(ptr) == '"') {                                \
                (ptr)++;                                         \
                while (*(ptr)) {                                 \
                    if (*(ptr) == '\\') {                       \
                        if ((ptr)[1]) { (ptr) += 2; continue; } \
                        (ptr)++;                                 \
                        break;                                   \
                    }                                            \
                    if (*(ptr) == '"') { (ptr)++; break; }      \
                    (ptr)++;                                     \
                }                                                \
            }                                                    \
        } while (0)

    while (*p) {
        /* Skip whitespace and commas */
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ','))
            p++;
        if (*p == ']') break;  /* end of messages array */
        if (*p != '{') { p++; continue; }

        /* We're at a message object. Extract role. */
        char *role_str = json_get_str(p, "role");
        if (!role_str) {
            /* Skip this malformed object - find matching '}' */
            int depth = 1; p++;
            while (*p && depth > 0) {
                if (*p == '{') depth++;
                else if (*p == '}') depth--;
                else if (*p == '"') { SKIP_JSON_STRING(p); continue; }
                p++;
            }
            continue;
        }

        msg_role_t role = (strcmp(role_str, "user") == 0) ? ROLE_USER : ROLE_ASSISTANT;
        free(role_str);

        /* Find the "content" array within this message */
        const char *content_start = json_get_raw(p, "content");
        if (content_start && *content_start == '[') {
            /* Add a new message */
            message_t *m = NULL;
            if (c->count >= c->cap) {
                /* Use conv_add helper via a role-based add */
            }
            /* Directly grow the message array */
            if (c->count >= c->cap) {
                int new_cap = c->cap > 0 ? c->cap * 2 : 32;
                if (new_cap <= c->cap) new_cap = c->cap + 32;
                c->cap = new_cap;
                c->msgs = safe_realloc(c->msgs, c->cap * sizeof(message_t));
            }
            m = &c->msgs[c->count++];
            memset(m, 0, sizeof(*m));
            m->role = role;

            /* Parse content blocks inside the array */
            const char *cp = content_start + 1; /* skip '[' */
            while (*cp) {
                while (*cp && (*cp == ' ' || *cp == '\t' || *cp == '\n' || *cp == '\r' || *cp == ','))
                    cp++;
                if (*cp == ']') break;
                if (*cp != '{') { cp++; continue; }

                /* Parse this content block */
                char *ctype = json_get_str(cp, "type");
                char *ctext = json_get_str(cp, "text");
                char *ctool_name = json_get_str(cp, "tool_name");
                char *ctool_id = json_get_str(cp, "tool_id");
                char *ctool_input = json_get_str(cp, "tool_input");
                bool cis_error = json_get_bool(cp, "is_error", false);

                /* Add content to message */
                m->content = safe_realloc(m->content,
                                           (m->content_count + 1) * sizeof(msg_content_t));
                msg_content_t *mc = &m->content[m->content_count++];
                memset(mc, 0, sizeof(*mc));
                mc->type = ctype ? ctype : safe_strdup("text");
                mc->text = ctext;
                mc->tool_name = ctool_name;
                mc->tool_id = ctool_id;
                mc->tool_input = ctool_input;
                mc->is_error = cis_error;

                /* Skip to end of this content block object */
                int depth = 1; cp++;
                while (*cp && depth > 0) {
                    if (*cp == '{') depth++;
                    else if (*cp == '}') depth--;
                    else if (*cp == '"') { SKIP_JSON_STRING(cp); continue; }
                    cp++;
                }
            }
        }

        /* Skip to end of this message object */
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') { SKIP_JSON_STRING(p); continue; }
            p++;
        }
    }

    #undef SKIP_JSON_STRING

    free(messages_raw);
    free(data);
    return true;
}

bool conv_load(conversation_t *c, const char *path) {
    return conv_load_ex(c, NULL, path);
}
void conv_add_user_text(conversation_t *c, const char *text) {
    message_t *m = conv_add(c, ROLE_USER);
    msg_content_t *mc = msg_add_content(m);
    mc->type = safe_strdup("text");
    mc->text = safe_strdup(text);
}

void conv_add_assistant_text(conversation_t *c, const char *text) {
    message_t *m = conv_add(c, ROLE_ASSISTANT);
    msg_content_t *mc = msg_add_content(m);
    mc->type = safe_strdup("text");
    mc->text = safe_strdup(text);
}

void conv_add_assistant_tool_use(conversation_t *c, const char *tool_id,
                                  const char *tool_name, const char *tool_input) {
    message_t *m = conv_add(c, ROLE_ASSISTANT);
    msg_content_t *mc = msg_add_content(m);
    mc->type = safe_strdup("tool_use");
    mc->tool_id = safe_strdup(tool_id);
    mc->tool_name = safe_strdup(tool_name);
    mc->tool_input = tool_input ? safe_strdup(tool_input) : safe_strdup("{}");
}

void conv_add_tool_result_named(conversation_t *c, const char *tool_id,
                                const char *tool_name,
                                const char *result, bool is_error) {
    /* Reuse the last message if it's already a user message with tool_result
       content — multiple tool results for the same turn must be combined
       into a single user message to satisfy the alternating-role requirement. */
    message_t *m = NULL;
    if (c->count > 0 && c->msgs[c->count - 1].role == ROLE_USER &&
        c->msgs[c->count - 1].content_count > 0 &&
        c->msgs[c->count - 1].content[0].type &&
        strcmp(c->msgs[c->count - 1].content[0].type, "tool_result") == 0) {
        m = &c->msgs[c->count - 1];
    } else {
        m = conv_add(c, ROLE_USER);
    }
    msg_content_t *mc = msg_add_content(m);
    mc->type = safe_strdup("tool_result");
    mc->tool_id = safe_strdup(tool_id);
    if (tool_name && *tool_name) mc->tool_name = safe_strdup(tool_name);
    mc->text = safe_strdup(result ? result : "");
    dsco_strip_terminal_controls_inplace(mc->text);
    mc->is_error = is_error;
}

void conv_add_tool_result(conversation_t *c, const char *tool_id,
                          const char *result, bool is_error) {
    conv_add_tool_result_named(c, tool_id, NULL, result, is_error);
}

static bool tool_name_is(const msg_content_t *mc, const char *name) {
    return mc && mc->tool_name && name && strcmp(mc->tool_name, name) == 0;
}

static int tool_result_trim_budget(const msg_content_t *mc, int max_chars) {
    (void)mc;  /* legacy context tool special budgets removed */
    return max_chars;
}

static void append_trimmed_preview(jbuf_t *b, const char *text, int limit) {
    if (!b || !text || limit <= 0) return;
    int used = 0;
    bool prev_space = false;
    for (const char *p = text; *p && used < limit; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!prev_space && used > 0 && used < limit) {
                jbuf_append_char(b, ' ');
                used++;
            }
            prev_space = true;
            continue;
        }
        if (c < 32) continue;
        jbuf_append_char(b, (char)c);
        used++;
        prev_space = false;
    }
}

static char *trim_context_get_batch_result(const char *text, int max_chars) {
    if (!text) return safe_strdup("");

    size_t original_len = strlen(text);
    jbuf_t out;
    jbuf_init(&out, (size_t)max_chars + 256);

    const char *p = text;
    int chunk_count = 0;
    while ((p = strstr(p, "[chunk_id=")) != NULL) {
        const char *hdr_end = strchr(p, '\n');
        if (!hdr_end) break;

        if (out.len > 0) jbuf_append(&out, "\n");
        jbuf_append_len(&out, p, (size_t)(hdr_end - p));
        jbuf_append_char(&out, '\n');

        const char *body = hdr_end + 1;
        const char *next = strstr(body, "\n---\n[chunk_id=");
        const char *batch_tail = strstr(body, "\n\n--- batch:");
        const char *end = next ? next : (batch_tail ? batch_tail : text + original_len);
        append_trimmed_preview(&out, body, 140);
        jbuf_append_char(&out, '\n');

        chunk_count++;
        if (out.len >= (size_t)(max_chars - 220)) break;
        p = end;
    }

    const char *footer = strstr(text, "--- batch:");
    if (footer && out.len < (size_t)(max_chars - 96)) {
        jbuf_append_char(&out, '\n');
        jbuf_append(&out, footer);
    }

    if (chunk_count == 0) {
        int copy = max_chars < 240 ? max_chars : 240;
        append_trimmed_preview(&out, text, copy);
    }

    jbuf_appendf(&out, "\n[trimmed %zu→%zu chars]", original_len, out.len);
    return out.data;
}

void conv_ensure_tool_results(conversation_t *c) {
    /* Scan for tool_use blocks in assistant messages that have no matching
       tool_result in the subsequent user message.  Insert synthetic error
       results for any orphans.  This prevents HTTP 400 from the API. */
    for (int i = 0; i < c->count; i++) {
        message_t *m = &c->msgs[i];
        if (m->role != ROLE_ASSISTANT) continue;

        /* Collect tool_use ids from this assistant message */
        for (int j = 0; j < m->content_count; j++) {
            msg_content_t *mc = &m->content[j];
            if (!mc->type || strcmp(mc->type, "tool_use") != 0 || !mc->tool_id)
                continue;

            /* Search subsequent user messages for a matching tool_result */
            bool found = false;
            for (int k = i + 1; k < c->count && !found; k++) {
                message_t *um = &c->msgs[k];
                if (um->role != ROLE_USER) break; /* stop at next assistant msg */
                for (int l = 0; l < um->content_count; l++) {
                    if (um->content[l].type &&
                        strcmp(um->content[l].type, "tool_result") == 0 &&
                        um->content[l].tool_id &&
                        strcmp(um->content[l].tool_id, mc->tool_id) == 0) {
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                conv_add_tool_result_named(c, mc->tool_id, mc->tool_name,
                                           "tool result missing (session interrupted)", true);
            }
        }
    }
}

void conv_add_assistant_raw(conversation_t *c, parsed_response_t *resp) {
    message_t *m = NULL;
    for (int i = 0; i < resp->count; i++) {
        const char *type = resp->blocks[i].type;
        /* Anthropic requires a signature when replaying thinking blocks.
           We currently stream/display thinking, but do not persist or replay it. */
        if (type && strcmp(type, "thinking") == 0) {
            continue;
        }
        if (!m) m = conv_add(c, ROLE_ASSISTANT);
        msg_content_t *mc = msg_add_content(m);
        mc->type = safe_strdup(type);
        if (resp->blocks[i].text) mc->text = safe_strdup(resp->blocks[i].text);
        if (resp->blocks[i].tool_name) mc->tool_name = safe_strdup(resp->blocks[i].tool_name);
        if (resp->blocks[i].tool_id) mc->tool_id = safe_strdup(resp->blocks[i].tool_id);
        if (resp->blocks[i].tool_input) mc->tool_input = safe_strdup(resp->blocks[i].tool_input);
    }
}

void conv_add_user_image_base64(conversation_t *c, const char *media_type,
                                 const char *base64_data, const char *text) {
    /* Refuse to inject empty base64 — every provider rejects it and that
     * corrupts the whole conversation (fallback chain re-sends it and
     * every fallback ALSO fails). Degrade gracefully to a text-only turn
     * so the user gets a clear signal instead of a silent API failure. */
    if (!base64_data || !base64_data[0]) {
        message_t *m = conv_add(c, ROLE_USER);
        msg_content_t *tc = msg_add_content(m);
        tc->type = safe_strdup("text");
        if (text && text[0]) {
            char buf[512];
            snprintf(buf, sizeof(buf),
                     "[image skipped: empty base64 payload] %s", text);
            tc->text = safe_strdup(buf);
        } else {
            tc->text = safe_strdup("[image skipped: empty base64 payload]");
        }
        return;
    }
    message_t *m = conv_add(c, ROLE_USER);
    /* Image block */
    msg_content_t *mc = msg_add_content(m);
    mc->type = safe_strdup("image");
    mc->image_media_type = safe_strdup(media_type);
    mc->image_data = safe_strdup(base64_data);
    /* Optional text block */
    if (text && text[0]) {
        msg_content_t *tc = msg_add_content(m);
        tc->type = safe_strdup("text");
        tc->text = safe_strdup(text);
    }
}

void conv_add_user_image_url(conversation_t *c, const char *url, const char *text) {
    message_t *m = conv_add(c, ROLE_USER);
    msg_content_t *mc = msg_add_content(m);
    mc->type = safe_strdup("image");
    mc->image_url = safe_strdup(url);
    if (text && text[0]) {
        msg_content_t *tc = msg_add_content(m);
        tc->type = safe_strdup("text");
        tc->text = safe_strdup(text);
    }
}

void conv_add_user_document(conversation_t *c, const char *media_type,
                             const char *base64_data, const char *title,
                             const char *text) {
    message_t *m = conv_add(c, ROLE_USER);
    /* Document block */
    msg_content_t *mc = msg_add_content(m);
    mc->type = safe_strdup("document");
    mc->doc_media_type = safe_strdup(media_type);
    mc->doc_data = safe_strdup(base64_data);
    if (title && title[0]) mc->doc_title = safe_strdup(title);
    /* Optional text block */
    if (text && text[0]) {
        msg_content_t *tc = msg_add_content(m);
        tc->type = safe_strdup("text");
        tc->text = safe_strdup(text);
    }
}

/* ── Build JSON request ────────────────────────────────────────────────── */

static void append_content_block(jbuf_t *b, msg_content_t *mc,
                                 bool claude_code_oauth);

static const char *skip_json_ws(const char *s) {
    while (s && *s && isspace((unsigned char)*s)) s++;
    return s;
}

static bool json_compound_is_complete(const char *s, char open_ch, char close_ch) {
    const char *p = skip_json_ws(s);
    if (!p || *p != open_ch) return false;

    int depth = 0;
    bool in_str = false;
    for (; *p; p++) {
        char ch = *p;
        if (in_str) {
            if (ch == '\\' && p[1]) {
                p++;
                continue;
            }
            if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') {
            in_str = true;
            continue;
        }
        if (ch == open_ch) {
            depth++;
            continue;
        }
        if (ch == close_ch) {
            depth--;
            if (depth == 0) {
                p++;
                p = skip_json_ws(p);
                return *p == '\0';
            }
            if (depth < 0) return false;
        }
    }
    return false;
}

static char *normalize_server_result_content(const char *candidate, bool require_array) {
    const char *p = skip_json_ws(candidate);
    if (!p || !*p) return NULL;

    if (*p == '[') {
        if (!json_compound_is_complete(p, '[', ']')) return NULL;
        return safe_strdup(p);
    }
    if (*p == '{') {
        if (!json_compound_is_complete(p, '{', '}')) return NULL;
        if (!require_array) return safe_strdup(p);
        jbuf_t wrapped;
        jbuf_init(&wrapped, strlen(p) + 4);
        jbuf_append(&wrapped, "[");
        jbuf_append(&wrapped, p);
        jbuf_append(&wrapped, "]");
        return wrapped.data;
    }

    return NULL;
}

static char *extract_server_result_content_raw(const msg_content_t *mc, bool require_array) {
    if (!mc) return NULL;

    if (mc->tool_input && mc->tool_input[0]) {
        const char *tool_input = skip_json_ws(mc->tool_input);
        if (tool_input && *tool_input == '{') {
            char *raw = json_get_raw(tool_input, "content");
            if (raw) {
                char *normalized = normalize_server_result_content(raw, require_array);
                free(raw);
                if (normalized) return normalized;
            }
        }

        char *direct = normalize_server_result_content(tool_input, require_array);
        if (direct) return direct;
    }

    if (mc->text && mc->text[0]) {
        const char *text = skip_json_ws(mc->text);
        if (text && *text == '{') {
            char *raw = json_get_raw(text, "content");
            if (raw) {
                char *normalized = normalize_server_result_content(raw, require_array);
                free(raw);
                if (normalized) return normalized;
            }
        }

        char *direct = normalize_server_result_content(text, require_array);
        if (direct) return direct;
    }

    return NULL;
}

static bool content_block_is_sendable(const msg_content_t *mc) {
    if (!mc || !mc->type) return false;
    /* Do not replay assistant thinking blocks: signature handling is not persisted. */
    if (strcmp(mc->type, "thinking") == 0) return false;
    /* Server-side tool types are managed by the API — pass through */
    if (strcmp(mc->type, "server_tool_use") == 0) return true;
    if (strcmp(mc->type, "web_search_tool_result") == 0) return true;
    if (strcmp(mc->type, "code_execution_tool_result") == 0) return true;
    return true;
}

static bool content_block_is_server_side(const msg_content_t *mc) {
    if (!mc || !mc->type) return false;
    if (strcmp(mc->type, "server_tool_use") == 0) return true;
    if (strcmp(mc->type, "web_search_tool_result") == 0) return true;
    if (strcmp(mc->type, "code_execution_tool_result") == 0) return true;
    return false;
}

static int message_sendable_count(const message_t *m) {
    int count = 0;
    for (int i = 0; i < m->content_count; i++) {
        if (content_block_is_sendable(&m->content[i])) count++;
    }
    return count;
}

/* Backward-compatibility: older dsco versions persisted server tool result
   blocks as user messages. Promote those message roles at serialization time. */
static int message_effective_role(const message_t *m) {
    if (!m) return ROLE_USER;
    if (m->role != ROLE_USER) return (int)m->role;

    bool saw_server_side = false;
    for (int i = 0; i < m->content_count; i++) {
        const msg_content_t *mc = &m->content[i];
        if (!content_block_is_sendable(mc)) continue;
        if (content_block_is_server_side(mc)) {
            saw_server_side = true;
            continue;
        }
        return ROLE_USER;
    }
    return saw_server_side ? ROLE_ASSISTANT : ROLE_USER;
}

static const char *anthropic_oauth_wire_tool_name(const char *name,
                                                  char *buf,
                                                  size_t buf_len,
                                                  bool claude_code_oauth) {
    if (!name) return "";
    if (!claude_code_oauth) return name;
    if (dsco_mcp_is_canonical_tool_name(name)) return name;
    if (strncmp(name, "mcp_", 4) == 0 && buf && buf_len > 0) {
        for (int i = 0; i < g_external_tool_count; i++) {
            const char *candidate = g_external_tools[i].name;
            if (!dsco_mcp_is_canonical_tool_name(candidate)) continue;
            char legacy[128];
            dsco_mcp_legacy_alias_from_canonical(candidate, legacy, sizeof(legacy));
            if (strcmp(legacy, name) == 0)
                return candidate;
        }
        snprintf(buf, buf_len, "mcp__%s", name + 4);
        return buf;
    }
    return name;
}

static void append_message_sendable_content(jbuf_t *b, message_t *m,
                                            bool force_leading_comma,
                                            bool claude_code_oauth) {
    int written = 0;
    for (int i = 0; i < m->content_count; i++) {
        msg_content_t *mc = &m->content[i];
        if (!content_block_is_sendable(mc)) continue;
        if (force_leading_comma || written > 0) jbuf_append(b, ",");
        append_content_block(b, mc, claude_code_oauth);
        written++;
    }
}

static void append_content_block(jbuf_t *b, msg_content_t *mc,
                                 bool claude_code_oauth) {
    if (strcmp(mc->type, "text") == 0) {
        jbuf_append(b, "{\"type\":\"text\",\"text\":");
        jbuf_append_json_str(b, mc->text ? mc->text : "");
        jbuf_append(b, "}");
    } else if (strcmp(mc->type, "tool_use") == 0) {
        char wire_name[128];
        jbuf_append(b, "{\"type\":\"tool_use\",\"id\":");
        jbuf_append_json_str(b, mc->tool_id);
        jbuf_append(b, ",\"name\":");
        jbuf_append_json_str(b, anthropic_oauth_wire_tool_name(
            mc->tool_name, wire_name, sizeof(wire_name), claude_code_oauth));
        jbuf_append(b, ",\"input\":");
        /* Validate tool_input is a JSON object before embedding raw */
        bool valid_json = false;
        if (mc->tool_input && mc->tool_input[0] == '{') {
            /* Quick validation: matching braces */
            int depth = 0;
            const char *p = mc->tool_input;
            bool in_str = false;
            for (; *p; p++) {
                if (in_str) {
                    if (*p == '\\' && p[1]) { p++; continue; }
                    if (*p == '"') in_str = false;
                } else {
                    if (*p == '"') in_str = true;
                    else if (*p == '{') depth++;
                    else if (*p == '}') depth--;
                }
            }
            valid_json = (depth == 0 && !in_str);
        }
        if (valid_json) {
            jbuf_append(b, mc->tool_input);
        } else {
            jbuf_append(b, "{}");
        }
        jbuf_append(b, "}");
    } else if (strcmp(mc->type, "thinking") == 0) {
        jbuf_append(b, "{\"type\":\"thinking\",\"thinking\":");
        jbuf_append_json_str(b, mc->text ? mc->text : "");
        jbuf_append(b, "}");
    } else if (strcmp(mc->type, "image") == 0) {
        /* Defensive: if the block has neither a URL nor real base64 data,
         * emit a text placeholder instead of a broken image block. Every
         * provider (Anthropic, xAI, OpenAI, Google, OpenRouter) rejects
         * empty base64, which also blocks the fallback chain. */
        bool has_url  = (mc->image_url && mc->image_url[0]);
        bool has_data = (mc->image_data && mc->image_data[0]);
        if (!has_url && !has_data) {
            jbuf_append(b, "{\"type\":\"text\",\"text\":");
            jbuf_append_json_str(b, "[image omitted: empty payload]");
            jbuf_append(b, "}");
        } else {
            jbuf_append(b, "{\"type\":\"image\",\"source\":{");
            if (has_url) {
                jbuf_append(b, "\"type\":\"url\",\"url\":");
                jbuf_append_json_str(b, mc->image_url);
            } else {
                jbuf_append(b, "\"type\":\"base64\",\"media_type\":");
                jbuf_append_json_str(b, mc->image_media_type ? mc->image_media_type : "image/png");
                jbuf_append(b, ",\"data\":");
                jbuf_append_json_str(b, mc->image_data);
            }
            jbuf_append(b, "}}");
        }
    } else if (strcmp(mc->type, "document") == 0) {
        /* Same defense as image: skip empty documents with a text placeholder. */
        bool has_doc = (mc->doc_data && mc->doc_data[0]);
        if (!has_doc) {
            jbuf_append(b, "{\"type\":\"text\",\"text\":");
            jbuf_append_json_str(b, "[document omitted: empty payload]");
            jbuf_append(b, "}");
        } else {
            jbuf_append(b, "{\"type\":\"document\",\"source\":{\"type\":\"base64\",\"media_type\":");
            jbuf_append_json_str(b, mc->doc_media_type ? mc->doc_media_type : "application/pdf");
            jbuf_append(b, ",\"data\":");
            jbuf_append_json_str(b, mc->doc_data);
            jbuf_append(b, "}");
            if (mc->doc_title) {
                jbuf_append(b, ",\"title\":");
                jbuf_append_json_str(b, mc->doc_title);
            }
            jbuf_append(b, "}");
        }
    } else if (strcmp(mc->type, "tool_result") == 0) {
        jbuf_append(b, "{\"type\":\"tool_result\",\"tool_use_id\":");
        jbuf_append_json_str(b, mc->tool_id);
        if (mc->is_error) jbuf_append(b, ",\"is_error\":true");
        jbuf_append(b, ",\"content\":");
        jbuf_append_json_str(b, mc->text ? mc->text : "");
        jbuf_append(b, "}");
    } else if (strcmp(mc->type, "server_tool_use") == 0) {
        /* Server-side tool_use blocks — validate input JSON before embedding */
        jbuf_append(b, "{\"type\":\"server_tool_use\",\"id\":");
        jbuf_append_json_str(b, mc->tool_id ? mc->tool_id : "");
        jbuf_append(b, ",\"name\":");
        jbuf_append_json_str(b, mc->tool_name ? mc->tool_name : "");
        jbuf_append(b, ",\"input\":");
        /* Validate JSON before embedding raw */
        bool valid = false;
        if (mc->tool_input && mc->tool_input[0] == '{') {
            int d = 0; bool in_s = false;
            for (const char *p = mc->tool_input; *p; p++) {
                if (in_s) { if (*p == '\\' && p[1]) p++; else if (*p == '"') in_s = false; }
                else { if (*p == '"') in_s = true; else if (*p == '{') d++; else if (*p == '}') d--; }
            }
            valid = (d == 0 && !in_s);
        }
        jbuf_append(b, valid ? mc->tool_input : "{}");
        jbuf_append(b, "}");
    } else if (strcmp(mc->type, "web_search_tool_result") == 0 ||
               strcmp(mc->type, "code_execution_tool_result") == 0) {
        /* Server-side tool results require structured content (list/object). */
        bool require_array = (strcmp(mc->type, "web_search_tool_result") == 0);
        bool require_object = (strcmp(mc->type, "code_execution_tool_result") == 0);
        if (!mc->tool_id || !mc->tool_id[0]) {
            jbuf_append(b, "{\"type\":\"text\",\"text\":");
            jbuf_append_json_str(
                b,
                mc->text ? mc->text : "[server tool result omitted: missing tool_use_id]"
            );
            jbuf_append(b, "}");
            return;
        }
        jbuf_append(b, "{\"type\":");
        jbuf_append_json_str(b, mc->type);
        jbuf_append(b, ",\"tool_use_id\":");
        jbuf_append_json_str(b, mc->tool_id ? mc->tool_id : "");
        jbuf_append(b, ",\"content\":");
        char *raw_content = extract_server_result_content_raw(mc, require_array);
        const char *raw_p = raw_content ? skip_json_ws(raw_content) : NULL;
        if (raw_content &&
            ((require_array && raw_p && *raw_p == '[') ||
             (require_object && raw_p && *raw_p == '{') ||
             (!require_array && !require_object && raw_p &&
              (*raw_p == '{' || *raw_p == '[')))) {
            jbuf_append(b, raw_content);
            free(raw_content);
        } else {
            /* Fallback that preserves schema validity when no structured
               content could be recovered from persisted history. */
            if (raw_content) free(raw_content);
            if (require_object) {
                jbuf_append(b,
                            "{\"type\":\"code_execution_tool_result_error\","
                            "\"error_code\":\"unavailable\"}");
            } else {
                jbuf_append(b, "[]");
            }
        }
        jbuf_append(b, "}");
    } else {
        /* Unknown type — emit as text to avoid breaking JSON structure */
        jbuf_append(b, "{\"type\":\"text\",\"text\":");
        jbuf_append_json_str(b, mc->text ? mc->text : "");
        jbuf_append(b, "}");
    }
}

/* Default max tools sent per Anthropic request (override: DSCO_MAX_TOOLS).
 * Capped at 32 — bash+python core, everything else loaded dynamically. */
#define ANTHROPIC_DEFAULT_MAX_TOOLS 32

/* ── Compact tool catalog (lazy singleton) ──────────────────────────── */

static char *s_compact_catalog = NULL;

static const char *get_compact_catalog(void) {
    if (!s_compact_catalog) {
        s_compact_catalog = tools_build_compact_catalog();
    }
    return s_compact_catalog;
}

/* Append a single tool definition to the JSON buffer */
static void append_one_tool(jbuf_t *b, const tool_def_t *t, bool cache_mark,
                            bool claude_code_oauth) {
    char wire_name[128];
    jbuf_append(b, "{\"name\":");
    jbuf_append_json_str(b, anthropic_oauth_wire_tool_name(
        t->name, wire_name, sizeof(wire_name), claude_code_oauth));
    jbuf_append(b, ",\"description\":");
    jbuf_append_json_str(b, t->description);
    jbuf_append(b, ",\"input_schema\":");
    jbuf_append(b, t->input_schema_json);
    if (cache_mark)
        jbuf_append(b, ",\"cache_control\":{\"type\":\"ephemeral\"}");
    jbuf_append(b, "}");
}

/* Append filtered tools — tiered, cache-aware serialization.
 *
 * Layout:  [Tier 0: pinned] [Tier 1: working] [cache break] [Tier 2: discovery]
 *
 * Tier 0+1 are stable across turns → prompt-cached (78% savings).
 * Tier 2 is volatile each turn → cheap to recompute.
 * Cache breakpoint (ephemeral marker) goes on the last Tier 1 tool. */
static void append_tools_json_filtered(jbuf_t *b, session_state_t *session,
                                        conversation_t *conv,
                                        bool claude_code_oauth) {
    int max_tools = ANTHROPIC_DEFAULT_MAX_TOOLS;
    const char *mt_env = getenv("DSCO_MAX_TOOLS");
    if (mt_env && mt_env[0]) {
        int v = atoi(mt_env);
        if (v > 0) max_tools = v;
    }

    /* Rolling context window: concatenate last 3 user messages for richer
     * retrieval signal. More context → better embedding/TF-IDF scores.
     * Single-message retrieval misses multi-turn intent (e.g. user says
     * "now deploy it" after discussing Kubernetes for 3 turns). */
    char ctx_buf[4096] = "";
    const char *ctx = NULL;
    if (conv) {
        int found = 0;
        int pos = 0;
        for (int i = conv->count - 1; i >= 0 && found < 3; i--) {
            if (conv->msgs[i].role == ROLE_USER) {
                for (int j = 0; j < conv->msgs[i].content_count; j++) {
                    if (conv->msgs[i].content[j].text) {
                        int len = (int)strlen(conv->msgs[i].content[j].text);
                        int avail = (int)sizeof(ctx_buf) - pos - 2;
                        if (avail <= 0) break;
                        if (pos > 0) ctx_buf[pos++] = ' ';
                        int copy = len < avail ? len : avail;
                        memcpy(ctx_buf + pos, conv->msgs[i].content[j].text, copy);
                        pos += copy;
                        ctx_buf[pos] = '\0';
                        found++;
                        break;
                    }
                }
            }
        }
        if (pos > 0) ctx = ctx_buf;
    }

    float budget_ratio = session ? session->tool_budget_ratio : 1.0f;
    tool_page_result_t paged = tools_get_paged(ctx, max_tools, budget_ratio);

    bool has_server_tools = session && (session->web_search || session->code_execution);
    bool has_after_working = (paged.discovery_count > 0 ||
                              g_external_tool_count > 0 || has_server_tools);
    bool has_after_discovery = (g_external_tool_count > 0 || has_server_tools);

    jbuf_append(b, ",\"tools\":[");
    int written = 0;

    /* Tier 0: Pinned — stable core tools (first position = high attention) */
    for (int i = 0; i < paged.pinned_count; i++) {
        if (written > 0) jbuf_append(b, ",");
        bool mark = (i == paged.pinned_count - 1) &&
                    (paged.working_count == 0) && !has_after_working;
        append_one_tool(b, paged.pinned[i], mark, claude_code_oauth);
        written++;
    }

    /* Tier 1: Working set — slow-evolving (hot + cooc + centroid) */
    for (int i = 0; i < paged.working_count; i++) {
        if (written > 0) jbuf_append(b, ",");
        /* Cache breakpoint: mark last working-set tool if volatile content follows */
        bool mark = (i == paged.working_count - 1) &&
                    (has_after_working
                         ? true  /* cache break before volatile tier */
                         : true  /* end of tools → final ephemeral */);
        append_one_tool(b, paged.working[i], mark, claude_code_oauth);
        written++;
    }

    /* Tier 2: Discovery — now sends REAL schemas (not progressive stubs).
     * The ~200 tokens/tool cost (8 tools × 200 = 1600 tokens) is negligible
     * compared to the cost of failed tool calls from missing schemas.
     * The compact catalog in the system prompt provides full tool awareness. */
    for (int i = 0; i < paged.discovery_count; i++) {
        if (written > 0) jbuf_append(b, ",");
        bool mark = (i == paged.discovery_count - 1) && !has_after_discovery;
        append_one_tool(b, paged.discovery[i], mark, claude_code_oauth);
        written++;
    }

    tool_page_result_free(&paged);

    /* External tools (MCP, etc.) — loaded tools win first.  The old behavior
     * serialized the first N MCP tools by discovery order, which made
     * load_tools useless for large servers like email/heat. */
    int ext_budget = max_tools - written;
    int loaded_ext_count = 0;
    for (int i = 0; i < g_external_tool_count; i++)
        if (g_external_tools[i].loaded) loaded_ext_count++;
    if (loaded_ext_count > ext_budget) ext_budget = loaded_ext_count;
    if (ext_budget > 24) ext_budget = 24;   /* keep requests bounded */
    if (ext_budget < 0) ext_budget = 0;
    int ext_written = 0;
    for (int pass = 0; pass < 2 && ext_written < ext_budget; pass++) {
        bool want_loaded = (pass == 0);
        for (int i = 0; i < g_external_tool_count && ext_written < ext_budget; i++) {
            if ((bool)g_external_tools[i].loaded != want_loaded) continue;
            if (written > 0) jbuf_append(b, ",");
            char wire_name[128];
            jbuf_append(b, "{\"name\":");
            jbuf_append_json_str(b, anthropic_oauth_wire_tool_name(
                g_external_tools[i].name, wire_name, sizeof(wire_name),
                claude_code_oauth));
            jbuf_append(b, ",\"description\":");
            jbuf_append_json_str(b, g_external_tools[i].description);
            jbuf_append(b, ",\"input_schema\":");
            jbuf_append(b, g_external_tools[i].input_schema_json);
            if (ext_written == ext_budget - 1 && !has_server_tools) {
                jbuf_append(b, ",\"cache_control\":{\"type\":\"ephemeral\"}");
            }
            jbuf_append(b, "}");
            written++;
            ext_written++;
        }
    }

    /* Server-side tools */
    if (session && session->web_search) {
        if (written > 0) jbuf_append(b, ",");
        jbuf_append(b, "{\"type\":\"web_search_20250305\",\"name\":\"web_search\",\"max_uses\":5}");
    }
    if (session && session->code_execution) {
        if (written > 0) jbuf_append(b, ",");
        jbuf_append(b, "{\"type\":\"code_execution_20250522\",\"name\":\"code_execution\"");
        if (session->container_id[0]) {
            jbuf_append(b, ",\"container_id\":");
            jbuf_append_json_str(b, session->container_id);
        }
        jbuf_append(b, "}");
    }
    jbuf_append(b, "]");
}


static void build_messages_json(jbuf_t *b, conversation_t *c, session_state_t *session,
                                bool claude_code_oauth) {
    /* Ensure every tool_use has a matching tool_result before serialization */
    conv_ensure_tool_results(c);
    jbuf_append(b, ",\"messages\":[");
    int msg_written = 0;
    int last_written_role = -1;
    for (int i = 0; i < c->count; i++) {
        message_t *m = &c->msgs[i];
        if (message_sendable_count(m) == 0) continue;
        int role = message_effective_role(m);

        /* If same role as previous, merge content into previous message.
           The API requires strict role alternation (user/assistant/user/...).
           We do this safely by closing and reopening — no buffer backup tricks. */
        if (msg_written > 0 && last_written_role == role) {
            /* Back up over the closing ]} to append more content */
            if (b->len >= 2 && b->data[b->len-1] == '}' && b->data[b->len-2] == ']') {
                b->len -= 2;
                b->data[b->len] = '\0';
                append_message_sendable_content(b, m, true, claude_code_oauth);
                jbuf_append(b, "]}");
            } else {
                /* Fallback: emit as separate message (may cause API error
                   but won't corrupt JSON) */
                jbuf_append(b, ",");
                jbuf_append(b, "{\"role\":");
                jbuf_append_json_str(b, role == ROLE_USER ? "user" : "assistant");
                jbuf_append(b, ",\"content\":[");
                append_message_sendable_content(b, m, false, claude_code_oauth);
                jbuf_append(b, "]}");
                msg_written++;
            }
        } else {
            if (msg_written > 0) jbuf_append(b, ",");
            jbuf_append(b, "{\"role\":");
            jbuf_append_json_str(b, role == ROLE_USER ? "user" : "assistant");
            jbuf_append(b, ",\"content\":[");
            append_message_sendable_content(b, m, false, claude_code_oauth);
            jbuf_append(b, "]}");
            msg_written++;
        }
        last_written_role = role;
    }

    /* Budget-adaptive pressure (BAVT 2026): as budget depletes,
       nudge the model toward exploitation over exploration.
       Injected as trailing user context to avoid breaking cache prefix. */
    if (session && session->total_input_tokens > 0) {
        const model_info_t *bmi = model_lookup(session->model);
        if (bmi) {
            double cost = session->total_input_tokens  * bmi->input_price / 1e6
                        + session->total_output_tokens * bmi->output_price / 1e6
                        + session->total_cache_read_tokens * bmi->cache_read_price / 1e6;
            extern double g_cost_budget;
            if (g_cost_budget > 0 && cost > g_cost_budget * 0.6) {
                double pct = 100.0 * cost / g_cost_budget;
                char pressure[256];
                if (pct >= 85.0)
                    snprintf(pressure, sizeof(pressure),
                             "[Budget: %.0f%% used ($%.2f/$%.2f). "
                             "Synthesize your answer now from gathered evidence. "
                             "Do not make additional tool calls unless critical.]",
                             pct, cost, g_cost_budget);
                else
                    snprintf(pressure, sizeof(pressure),
                             "[Budget: %.0f%% used ($%.2f/$%.2f). "
                             "Prefer concise tool calls. Batch with context_get_batch.]",
                             pct, cost, g_cost_budget);

                /* Append as trailing user message if last was assistant,
                   or merge into last user message */
                if (last_written_role == ROLE_USER && b->len >= 2 &&
                    b->data[b->len-1] == '}' && b->data[b->len-2] == ']') {
                    b->len -= 2;
                    b->data[b->len] = '\0';
                    jbuf_append(b, ",{\"type\":\"text\",\"text\":");
                    jbuf_append_json_str(b, pressure);
                    jbuf_append(b, "}]}");
                } else {
                    jbuf_append(b, ",{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":");
                    jbuf_append_json_str(b, pressure);
                    jbuf_append(b, "}]}");
                }
            }
        }
    }

    /* Response prefilling */
    if (session && session->prefill[0]) {
        jbuf_append(b, ",{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":");
        jbuf_append_json_str(b, session->prefill);
        jbuf_append(b, "}]}");
    }

    jbuf_append(b, "]");
    (void)last_written_role;
}

static bool env_truthy_local(const char *value) {
    return value && (value[0] == '1' || strcasecmp(value, "true") == 0 ||
                     strcasecmp(value, "yes") == 0);
}

bool llm_anthropic_uses_claude_code_auth(const char *credential) {
    const char *forced = getenv("DSCO_FORCE_CLAUDE_CODE_AUTH");
    if (env_truthy_local(forced)) return true;
    return credential && strncmp(credential, "sk-ant-oat", 10) == 0;
}

static const char *claude_code_entrypoint(void) {
    const char *entrypoint = getenv("DSCO_CLAUDE_CODE_ENTRYPOINT");
    if (entrypoint && entrypoint[0]) return entrypoint;
    entrypoint = getenv("CLAUDE_CODE_ENTRYPOINT");
    if (entrypoint && entrypoint[0]) return entrypoint;
    return "cli";
}

static const char *claude_code_version(void) {
    const char *override = getenv("DSCO_CLAUDE_CODE_VERSION");
    if (override && override[0]) return override;

    override = getenv("CLAUDE_CODE_VERSION");
    if (override && override[0]) return override;

    static bool loaded = false;
    static char version[32];

    if (loaded) return version[0] ? version : CLAUDE_CODE_VERSION_FALLBACK;
    loaded = true;

    FILE *fp = popen("claude --version 2>/dev/null", "r");
    if (!fp) return CLAUDE_CODE_VERSION_FALLBACK;

    char line[128];
    if (fgets(line, sizeof(line), fp)) {
        size_t n = 0;
        while (line[n] &&
               ((line[n] >= '0' && line[n] <= '9') || line[n] == '.' || line[n] == '-')) {
            if (n + 1 >= sizeof(version)) break;
            version[n] = line[n];
            n++;
        }
        version[n] = '\0';
    }
    pclose(fp);

    return version[0] ? version : CLAUDE_CODE_VERSION_FALLBACK;
}

static const char *conv_first_user_text(conversation_t *c) {
    static const char empty[] = "";
    if (!c) return empty;

    for (int i = 0; i < c->count; i++) {
        message_t *msg = &c->msgs[i];
        if (msg->role != ROLE_USER) continue;
        for (int j = 0; j < msg->content_count; j++) {
            msg_content_t *mc = &msg->content[j];
            if (mc->type && strcmp(mc->type, "text") == 0 && mc->text)
                return mc->text;
        }
        return empty;
    }

    return empty;
}

static void build_claude_code_billing_header(conversation_t *c,
                                             char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';

    const char *message_text = conv_first_user_text(c);
    size_t message_len = strlen(message_text);
    char cch_hex[65];
    sha256_hex((const uint8_t *)message_text, message_len, cch_hex);

    char sampled[4];
    const int idxs[3] = {4, 7, 20};
    for (int i = 0; i < 3; i++) {
        int idx = idxs[i];
        sampled[i] = ((size_t)idx < message_len) ? message_text[idx] : '0';
    }
    sampled[3] = '\0';

    const char *version = claude_code_version();
    char fingerprint_input[128];
    snprintf(fingerprint_input, sizeof(fingerprint_input), "%s%s%s",
             CLAUDE_CODE_BILLING_SALT, sampled, version);
    char version_hex[65];
    sha256_hex((const uint8_t *)fingerprint_input, strlen(fingerprint_input), version_hex);

    snprintf(out, out_len,
             "x-anthropic-billing-header: cc_version=%s.%.3s; cc_entrypoint=%s; cch=%.5s;",
             version, version_hex, claude_code_entrypoint(), cch_hex);
}

static bool append_claude_code_billing_system_block(jbuf_t *b,
                                                    conversation_t *c,
                                                    const char *credential) {
    if (!llm_anthropic_uses_claude_code_auth(credential)) return false;

    char header[256];
    build_claude_code_billing_header(c, header, sizeof(header));
    if (!header[0]) return false;

    jbuf_append(b, "{\"type\":\"text\",\"text\":");
    jbuf_append_json_str(b, header);
    jbuf_append(b, "}");
    return true;
}

char *llm_build_request_for_credential(conversation_t *c, const char *model,
                                       int max_tokens, const char *credential) {
    jbuf_t b;
    jbuf_init(&b, 16384);
    bool claude_code_oauth = llm_anthropic_uses_claude_code_auth(credential);

    jbuf_append(&b, "{\"model\":");
    jbuf_append_json_str(&b, model);
    jbuf_append(&b, ",\"max_tokens\":");
    jbuf_append_int(&b, max_tokens);
    jbuf_append(&b, ",\"stream\":true");

    /* System prompt with compact tool catalog + cache breakpoint */
    const char *custom = llm_get_custom_system_prompt();
    const char *catalog = g_cheap_mode ? NULL : get_compact_catalog();
    jbuf_append(&b, ",\"system\":[");
    if (append_claude_code_billing_system_block(&b, c, credential))
        jbuf_append(&b, ",");
    if (custom) {
        jbuf_append(&b, "{\"type\":\"text\",\"text\":");
        jbuf_append_json_str(&b, custom);
        jbuf_append(&b, "},");
    }
    jbuf_append(&b, "{\"type\":\"text\",\"text\":");
    jbuf_append_json_str(&b, g_cheap_mode ? SYSTEM_PROMPT_CHEAP : SYSTEM_PROMPT);
    if (catalog) {
        /* Catalog is stable → include in cached prefix */
        jbuf_append(&b, "},");
        jbuf_append(&b, "{\"type\":\"text\",\"text\":");
        jbuf_append_json_str(&b, catalog);
        jbuf_append(&b, ",\"cache_control\":{\"type\":\"ephemeral\"}}");
    } else {
        jbuf_append(&b, ",\"cache_control\":{\"type\":\"ephemeral\"}}");
    }
    jbuf_append(&b, "]");

    /* Adaptive thinking — only on Opus 4.6 and Sonnet 4.6 */
    if (strstr(model, "opus-4-6") || strstr(model, "sonnet-4-6")) {
        jbuf_append(&b, ",\"thinking\":{\"type\":\"adaptive\"}");
    }

    append_tools_json_filtered(&b, NULL, c, claude_code_oauth);
    build_messages_json(&b, c, NULL, claude_code_oauth);
    jbuf_append(&b, "}");

    return b.data;
}

char *llm_build_request(conversation_t *c, const char *model, int max_tokens) {
    return llm_build_request_for_credential(c, model, max_tokens, NULL);
}

char *llm_build_request_ex_for_credential(conversation_t *c,
                                          session_state_t *session,
                                          int max_tokens,
                                          const char *credential) {
    if (!session) return llm_build_request_for_credential(c, DEFAULT_MODEL,
                                                          max_tokens, credential);

    jbuf_t b;
    jbuf_init(&b, 16384);
    bool claude_code_oauth = llm_anthropic_uses_claude_code_auth(credential);

    jbuf_append(&b, "{\"model\":");
    jbuf_append_json_str(&b, session->model);
    jbuf_append(&b, ",\"max_tokens\":");
    /* Phase 4: auto-escalation — use override if set, else default */
    int effective_max_tokens = (session->max_output_override > 0)
                              ? session->max_output_override : max_tokens;
    jbuf_append_int(&b, effective_max_tokens);
    jbuf_append(&b, ",\"stream\":true");

    /* System prompt — cache-aware ordering (Anthropic prompt caching
       requires static prefix before dynamic content for cache hits).
       Order: SYSTEM_PROMPT (static) → TOOL CATALOG (static, cache break) → dynamic */
    const char *custom = llm_get_custom_system_prompt();
    const char *catalog = g_cheap_mode ? NULL : get_compact_catalog();
    char *active_skill_prompt = NULL;
    bool has_dynamic = (custom != NULL) || session->active_skill[0];

    jbuf_append(&b, ",\"system\":[");
    if (append_claude_code_billing_system_block(&b, c, credential))
        jbuf_append(&b, ",");

    /* Block 1: Static system prompt (cheap mode gets a leaner one) */
    jbuf_append(&b, "{\"type\":\"text\",\"text\":");
    jbuf_append_json_str(&b, g_cheap_mode ? SYSTEM_PROMPT_CHEAP : SYSTEM_PROMPT);

    if (catalog) {
        jbuf_append(&b, "},");
        /* Block 2: Compact tool catalog — stable across turns, cacheable.
         * Gives the model full awareness of all tools with signatures,
         * so it can call any tool correctly on first attempt. */
        jbuf_append(&b, "{\"type\":\"text\",\"text\":");
        jbuf_append_json_str(&b, catalog);
    }

    /* Deferred catalog removed — the compact TOOL INDEX in block 2 already
     * lists all tool names by group. The model uses discover_tools/load_tools
     * to page in full schemas on demand. This saves ~5-15k tokens/request
     * that were previously wasted on redundant name+description lines. */

    /* Cache breakpoint on last static block */
    if (!has_dynamic) {
        jbuf_append(&b, ",\"cache_control\":{\"type\":\"ephemeral\"}}");
    } else {
        jbuf_append(&b, ",\"cache_control\":{\"type\":\"ephemeral\"}},");
    }

    /* Block 3+: Dynamic content (workspace prompt, skills) — after cache break */
    if (custom) {
        jbuf_append(&b, "{\"type\":\"text\",\"text\":");
        jbuf_append_json_str(&b, custom);
        jbuf_append(&b, "}");
        if (session->active_skill[0]) jbuf_append(&b, ",");
    }
    if (session->active_skill[0]) {
        const char *skill = dsco_workspace_skill_prompt(session->active_skill);
        if (skill && *skill) {
            size_t n = strlen(skill) + strlen(session->active_skill) + 32;
            active_skill_prompt = safe_malloc(n);
            snprintf(active_skill_prompt, n, "[Active Skill: %s]\n%s",
                     session->active_skill, skill);
            jbuf_append(&b, "{\"type\":\"text\",\"text\":");
            jbuf_append_json_str(&b, active_skill_prompt);
            jbuf_append(&b, "}");
        }
    }

    /* Phase 3: Inject recalled memories as system block */
    if (session->memory_context[0]) {
        jbuf_append(&b, ",{\"type\":\"text\",\"text\":");
        jbuf_append_json_str(&b, session->memory_context);
        jbuf_append(&b, "}");
    }

    jbuf_append(&b, "]");

    /* Sampling parameters */
    if (session->temperature >= 0) {
        char tbuf[32]; snprintf(tbuf, sizeof(tbuf), ",\"temperature\":%.2f", session->temperature);
        jbuf_append(&b, tbuf);
    }
    if (session->top_p >= 0) {
        char tbuf[32]; snprintf(tbuf, sizeof(tbuf), ",\"top_p\":%.2f", session->top_p);
        jbuf_append(&b, tbuf);
    }
    if (session->top_k > 0) {
        jbuf_append(&b, ",\"top_k\":");
        jbuf_append_int(&b, session->top_k);
    }

    /* Adaptive thinking — gate on model support */
    const model_info_t *mi = model_lookup(session->model);
    if (mi && mi->supports_thinking) {
        if (session->thinking_budget > 0) {
            jbuf_append(&b, ",\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":");
            jbuf_append_int(&b, session->thinking_budget);
            jbuf_append(&b, "}");
        } else {
            jbuf_append(&b, ",\"thinking\":{\"type\":\"adaptive\"}");
        }
    }

    /* Effort parameter */
    if (session->effort[0] && strcmp(session->effort, "high") != 0) {
        jbuf_append(&b, ",\"output_config\":{\"effort\":");
        jbuf_append_json_str(&b, session->effort);
        jbuf_append(&b, "}");
    }

    /* Stop sequences */
    if (session->stop_seq[0]) {
        jbuf_append(&b, ",\"stop_sequences\":[");
        jbuf_append_json_str(&b, session->stop_seq);
        jbuf_append(&b, "]");
    }

    append_tools_json_filtered(&b, session, c, claude_code_oauth);

    /* Tool choice control */
    if (session->tool_choice[0]) {
        if (strcmp(session->tool_choice, "any") == 0) {
            jbuf_append(&b, ",\"tool_choice\":{\"type\":\"any\"}");
        } else if (strncmp(session->tool_choice, "tool:", 5) == 0) {
            char wire_name[128];
            jbuf_append(&b, ",\"tool_choice\":{\"type\":\"tool\",\"name\":");
            jbuf_append_json_str(&b, anthropic_oauth_wire_tool_name(
                session->tool_choice + 5, wire_name, sizeof(wire_name),
                claude_code_oauth));
            jbuf_append(&b, "}");
        } else if (strcmp(session->tool_choice, "none") == 0) {
            jbuf_append(&b, ",\"tool_choice\":{\"type\":\"none\"}");
        }
        /* "auto" is the default — don't send it */
    }

    build_messages_json(&b, c, session, claude_code_oauth);
    jbuf_append(&b, "}");

    /* Reset single-shot options after use */
    if (session->prefill[0]) session->prefill[0] = '\0';
    if (session->stop_seq[0]) session->stop_seq[0] = '\0';
    free(active_skill_prompt);

    return b.data;
}

char *llm_build_request_ex(conversation_t *c, session_state_t *session, int max_tokens) {
    return llm_build_request_ex_for_credential(c, session, max_tokens, NULL);
}

/* ── Token counting endpoint ───────────────────────────────────────────── */

static size_t count_tokens_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    jbuf_append_len((jbuf_t *)userdata, (const char *)ptr, total);
    return total;
}

int llm_count_tokens(const char *api_key, const char *request_json) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    char auth[512];
    if (llm_anthropic_uses_claude_code_auth(api_key))
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    else
        snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
    hdrs = curl_slist_append(hdrs, auth);
    char ver[128];
    snprintf(ver, sizeof(ver), "anthropic-version: %s", ANTHROPIC_VERSION);
    hdrs = curl_slist_append(hdrs, ver);
    char beta[256];
    if (llm_anthropic_uses_claude_code_auth(api_key))
        snprintf(beta, sizeof(beta), "anthropic-beta: %s,%s",
                 CLAUDE_CODE_OAUTH_BETA, ANTHROPIC_BETAS);
    else
        snprintf(beta, sizeof(beta), "anthropic-beta: %s", ANTHROPIC_BETAS);
    hdrs = curl_slist_append(hdrs, beta);

    jbuf_t resp_buf;
    jbuf_init(&resp_buf, 4096);

    curl_easy_setopt(curl, CURLOPT_URL, API_URL_COUNT_TOKENS);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, count_tokens_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_buf);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    int tokens = -1;
    if (res == CURLE_OK && http_code == 200 && resp_buf.data) {
        tokens = json_get_int(resp_buf.data, "input_tokens", -1);
    }
    jbuf_free(&resp_buf);
    return tokens;
}


/* ── SSE streaming state machine ───────────────────────────────────────── */

typedef struct {
    /* Accumulated content blocks */
    content_block_t blocks[MAX_CONTENT_BLOCKS];
    int             block_count;
    int             current_index;

    /* Per-block accumulators */
    jbuf_t          text_buf;       /* for text blocks */
    jbuf_t          input_buf;      /* for tool_use input JSON */

    /* Current block metadata */
    char           *cur_type;
    char           *cur_tool_name;
    char           *cur_tool_id;

    char           *stop_reason;
    usage_t         usage;

    /* Thinking block tracking */
    bool            in_thinking;

    /* Callbacks */
    stream_text_cb       text_cb;
    stream_tool_start_cb tool_cb;
    stream_thinking_cb   thinking_cb;
    void                *cb_ctx;

    /* SSE line buffer */
    jbuf_t          line_buf;
    bool            got_error;
    bool            credit_too_low; /* 402 / credit balance too low / insufficient funds */
    char           *error_msg;

    /* ── Streaming telemetry ───────────────────────────────────────── */
    double          telemetry_start;         /* request start time      */
    double          telemetry_first_delta;   /* first content_block_delta */
    double          telemetry_first_tool;    /* first tool_use block    */
    int             telemetry_thinking_chars; /* thinking text chars     */
    bool            telemetry_got_first;     /* whether first delta seen */

    /* ── Streaming degeneration detector ──────────────────────────── */
#define REPDET_WINDOW    4096  /* rolling window of recent text         */
#define REPDET_CHECK     256   /* check every N text bytes              */
#define REPDET_MIN_PAT   3    /* shortest repeating unit to detect      */
#define REPDET_MAX_PAT   256  /* longest repeating unit to scan         */
#define REPDET_THRESH    6    /* exact-repeat reps to trigger           */
#define REPDET_NGRAM_SZ  4    /* n-gram size for entropy analysis       */
#define REPDET_NGRAM_TAB 512  /* hash table buckets for n-gram counts   */
#define REPDET_ENTROPY_LO 1.5 /* bits — below this = degenerate        */
#define REPDET_STALL_WIN 512  /* bytes to measure alphabet collapse     */
#define REPDET_STALL_THR 0.08 /* unique-byte ratio below = stalled      */
    char            repdet_buf[REPDET_WINDOW];
    int             repdet_len;
    int             repdet_total_fed;        /* lifetime bytes fed        */
    int             repdet_bytes_since_check;
    bool            repdet_tripped;
    bool            repdet_subagent;
    char            repdet_diag[256];        /* human-readable diagnosis  */

    /* n-gram frequency table (rolling, rebuilt each check) */
    uint16_t        repdet_ngram[REPDET_NGRAM_TAB];
} sse_state_t;

/* Remove terminal control bytes/escape sequences from streamed model text.
   This prevents malformed or hostile ANSI/CSI sequences from leaking into
   rendered output while preserving regular UTF-8 text. */
void dsco_strip_terminal_controls_inplace(char *s) {
    if (!s || !*s) return;

    unsigned char *r = (unsigned char *)s;
    char *w = s;

    while (*r) {
        if (*r == 0x1b) {  /* ESC */
            r++;
            if (*r == '[') {  /* CSI ... final-byte */
                r++;
                while (*r && !(*r >= 0x40 && *r <= 0x7e)) r++;
                if (*r) r++;
                continue;
            }
            if (*r == ']') {  /* OSC ... BEL or ST */
                r++;
                while (*r) {
                    if (*r == '\a') { r++; break; }
                    if (r[0] == 0x1b && r[1] == '\\') { r += 2; break; }
                    r++;
                }
                continue;
            }
            if (*r == 'P' || *r == 'X' || *r == '^' || *r == '_') {  /* DCS/SOS/PM/APC */
                r++;
                while (*r) {
                    if (r[0] == 0x1b && r[1] == '\\') { r += 2; break; }
                    r++;
                }
                continue;
            }
            if (*r) r++;  /* ESC + single char sequence */
            continue;
        }

        if (*r < 0x20 && *r != '\n' && *r != '\r' && *r != '\t') { r++; continue; }
        if (*r == 0x7f) { r++; continue; }
        *w++ = (char)*r++;
    }

    *w = '\0';
}

/* ══════════════════════════════════════════════════════════════════════════
 * Streaming degeneration detector
 *
 * Three independent strategies, any one trips the abort:
 *
 * 1. EXACT REPEAT — finds the shortest period P (3–256 bytes) such that
 *    the tail of the window consists of ≥ REPDET_THRESH identical copies
 *    of a P-byte pattern.  O(window * max_pat) but bounded by constants.
 *
 * 2. N-GRAM ENTROPY — hashes every overlapping 4-gram in the window into
 *    a 512-bucket table, computes Shannon entropy over the distribution.
 *    Healthy text ≈ 6–8 bits; degenerate loops drop below REPDET_ENTROPY_LO.
 *    Only activates after 1 KB has been seen (avoids false positives on
 *    short JSON fragments).
 *
 * 3. BYTE STALL — counts distinct byte values in the last 512 bytes.
 *    If the ratio of unique bytes to the byte alphabet drops below 0.08,
 *    the output has collapsed to a tiny alphabet (e.g. one repeated word).
 *    Only activates after 2 KB has been seen.
 *
 * The detector feeds from text_delta events only, using a single 4 KB
 * rolling window. It checks every 256 new bytes.
 * ══════════════════════════════════════════════════════════════════════════ */

static int repdet_exact_threshold(const sse_state_t *s) {
    return s->repdet_subagent ? 8 : REPDET_THRESH;
}

static int repdet_entropy_min_feed(const sse_state_t *s) {
    return s->repdet_subagent ? 2048 : 1024;
}

static double repdet_entropy_threshold(const sse_state_t *s) {
    return s->repdet_subagent ? 0.75 : REPDET_ENTROPY_LO;
}

static int repdet_stall_min_feed(const sse_state_t *s) {
    return s->repdet_subagent ? 4096 : 2048;
}

static double repdet_stall_threshold(const sse_state_t *s) {
    return s->repdet_subagent ? 0.08 : REPDET_STALL_THR;
}

/* FNV-1a 32-bit for short n-grams → bucket index */
static inline uint32_t repdet_fnv(const char *p, int n) {
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < n; i++) {
        h ^= (uint32_t)(unsigned char)p[i];
        h *= 0x01000193u;
    }
    return h;
}

/* Check if a byte pattern consists entirely of benign formatting characters:
 * box-drawing (U+2500–U+257F), dashes, equals, underscores, spaces, stars,
 * em-dash (U+2014), en-dash (U+2013), horizontal ellipsis (U+2026), bullets. */
static bool repdet_is_formatting(const char *pat, int plen) {
    int i = 0;
    while (i < plen) {
        unsigned char c = (unsigned char)pat[i];
        if (c < 0x80) {
            /* ASCII: allow common formatting chars */
            if (c == '-' || c == '=' || c == '_' || c == ' ' ||
                c == '\n' || c == '\r' || c == '\t' ||
                c == '*' || c == '#' || c == '~' || c == '.' ||
                c == '|' || c == '+') {
                i++;
                continue;
            }
            return false;
        }
        /* UTF-8: decode codepoint */
        uint32_t cp = 0;
        int seqlen = 0;
        if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; seqlen = 2; }
        else if ((c & 0xF0) == 0xE0)  { cp = c & 0x0F; seqlen = 3; }
        else if ((c & 0xF8) == 0xF0)  { cp = c & 0x07; seqlen = 4; }
        else return false;
        if (i + seqlen > plen) return false;
        for (int j = 1; j < seqlen; j++) {
            unsigned char cont = (unsigned char)pat[i + j];
            if ((cont & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cont & 0x3F);
        }
        /* Allow box-drawing U+2500–U+257F, em/en-dash, ellipsis, bullets */
        if ((cp >= 0x2500 && cp <= 0x257F) || /* box drawing */
            cp == 0x2014 || cp == 0x2013 ||   /* em-dash, en-dash */
            cp == 0x2026 ||                    /* horizontal ellipsis */
            cp == 0x2022 || cp == 0x25CF ||    /* bullets */
            cp == 0x2502 || cp == 0x2503)      /* already in box-drawing but be explicit */
        {
            i += seqlen;
            continue;
        }
        return false;
    }
    return true;
}

/* Like repdet_is_formatting(), but tolerant of a rolling window that may
 * begin or end in the middle of a UTF-8 codepoint. */
static bool repdet_is_formatting_window(const char *pat, int plen) {
    int i = 0;
    bool saw_any = false;

    if (!pat || plen <= 0) return false;

    while (i < plen && (((unsigned char)pat[i] & 0xC0) == 0x80)) i++;

    while (i < plen) {
        unsigned char c = (unsigned char)pat[i];
        if (c < 0x80) {
            if (c == '-' || c == '=' || c == '_' || c == ' ' ||
                c == '\n' || c == '\r' || c == '\t' ||
                c == '*' || c == '#' || c == '~' || c == '.' ||
                c == '|' || c == '+') {
                saw_any = true;
                i++;
                continue;
            }
            return false;
        }

        uint32_t cp = 0;
        int seqlen = 0;
        if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; seqlen = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; seqlen = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; seqlen = 4; }
        else return false;

        if (i + seqlen > plen) break;
        for (int j = 1; j < seqlen; j++) {
            unsigned char cont = (unsigned char)pat[i + j];
            if ((cont & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cont & 0x3F);
        }

        if ((cp >= 0x2500 && cp <= 0x257F) ||
            cp == 0x2014 || cp == 0x2013 ||
            cp == 0x2026 ||
            cp == 0x2022 || cp == 0x25CF ||
            cp == 0x2502 || cp == 0x2503) {
            saw_any = true;
            i += seqlen;
            continue;
        }
        return false;
    }

    return saw_any;
}

/* Strategy 1: exact periodic repeat detection.
 * For each candidate period length, scan backwards from the tail checking
 * for consecutive identical copies.  Uses early-exit so small periods that
 * don't repeat are rejected quickly. */
static bool repdet_exact(sse_state_t *s) {
    const char *buf = s->repdet_buf;
    int len = s->repdet_len;
    int exact_thresh = repdet_exact_threshold(s);
    if (len < REPDET_MIN_PAT * exact_thresh) return false;

    int max_pat = len / exact_thresh;
    if (max_pat > REPDET_MAX_PAT) max_pat = REPDET_MAX_PAT;

    for (int plen = REPDET_MIN_PAT; plen <= max_pat; plen++) {
        const char *pat = buf + len - plen;
        int reps = 1;
        int pos = len - plen * 2;
        while (pos >= 0) {
            if (memcmp(pat, buf + pos, (size_t)plen) != 0) break;
            reps++;
            pos -= plen;
        }
        if (reps >= exact_thresh) {
            /* Skip benign formatting patterns (box-drawing, dashes, etc.) */
            if (repdet_is_formatting(pat, plen)) continue;
            /* Extract a preview of the repeating unit */
            int preview_len = plen < 60 ? plen : 60;
            char preview[64];
            memcpy(preview, pat, (size_t)preview_len);
            preview[preview_len] = '\0';
            /* Sanitize for display */
            for (int i = 0; i < preview_len; i++)
                if ((unsigned char)preview[i] < 32) preview[i] = '.';
            snprintf(s->repdet_diag, sizeof(s->repdet_diag),
                     "exact repeat: \"%s\"%s x%d (period=%d)",
                     preview, plen > 60 ? "..." : "", reps, plen);
            return true;
        }
    }
    return false;
}

/* Strategy 2: n-gram Shannon entropy.
 * Low entropy means the same few n-grams dominate → degenerate output. */
static bool repdet_entropy(sse_state_t *s) {
    int len = s->repdet_len;
    if (len < REPDET_NGRAM_SZ + 1) return false;
    /* Only trigger after enough data to be meaningful */
    if (s->repdet_total_fed < repdet_entropy_min_feed(s)) return false;

    memset(s->repdet_ngram, 0, sizeof(s->repdet_ngram));

    int ngram_count = len - REPDET_NGRAM_SZ + 1;
    for (int i = 0; i < ngram_count; i++) {
        uint32_t h = repdet_fnv(s->repdet_buf + i, REPDET_NGRAM_SZ);
        s->repdet_ngram[h % REPDET_NGRAM_TAB]++;
    }

    /* Compute entropy over bucket distribution */
    double entropy = 0.0;
    double inv_n = 1.0 / (double)ngram_count;
    for (int i = 0; i < REPDET_NGRAM_TAB; i++) {
        if (s->repdet_ngram[i] == 0) continue;
        double p = (double)s->repdet_ngram[i] * inv_n;
        entropy -= p * log2(p);
    }

    double threshold = repdet_entropy_threshold(s);
    if (entropy < threshold) {
        snprintf(s->repdet_diag, sizeof(s->repdet_diag),
                 "low n-gram entropy: %.2f bits (threshold=%.1f, window=%d bytes)",
                 entropy, threshold, len);
        return true;
    }
    return false;
}

/* Strategy 3: byte-alphabet stall detection.
 * If the last N bytes use very few distinct byte values, the output
 * has collapsed (e.g., "aaaaaaa" or alternating between 2-3 chars). */
static bool repdet_stall(sse_state_t *s) {
    int len = s->repdet_len;
    if (len < REPDET_STALL_WIN) return false;
    if (s->repdet_total_fed < repdet_stall_min_feed(s)) return false;

    /* Count distinct bytes in the tail */
    uint8_t seen[256];
    memset(seen, 0, sizeof(seen));
    const char *tail = s->repdet_buf + len - REPDET_STALL_WIN;
    if (repdet_is_formatting_window(tail, REPDET_STALL_WIN)) {
        return false;
    }
    int distinct = 0;
    for (int i = 0; i < REPDET_STALL_WIN; i++) {
        uint8_t b = (uint8_t)tail[i];
        if (!seen[b]) { seen[b] = 1; distinct++; }
    }

    /* Normalize against the byte alphabet, not the sample width.
       Healthy prose only uses a subset of 256 byte values, so dividing
       by the 512-byte window badly over-penalizes normal text. */
    double ratio = (double)distinct / 256.0;
    double threshold = repdet_stall_threshold(s);
    if (ratio < threshold) {
        snprintf(s->repdet_diag, sizeof(s->repdet_diag),
                 "byte stall: %d distinct bytes in %d (alphabet-ratio=%.3f, threshold=%.2f)",
                 distinct, REPDET_STALL_WIN, ratio, threshold);
        return true;
    }
    return false;
}

/* Run all strategies and trip if any fires. */
static bool repdet_check(sse_state_t *s) {
    if (repdet_exact(s))   return true;
    if (repdet_entropy(s)) return true;
    if (repdet_stall(s))   return true;
    return false;
}

static void repdet_feed(sse_state_t *s, const char *text, size_t tlen) {
    if (s->repdet_tripped || tlen == 0) return;
    if (repdet_is_formatting(text, (int)tlen)) return;

    /* Append to rolling window */
    if (s->repdet_len + (int)tlen > REPDET_WINDOW) {
        int shift = s->repdet_len + (int)tlen - REPDET_WINDOW;
        if (shift > s->repdet_len) shift = s->repdet_len;
        memmove(s->repdet_buf, s->repdet_buf + shift, (size_t)(s->repdet_len - shift));
        s->repdet_len -= shift;
    }
    int copylen = (int)tlen;
    if (copylen > REPDET_WINDOW) {
        text += (tlen - (size_t)REPDET_WINDOW);
        copylen = REPDET_WINDOW;
    }
    memcpy(s->repdet_buf + s->repdet_len, text, (size_t)copylen);
    s->repdet_len += copylen;
    s->repdet_total_fed += copylen;

    s->repdet_bytes_since_check += copylen;
    if (s->repdet_bytes_since_check >= REPDET_CHECK) {
        s->repdet_bytes_since_check = 0;
        if (repdet_check(s)) {
            s->repdet_tripped = true;
            fprintf(stderr,
                "\n\033[33m[dsco] degenerate output detected — aborting stream\033[0m\n"
                "\033[2m  %s\033[0m\n", s->repdet_diag);
        }
    }
}

/* Reset repdet state (called between content blocks of different types) */
static void repdet_reset(sse_state_t *s) {
    s->repdet_len = 0;
    s->repdet_bytes_since_check = 0;
    s->repdet_total_fed = 0;
    s->repdet_diag[0] = '\0';
    /* NOTE: do not reset repdet_tripped — once tripped, stay tripped */
}

bool llm_repdet_text_is_degenerate(const char *text, bool subagent,
                                   char *diag, size_t diag_len) {
    sse_state_t s = {0};
    s.repdet_subagent = subagent;

    if (text && text[0]) {
        repdet_feed(&s, text, strlen(text));
    }

    if (diag && diag_len > 0) {
        snprintf(diag, diag_len, "%s", s.repdet_diag);
    }
    return s.repdet_tripped;
}

/* Curl progress callback — allows Ctrl+C to abort transfers and feeds
   the stream heartbeat with download activity information. */
static int stream_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                                curl_off_t ultotal, curl_off_t ulnow) {
    sse_state_t *s = (sse_state_t *)clientp;
    (void)dltotal; (void)ultotal; (void)ulnow;
    if (g_interrupted || (s && s->repdet_tripped)) return 1;  /* non-zero aborts transfer */
    /* Keep heartbeat alive when curl is actively receiving, even if
       no SSE events have fired yet (e.g. during HTTP/2 negotiation) */
    if (dlnow > 0 && g_stream_heartbeat)
        tui_stream_heartbeat_recv(g_stream_heartbeat, 0);  /* 0 = just a ping */
    return 0;
}

static void sse_finalize_block(sse_state_t *s) {
    if (s->current_index < 0) return;
    if (s->block_count >= MAX_CONTENT_BLOCKS) {
        /* Drop excess blocks safely, but always reset current block state
           so parsers and loops cannot get stuck on overflow. */
        free(s->cur_type); s->cur_type = NULL;
        free(s->cur_tool_name); s->cur_tool_name = NULL;
        free(s->cur_tool_id); s->cur_tool_id = NULL;
        jbuf_reset(&s->text_buf);
        jbuf_reset(&s->input_buf);
        s->current_index = -1;
        return;
    }
    int idx = s->block_count++;

    content_block_t *blk = &s->blocks[idx];
    blk->type = s->cur_type ? safe_strdup(s->cur_type) : safe_strdup("text");

    if (s->cur_type && strcmp(s->cur_type, "text") == 0) {
        blk->text = safe_strdup(s->text_buf.data ? s->text_buf.data : "");
    } else if (s->cur_type && strcmp(s->cur_type, "thinking") == 0) {
        blk->text = safe_strdup(s->text_buf.data ? s->text_buf.data : "");
        s->in_thinking = false;
    } else if (s->cur_type && strcmp(s->cur_type, "tool_use") == 0) {
        blk->tool_name = s->cur_tool_name ? safe_strdup(s->cur_tool_name) : NULL;
        blk->tool_id = s->cur_tool_id ? safe_strdup(s->cur_tool_id) : NULL;
        if (s->input_buf.data && s->input_buf.data[0] != '\0') {
            blk->tool_input = safe_strdup(s->input_buf.data);
        } else {
            blk->tool_input = safe_strdup("{}");
        }
    } else if (s->cur_type && strcmp(s->cur_type, "server_tool_use") == 0) {
        /* Server-side tool — preserve for conversation history */
        blk->tool_name = s->cur_tool_name ? safe_strdup(s->cur_tool_name) : NULL;
        blk->tool_id = s->cur_tool_id ? safe_strdup(s->cur_tool_id) : NULL;
        if (s->input_buf.data && s->input_buf.data[0] != '\0') {
            blk->tool_input = safe_strdup(s->input_buf.data);
        } else {
            blk->tool_input = safe_strdup("{}");
        }
    } else if (s->cur_type &&
               (strcmp(s->cur_type, "web_search_tool_result") == 0 ||
                strcmp(s->cur_type, "code_execution_tool_result") == 0)) {
        /* Server-side tool results — preserve structured content payload. */
        bool is_code_execution =
            (strcmp(s->cur_type, "code_execution_tool_result") == 0);
        blk->tool_id = s->cur_tool_id ? safe_strdup(s->cur_tool_id) : NULL;
        blk->text = safe_strdup(s->text_buf.data ? s->text_buf.data : "");
        if (s->input_buf.data && s->input_buf.data[0]) {
            blk->tool_input = safe_strdup(s->input_buf.data);
        } else if (s->text_buf.data &&
                   (s->text_buf.data[0] == '[' || s->text_buf.data[0] == '{')) {
            blk->tool_input = safe_strdup(s->text_buf.data);
        } else {
            blk->tool_input = safe_strdup(
                is_code_execution
                    ? "{\"type\":\"code_execution_tool_result_error\","
                      "\"error_code\":\"unavailable\"}"
                    : "[]");
        }
        if (is_code_execution &&
            s->text_buf.data && s->text_buf.data[0]) {
            fprintf(stderr, "    \033[32m\xe2\x9c\x93\033[0m \033[2mcode output: %.*s\033[0m\n",
                    120, s->text_buf.data);
        }
    }

    free(s->cur_type); s->cur_type = NULL;
    free(s->cur_tool_name); s->cur_tool_name = NULL;
    free(s->cur_tool_id); s->cur_tool_id = NULL;
    jbuf_reset(&s->text_buf);
    jbuf_reset(&s->input_buf);
    repdet_reset(s);
    s->current_index = -1;
}

static void sse_handle_event(sse_state_t *s, const char *data) {
    char *event_type = json_get_str(data, "type");
    if (!event_type) return;

    if (strcmp(event_type, "message_start") == 0) {
        /* Extract usage from message.usage */
        char *msg = json_get_raw(data, "message");
        if (msg) {
            char *usage_raw = json_get_raw(msg, "usage");
            if (usage_raw) {
                s->usage.input_tokens = json_get_int(usage_raw, "input_tokens", 0);
                s->usage.cache_creation_input_tokens = json_get_int(usage_raw, "cache_creation_input_tokens", 0);
                s->usage.cache_read_input_tokens = json_get_int(usage_raw, "cache_read_input_tokens", 0);
                free(usage_raw);
            }
            free(msg);
        }
    } else if (strcmp(event_type, "content_block_start") == 0) {
        int index = json_get_int(data, "index", 0);
        s->current_index = index;

        char *cb_raw = json_get_raw(data, "content_block");
        if (cb_raw) {
            free(s->cur_type);
            s->cur_type = json_get_str(cb_raw, "type");

            if (s->cur_type && strcmp(s->cur_type, "tool_use") == 0) {
                free(s->cur_tool_name);
                s->cur_tool_name = json_get_str(cb_raw, "name");
                free(s->cur_tool_id);
                s->cur_tool_id = json_get_str(cb_raw, "id");
                jbuf_reset(&s->input_buf);
                /* NOTE: content_block_start always sends "input":{} as a
                   placeholder.  The real input arrives via input_json_delta
                   events.  Do NOT seed input_buf here — otherwise we get
                   {}{"url":"..."} which is invalid JSON. */

                if (s->tool_cb && s->cur_tool_name) {
                    s->tool_cb(s->cur_tool_name, s->cur_tool_id, s->cb_ctx);
                }
                /* Tool telemetry */
                if (s->telemetry_first_tool == 0)
                    s->telemetry_first_tool = cache_now_sec();
            } else if (s->cur_type && strcmp(s->cur_type, "thinking") == 0) {
                jbuf_reset(&s->text_buf);
                s->in_thinking = true;
                /* Only print header if no thinking callback handles display */
                if (!s->thinking_cb) {
                    fprintf(stderr, "  \033[2m\033[3m[thinking] ");
                    fflush(stderr);
                }
            } else if (s->cur_type && strcmp(s->cur_type, "server_tool_use") == 0) {
                /* Server-side tool (web_search, code_execution) */
                free(s->cur_tool_name);
                s->cur_tool_name = json_get_str(cb_raw, "name");
                free(s->cur_tool_id);
                s->cur_tool_id = json_get_str(cb_raw, "id");
                if (s->cur_tool_name) {
                    fprintf(stderr, "  \033[1m\033[36m%s\033[0m \033[2m%s (server)\033[0m\n",
                            tui_glyph()->icon_lightning, s->cur_tool_name);
                    fflush(stderr);
                }
                jbuf_reset(&s->input_buf);
            } else if (s->cur_type && strcmp(s->cur_type, "web_search_tool_result") == 0) {
                free(s->cur_tool_id);
                s->cur_tool_id = json_get_str(cb_raw, "tool_use_id");
                jbuf_reset(&s->text_buf);
                jbuf_reset(&s->input_buf);
                char *content_raw = json_get_raw(cb_raw, "content");
                if (content_raw) {
                    jbuf_append(&s->input_buf, content_raw);
                    free(content_raw);
                }
            } else if (s->cur_type && strcmp(s->cur_type, "code_execution_tool_result") == 0) {
                free(s->cur_tool_id);
                s->cur_tool_id = json_get_str(cb_raw, "tool_use_id");
                jbuf_reset(&s->text_buf);
                jbuf_reset(&s->input_buf);
                char *content_raw = json_get_raw(cb_raw, "content");
                if (content_raw) {
                    jbuf_append(&s->input_buf, content_raw);
                    free(content_raw);
                }
            }
            free(cb_raw);
        }
    } else if (strcmp(event_type, "content_block_delta") == 0) {
        char *delta_raw = json_get_raw(data, "delta");
        if (delta_raw) {
            char *delta_type = json_get_str(delta_raw, "type");
            if (delta_type && strcmp(delta_type, "text_delta") == 0) {
                char *text = json_get_str(delta_raw, "text");
                if (text) {
                    dsco_strip_terminal_controls_inplace(text);
                    if (text[0] != '\0') {
                        /* TTFT telemetry */
                        if (!s->telemetry_got_first) {
                            s->telemetry_first_delta = cache_now_sec();
                            s->telemetry_got_first = true;
                        }
                        jbuf_append(&s->text_buf, text);
                        repdet_feed(s, text, strlen(text));
                        if (s->text_cb)
                            s->text_cb(text, s->cb_ctx);
                    }
                    free(text);
                }
            } else if (delta_type && strcmp(delta_type, "thinking_delta") == 0) {
                char *thinking = json_get_str(delta_raw, "thinking");
                if (thinking) {
                    dsco_strip_terminal_controls_inplace(thinking);
                    if (thinking[0] != '\0') {
                        jbuf_append(&s->text_buf, thinking);
                        /* Stream thinking text dimmed+italic via callback or stderr */
                        if (s->thinking_cb) {
                            s->thinking_cb(thinking, s->cb_ctx);
                        } else {
                            fprintf(stderr, "%s", thinking);
                            fflush(stderr);
                        }
                    }
                    free(thinking);
                }
            } else if (delta_type && strcmp(delta_type, "input_json_delta") == 0) {
                char *partial = json_get_str(delta_raw, "partial_json");
                if (partial) {
                    jbuf_append(&s->input_buf, partial);
                    free(partial);
                }
            } else if (delta_type && strcmp(delta_type, "content_delta") == 0) {
                /* Server-side tool result content delta — may be text or JSON */
                char *content = json_get_str(delta_raw, "content");
                if (content) {
                    dsco_strip_terminal_controls_inplace(content);
                    jbuf_append(&s->text_buf, content);
                    if (s->cur_type &&
                        (strcmp(s->cur_type, "web_search_tool_result") == 0 ||
                         strcmp(s->cur_type, "code_execution_tool_result") == 0)) {
                        jbuf_append(&s->input_buf, content);
                    }
                    free(content);
                } else {
                    /* Try raw JSON (e.g., web_search results are structured) */
                    char *raw_content = json_get_raw(delta_raw, "content");
                    if (raw_content) {
                        jbuf_append(&s->text_buf, raw_content);
                        if (s->cur_type &&
                            (strcmp(s->cur_type, "web_search_tool_result") == 0 ||
                             strcmp(s->cur_type, "code_execution_tool_result") == 0)) {
                            jbuf_append(&s->input_buf, raw_content);
                        }
                        free(raw_content);
                    }
                }
            }
            free(delta_type);
            free(delta_raw);
        }
    } else if (strcmp(event_type, "content_block_stop") == 0) {
        /* Close thinking block ANSI styling if we printed the header */
        if (s->in_thinking && !s->thinking_cb) {
            fprintf(stderr, "\033[0m\n");
            fflush(stderr);
        }
        sse_finalize_block(s);
    } else if (strcmp(event_type, "message_delta") == 0) {
        char *delta_raw = json_get_raw(data, "delta");
        if (delta_raw) {
            free(s->stop_reason);
            s->stop_reason = json_get_str(delta_raw, "stop_reason");
            free(delta_raw);
        }
        char *usage_raw = json_get_raw(data, "usage");
        if (usage_raw) {
            s->usage.output_tokens = json_get_int(usage_raw, "output_tokens", 0);
            free(usage_raw);
        }
    } else if (strcmp(event_type, "error") == 0) {
        s->got_error = true;
        char *err_raw = json_get_raw(data, "error");
        if (err_raw) {
            s->error_msg = json_get_str(err_raw, "message");
            char *err_type = json_get_str(err_raw, "type");
            if (provider_msg_is_credit_too_low(s->error_msg) ||
                provider_msg_is_credit_too_low(err_type))
                s->credit_too_low = true;
            free(err_type);
            free(err_raw);
        }
        /* Also scan the raw SSE data in case the credit phrase is present
         * outside the "error.message" field (some proxies flatten it). */
        if (!s->credit_too_low && provider_msg_is_credit_too_low(data))
            s->credit_too_low = true;
    }
    /* message_stop, ping — ignored */

    free(event_type);
}

static void sse_process_line(sse_state_t *s, const char *line) {
    if (strncmp(line, "data: ", 6) == 0) {
        const char *json_data = line + 6;
        if (strcmp(json_data, "[DONE]") == 0) return;
        sse_handle_event(s, json_data);
    }
    /* event: lines, empty lines, comments — ignored */
}

/* Curl write callback that buffers and processes SSE lines */
static size_t stream_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    sse_state_t *s = (sse_state_t *)userdata;

    if (g_interrupted || s->repdet_tripped) return 0;

    /* Feed byte count to heartbeat for activity tracking */
    if (g_stream_heartbeat)
        tui_stream_heartbeat_recv(g_stream_heartbeat, total);

    const char *p = (const char *)ptr;
    for (size_t i = 0; i < total; i++) {
        if (g_interrupted || s->repdet_tripped) return 0;
        if (p[i] == '\n') {
            if (s->line_buf.len > 0) {
                sse_process_line(s, s->line_buf.data);
                jbuf_reset(&s->line_buf);
            }
        } else if (p[i] != '\r') {
            jbuf_append_char(&s->line_buf, p[i]);
        }
    }
    return total;
}

/* Helper: build curl headers for API request */
static struct curl_slist *build_api_headers(const char *api_key) {
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    char auth[512];
    if (llm_anthropic_uses_claude_code_auth(api_key))
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    else
        snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
    hdrs = curl_slist_append(hdrs, auth);
    char ver[128];
    snprintf(ver, sizeof(ver), "anthropic-version: %s", ANTHROPIC_VERSION);
    hdrs = curl_slist_append(hdrs, ver);
    char beta[256];
    if (llm_anthropic_uses_claude_code_auth(api_key))
        snprintf(beta, sizeof(beta), "anthropic-beta: %s,%s",
                 CLAUDE_CODE_OAUTH_BETA, ANTHROPIC_BETAS);
    else
        snprintf(beta, sizeof(beta), "anthropic-beta: %s", ANTHROPIC_BETAS);
    hdrs = curl_slist_append(hdrs, beta);
    hdrs = curl_slist_append(hdrs, "Expect:");
    return hdrs;
}

/* Helper: configure curl handle for streaming API call */
static void setup_curl_opts(CURL *curl, struct curl_slist *hdrs,
                            const char *request_json, sse_state_t *st) {
    curl_easy_setopt(curl, CURLOPT_URL, API_URL_ANTHROPIC);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, st);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stream_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, st);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 100L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);
}

/* Helper: reset SSE state for retry (frees accumulated blocks and buffers) */
static void sse_state_reset_for_retry(sse_state_t *s) {
    int safe_blocks = s->block_count;
    if (safe_blocks < 0) safe_blocks = 0;
    if (safe_blocks > MAX_CONTENT_BLOCKS) safe_blocks = MAX_CONTENT_BLOCKS;
    for (int i = 0; i < safe_blocks; i++) {
        free(s->blocks[i].type);
        free(s->blocks[i].text);
        free(s->blocks[i].tool_name);
        free(s->blocks[i].tool_id);
        free(s->blocks[i].tool_input);
        memset(&s->blocks[i], 0, sizeof(content_block_t));
    }
    s->block_count = 0;
    s->current_index = -1;
    s->in_thinking = false;
    s->got_error = false;
    free(s->error_msg); s->error_msg = NULL;
    free(s->stop_reason); s->stop_reason = NULL;
    free(s->cur_type); s->cur_type = NULL;
    free(s->cur_tool_name); s->cur_tool_name = NULL;
    free(s->cur_tool_id); s->cur_tool_id = NULL;
    jbuf_reset(&s->text_buf);
    jbuf_reset(&s->input_buf);
    jbuf_reset(&s->line_buf);
    memset(&s->usage, 0, sizeof(s->usage));
}

stream_result_t llm_stream(const char *api_key, const char *request_json,
                           stream_text_cb text_cb,
                           stream_tool_start_cb tool_cb,
                           stream_thinking_cb thinking_cb,
                           void *cb_ctx) {
    stream_result_t result = {0};

    /* Init SSE state */
    sse_state_t state = {0};
    state.current_index = -1;
    state.text_cb = text_cb;
    state.tool_cb = tool_cb;
    state.thinking_cb = thinking_cb;
    state.cb_ctx = cb_ctx;
    jbuf_init(&state.text_buf, 4096);
    jbuf_init(&state.input_buf, 4096);
    jbuf_init(&state.line_buf, 4096);
    state.telemetry_start = cache_now_sec();
    {
        const char *subagent = getenv("DSCO_SUBAGENT");
        state.repdet_subagent = subagent && subagent[0] && strcmp(subagent, "0") != 0;
    }

    /* Streaming checkpoint for retry resilience */
    stream_checkpoint_t checkpoint;
    stream_checkpoint_init(&checkpoint);

    /* Retry loop with exponential backoff */
    /* Configurable via DSCO_LLM_MAX_RETRIES and DSCO_LLM_RETRY_DELAY_MS */
    int max_retries = 3;
    int retry_delay_ms = 1000;
    const char *env_retries = getenv("DSCO_LLM_MAX_RETRIES");
    const char *env_delay = getenv("DSCO_LLM_RETRY_DELAY_MS");
    if (env_retries && env_retries[0]) {
        int v = atoi(env_retries);
        if (v >= 0 && v <= 10) max_retries = v;
    }
    if (env_delay && env_delay[0]) {
        int v = atoi(env_delay);
        if (v >= 100 && v <= 30000) retry_delay_ms = v;
    }
    CURLcode res = CURLE_OK;
    long http_code = 0;

    for (int attempt = 0; attempt <= max_retries; attempt++) {
        if (g_interrupted) {
            res = CURLE_ABORTED_BY_CALLBACK;
            break;
        }
        if (attempt > 0) {
            if (g_interrupted) {
                res = CURLE_ABORTED_BY_CALLBACK;
                break;
            }
            /* Save completed blocks before retry */
            if (state.block_count > 0) {
                stream_checkpoint_save(&checkpoint,
                    state.blocks, state.block_count,
                    state.text_buf.data, state.input_buf.data,
                    &state.usage, NULL);
            }
            fprintf(stderr, "  \033[33m\xe2\x9f\xb3 retry %d/%d (waiting %dms, %d blocks checkpointed)\033[0m\n",
                    attempt, max_retries, retry_delay_ms, checkpoint.saved_count);
            if (usleep((useconds_t)retry_delay_ms * 1000) != 0 &&
                errno == EINTR && g_interrupted) {
                res = CURLE_ABORTED_BY_CALLBACK;
                break;
            }
            if (g_interrupted) {
                res = CURLE_ABORTED_BY_CALLBACK;
                break;
            }
            retry_delay_ms *= 2;
            sse_state_reset_for_retry(&state);
        }

        CURL *curl = curl_easy_init();
        if (!curl) {
            fprintf(stderr, "dsco: curl_easy_init failed\n");
            if (attempt == max_retries) break;
            continue;
        }

        struct curl_slist *headers = build_api_headers(api_key);
        setup_curl_opts(curl, headers, request_json, &state);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK && (g_interrupted || state.repdet_tripped)) {
            res = CURLE_ABORTED_BY_CALLBACK;
        }

        http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        /* F40: Extract cURL timing breakdown */
        {
            double dns = 0, conn = 0, tls = 0, ttfb = 0, total = 0;
            curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &dns);
            curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &conn);
            curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &tls);
            curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &ttfb);
            curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);
            result.telemetry.latency.dns_ms = dns * 1000.0;
            result.telemetry.latency.connect_ms = conn * 1000.0;
            result.telemetry.latency.tls_ms = tls * 1000.0;
            result.telemetry.latency.ttfb_ms = ttfb * 1000.0;
            result.telemetry.latency.total_ms = total * 1000.0;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        /* Determine if we should retry */
        bool should_retry = false;
        if (res == CURLE_OPERATION_TIMEDOUT || res == CURLE_COULDNT_CONNECT) {
            should_retry = true;
        } else if (res == CURLE_OK &&
                   (http_code == 429 || http_code == 500 ||
                    http_code == 502 || http_code == 503 || http_code == 529)) {
            should_retry = true;
        }

        /* Don't retry user interrupts or if this was the last attempt */
        if (g_interrupted || res == CURLE_ABORTED_BY_CALLBACK ||
            !should_retry || attempt == max_retries) {
            break;
        }
    }

    result.http_status = (int)http_code;

    /* Restore checkpointed blocks if current attempt yielded nothing but
       we had saved blocks from a prior successful partial stream */
    if (state.block_count == 0 && checkpoint.saved_count > 0) {
        for (int i = 0; i < checkpoint.saved_count && i < MAX_CONTENT_BLOCKS; i++) {
            state.blocks[i].type = safe_strdup(checkpoint.saved_blocks[i].type);
            state.blocks[i].text = safe_strdup(checkpoint.saved_blocks[i].text);
            state.blocks[i].tool_name = safe_strdup(checkpoint.saved_blocks[i].tool_name);
            state.blocks[i].tool_id = safe_strdup(checkpoint.saved_blocks[i].tool_id);
            state.blocks[i].tool_input = safe_strdup(checkpoint.saved_blocks[i].tool_input);
        }
        state.block_count = checkpoint.saved_count;
        /* Merge usage */
        state.usage.input_tokens += checkpoint.saved_usage.input_tokens;
        state.usage.output_tokens += checkpoint.saved_usage.output_tokens;
    }
    stream_checkpoint_free(&checkpoint);

    if (res == CURLE_ABORTED_BY_CALLBACK) {
        /* If aborted due to repetition detection, still mark ok=true so the
           truncated response can be added to history, but the content is cleaned.
           If aborted by user interrupt (Ctrl+C), treat as ok if we got content. */
        result.ok = (state.block_count > 0 || state.text_buf.len > 0);
    } else if (res != CURLE_OK) {
        DSCO_SET_ERR(DSCO_ERR_NET, "stream failed: %s (HTTP %ld)",
                     curl_easy_strerror(res), http_code);
        fprintf(stderr, "dsco: stream failed: %s\n", curl_easy_strerror(res));
        result.ok = false;
    } else if (state.got_error) {
        if (state.credit_too_low) {
            fprintf(stderr,
                    "  \033[31m\xe2\x9c\x97 anthropic credit/billing error:\033[0m %s\n"
                    "  \033[2mhint: falling back to the next provider in the chain "
                    "(xAI/OpenAI/Google/etc). Set ANTHROPIC_API_KEY or OPENROUTER_API_KEY for an alternative path.\033[0m\n",
                    state.error_msg ? state.error_msg : "credit balance is too low");
        } else {
            fprintf(stderr, "dsco: API error: %s\n", state.error_msg ? state.error_msg : "unknown");
        }
        result.ok = false;
    } else if (http_code != 200) {
        /* Credit/billing errors show up as 400 with an error.message body, or
         * as 402 Payment Required depending on the upstream. Parse the body
         * so we can classify and present an actionable hint. */
        if (state.line_buf.len > 0) {
            char *body_err = json_get_raw(state.line_buf.data, "error");
            if (body_err) {
                char *msg = json_get_str(body_err, "message");
                char *typ = json_get_str(body_err, "type");
                if (http_code == 402 ||
                    provider_msg_is_credit_too_low(msg) ||
                    provider_msg_is_credit_too_low(typ) ||
                    provider_msg_is_credit_too_low(state.line_buf.data))
                    state.credit_too_low = true;
                if (state.credit_too_low) {
                    fprintf(stderr,
                            "  \033[31m\xe2\x9c\x97 anthropic credit/billing error (HTTP %d):\033[0m %s\n"
                            "  \033[2mhint: fallback chain will retry via xAI/OpenAI/Google/... "
                            "or set OPENROUTER_API_KEY for a cross-lab path.\033[0m\n",
                            (int)http_code, msg ? msg : "credit balance is too low");
                } else {
                    fprintf(stderr, "dsco: HTTP %d: %s\n", (int)http_code, state.line_buf.data);
                }
                free(msg);
                free(typ);
                free(body_err);
            } else {
                if (http_code == 402 || provider_msg_is_credit_too_low(state.line_buf.data))
                    state.credit_too_low = true;
                fprintf(stderr, "dsco: HTTP %d: %s\n", (int)http_code, state.line_buf.data);
            }
        } else {
            if (http_code == 402) state.credit_too_low = true;
            fprintf(stderr, "dsco: HTTP %d\n", (int)http_code);
        }
        /* Save failed request for debugging (HTTP 400 errors) */
        if (http_code == 400) {
            llm_debug_save_request(request_json, (int)http_code);
        }
        result.ok = false;
    } else {
        result.ok = true;
    }

    /* Propagate credit_too_low as a stop_reason sentinel so agent.c can log
     * it distinctly and the fallback loop gets a clean signal. */
    if (state.credit_too_low) {
        free(state.stop_reason);
        state.stop_reason = safe_strdup("credit_too_low");
    }

    /* Finalize any incomplete block (connection dropped mid-stream) */
    if (state.current_index >= 0) {
        sse_finalize_block(&state);
    }

    int safe_block_count = state.block_count;
    if (safe_block_count < 0) safe_block_count = 0;
    if (safe_block_count > MAX_CONTENT_BLOCKS) safe_block_count = MAX_CONTENT_BLOCKS;

    /* Build parsed response from accumulated blocks */
    result.parsed.count = safe_block_count;
    result.parsed.blocks = safe_malloc((safe_block_count > 0 ? safe_block_count : 1)
                                       * sizeof(content_block_t));
    memset(result.parsed.blocks, 0, (safe_block_count > 0 ? safe_block_count : 1)
                                     * sizeof(content_block_t));
    for (int i = 0; i < safe_block_count; i++) {
        result.parsed.blocks[i] = state.blocks[i]; /* move ownership */
    }
    result.parsed.stop_reason = state.stop_reason; /* move ownership */
    result.usage = state.usage;

    /* Populate streaming telemetry */
    double end_time = cache_now_sec();
    result.telemetry.total_ms = (end_time - state.telemetry_start) * 1000.0;
    if (state.telemetry_got_first)
        result.telemetry.ttft_ms = (state.telemetry_first_delta - state.telemetry_start) * 1000.0;
    if (state.telemetry_first_tool > 0)
        result.telemetry.ttft_tool_ms = (state.telemetry_first_tool - state.telemetry_start) * 1000.0;
    if (result.telemetry.total_ms > 0 && result.usage.output_tokens > 0)
        result.telemetry.tokens_per_sec = result.usage.output_tokens / (result.telemetry.total_ms / 1000.0);
    result.telemetry.thinking_tokens = state.telemetry_thinking_chars / 4; /* rough estimate */

    /* Cleanup */
    free(state.error_msg);
    free(state.cur_type);
    free(state.cur_tool_name);
    free(state.cur_tool_id);
    jbuf_free(&state.text_buf);
    jbuf_free(&state.input_buf);
    jbuf_free(&state.line_buf);

    return result;
}
