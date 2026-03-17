/* provider.c — Multi-provider abstraction for LLM API calls.
 *
 * Currently supports:
 *   - "anthropic" — Anthropic Messages API (default)
 *   - "openai"    — OpenAI Chat Completions API (and compatible)
 *
 * The Anthropic provider delegates to the existing llm.c functions.
 * The OpenAI provider implements request building and SSE parsing
 * for the Chat Completions API format.
 */

#include "provider.h"
#include "config.h"
#include "tools.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <curl/curl.h>

/* Forward declarations */
static char *openai_build_request(provider_t *p, conversation_t *conv,
                                   session_state_t *session, int max_tokens);
static struct curl_slist *openai_build_headers(provider_t *p, const char *api_key);
static bool openrouter_should_disable_thinking(session_state_t *session);

/* ── Anthropic Provider ────────────────────────────────────────────────── */

static char *anthropic_build_request(provider_t *p, conversation_t *conv,
                                      session_state_t *session, int max_tokens) {
    (void)p;
    return llm_build_request_ex(conv, session, max_tokens);
}

static struct curl_slist *anthropic_build_headers(provider_t *p, const char *api_key) {
    (void)p;
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    char auth[512];
    snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
    hdrs = curl_slist_append(hdrs, auth);
    char ver[128];
    snprintf(ver, sizeof(ver), "anthropic-version: %s", ANTHROPIC_VERSION);
    hdrs = curl_slist_append(hdrs, ver);
    char beta[256];
    snprintf(beta, sizeof(beta), "anthropic-beta: %s", ANTHROPIC_BETAS);
    hdrs = curl_slist_append(hdrs, beta);
    hdrs = curl_slist_append(hdrs, "Expect:");
    return hdrs;
}

static stream_result_t anthropic_stream(provider_t *p, const char *api_key,
                                          const char *request_json,
                                          stream_text_cb text_cb,
                                          stream_tool_start_cb tool_cb,
                                          stream_thinking_cb thinking_cb,
                                          void *cb_ctx) {
    (void)p;
    return llm_stream(api_key, request_json, text_cb, tool_cb, thinking_cb, cb_ctx);
}

/* ── OpenRouter Provider ────────────────────────────────────────────────── */

static struct curl_slist *openrouter_build_headers(provider_t *p, const char *api_key) {
    struct curl_slist *hdrs = openai_build_headers(p, api_key);
    const char *referer = getenv("DSCO_OR_REFERER");
    if (!referer) referer = "https://github.com/dsco-cli";
    char hdr[512];
    snprintf(hdr, sizeof(hdr), "HTTP-Referer: %s", referer);
    hdrs = curl_slist_append(hdrs, hdr);
    const char *title = getenv("DSCO_OR_TITLE");
    if (!title) title = "dsco";
    snprintf(hdr, sizeof(hdr), "X-Title: %s", title);
    hdrs = curl_slist_append(hdrs, hdr);
    return hdrs;
}

/* Helper: append a comma-separated env var as a JSON string array */
static void or_append_csv_array(jbuf_t *b, const char *csv) {
    jbuf_append(b, "[");
    const char *cur = csv;
    bool first = true;
    while (*cur) {
        const char *end = strchr(cur, ',');
        if (!end) end = cur + strlen(cur);
        size_t n = (size_t)(end - cur);
        char name[128];
        if (n >= sizeof(name)) n = sizeof(name) - 1;
        memcpy(name, cur, n);
        name[n] = '\0';
        char *s = name;
        while (*s == ' ') s++;
        char *e2 = s + strlen(s) - 1;
        while (e2 > s && *e2 == ' ') *e2-- = '\0';
        if (s[0]) {
            if (!first) jbuf_append(b, ",");
            jbuf_append_json_str(b, s);
            first = false;
        }
        cur = *end ? end + 1 : end;
    }
    jbuf_append(b, "]");
}

static bool or_env_bool(const char *val) {
    return val && (val[0] == '1' || strcasecmp(val, "true") == 0);
}

/* Builds an OpenAI-compat request then injects OpenRouter-specific fields.
 *
 * Env vars (all optional):
 *   DSCO_OR_TRANSFORMS        — e.g. "middle-out"
 *   DSCO_OR_ROUTE             — e.g. "fallback"
 *   DSCO_OR_PROVIDER_ORDER    — comma-sep provider slugs
 *   DSCO_OR_PROVIDER_ONLY     — comma-sep allowlist
 *   DSCO_OR_PROVIDER_IGNORE   — comma-sep blocklist
 *   DSCO_OR_REQUIRE_PARAMS    — "1"/"true"
 *   DSCO_OR_ALLOW_FALLBACKS   — "0"/"false" to disable (default: true)
 *   DSCO_OR_DATA_COLLECTION   — "deny" to disable
 *   DSCO_OR_ZDR               — "1"/"true" for zero data retention
 *   DSCO_OR_QUANTIZATIONS     — comma-sep: int4,int8,fp6,fp8,fp16,bf16,fp32
 *   DSCO_OR_SORT              — "price", "throughput", or "latency"
 *   DSCO_OR_MAX_PRICE_INPUT   — max $/token for input (e.g. "0.00001")
 *   DSCO_OR_MAX_PRICE_OUTPUT  — max $/token for output
 *   DSCO_OR_FALLBACK_MODELS   — comma-sep model IDs for automatic failover
 *   DSCO_OR_REASONING_EFFORT  — "low", "medium", "high" for reasoning models
 *   DSCO_OR_DEBUG              — "1"/"true" to echo upstream request body
 */
static char *openrouter_build_request(provider_t *p, conversation_t *conv,
                                       session_state_t *session, int max_tokens) {
    char *base = openai_build_request(p, conv, session, max_tokens);
    if (!base) return NULL;

    /* Gather all env config */
    const char *transforms      = getenv("DSCO_OR_TRANSFORMS");
    const char *route           = getenv("DSCO_OR_ROUTE");
    const char *prov_order      = getenv("DSCO_OR_PROVIDER_ORDER");
    const char *prov_only       = getenv("DSCO_OR_PROVIDER_ONLY");
    const char *prov_ignore     = getenv("DSCO_OR_PROVIDER_IGNORE");
    const char *req_params      = getenv("DSCO_OR_REQUIRE_PARAMS");
    const char *allow_fb        = getenv("DSCO_OR_ALLOW_FALLBACKS");
    const char *data_collect    = getenv("DSCO_OR_DATA_COLLECTION");
    const char *zdr             = getenv("DSCO_OR_ZDR");
    const char *quantizations   = getenv("DSCO_OR_QUANTIZATIONS");
    const char *sort_by         = getenv("DSCO_OR_SORT");
    const char *max_price_in    = getenv("DSCO_OR_MAX_PRICE_INPUT");
    const char *max_price_out   = getenv("DSCO_OR_MAX_PRICE_OUTPUT");
    const char *fallback_models = getenv("DSCO_OR_FALLBACK_MODELS");
    const char *reasoning       = getenv("DSCO_OR_REASONING_EFFORT");
    const char *debug_mode      = getenv("DSCO_OR_DEBUG");
    bool disable_thinking       = openrouter_should_disable_thinking(session);

    bool has_provider = prov_order || prov_only || prov_ignore || req_params ||
                        (allow_fb && !or_env_bool(allow_fb)) ||
                        data_collect || zdr || quantizations || sort_by ||
                        max_price_in || max_price_out;
    bool has_extras = transforms || route || has_provider || fallback_models ||
                      reasoning || debug_mode || disable_thinking;

    if (!has_extras) return base;

    /* Strip trailing '}' to append fields */
    size_t len = strlen(base);
    if (len == 0 || base[len - 1] != '}') return base;
    base[len - 1] = '\0';

    jbuf_t b;
    jbuf_init(&b, len + 1024);
    jbuf_append(&b, base);
    free(base);

    /* transforms: ["middle-out"] */
    if (transforms) {
        jbuf_append(&b, ",\"transforms\":");
        or_append_csv_array(&b, transforms);
    }

    /* route: "fallback" */
    if (route) {
        jbuf_append(&b, ",\"route\":");
        jbuf_append_json_str(&b, route);
    }

    /* models: ["model/a", "model/b"] — automatic failover */
    if (fallback_models) {
        jbuf_append(&b, ",\"models\":");
        or_append_csv_array(&b, fallback_models);
    }

    /* reasoning: {"effort": "high"} */
    if (reasoning) {
        jbuf_append(&b, ",\"reasoning\":{\"effort\":");
        jbuf_append_json_str(&b, reasoning);
        jbuf_append(&b, "}");
    }

    /* Kimi/OpenRouter tool calls are more reliable with non-thinking mode
       unless the user explicitly opts into a fixed thinking budget. */
    if (disable_thinking) {
        jbuf_append(&b, ",\"thinking\":{\"type\":\"disabled\"}");
    }

    /* debug: {"echo_upstream_body": true} */
    if (debug_mode && or_env_bool(debug_mode)) {
        jbuf_append(&b, ",\"debug\":{\"echo_upstream_body\":true}");
    }

    /* provider: { ... } */
    if (has_provider) {
        jbuf_append(&b, ",\"provider\":{");
        bool wrote = false;

        if (prov_order) {
            jbuf_append(&b, "\"order\":");
            or_append_csv_array(&b, prov_order);
            wrote = true;
        }
        if (prov_only) {
            if (wrote) jbuf_append(&b, ",");
            jbuf_append(&b, "\"only\":");
            or_append_csv_array(&b, prov_only);
            wrote = true;
        }
        if (prov_ignore) {
            if (wrote) jbuf_append(&b, ",");
            jbuf_append(&b, "\"ignore\":");
            or_append_csv_array(&b, prov_ignore);
            wrote = true;
        }
        if (req_params && or_env_bool(req_params)) {
            if (wrote) jbuf_append(&b, ",");
            jbuf_append(&b, "\"require_parameters\":true");
            wrote = true;
        }
        if (allow_fb && !or_env_bool(allow_fb)) {
            if (wrote) jbuf_append(&b, ",");
            jbuf_append(&b, "\"allow_fallbacks\":false");
            wrote = true;
        }
        if (data_collect) {
            if (wrote) jbuf_append(&b, ",");
            jbuf_append(&b, "\"data_collection\":");
            jbuf_append_json_str(&b, data_collect);
            wrote = true;
        }
        if (zdr && or_env_bool(zdr)) {
            if (wrote) jbuf_append(&b, ",");
            jbuf_append(&b, "\"zdr\":true");
            wrote = true;
        }
        if (quantizations) {
            if (wrote) jbuf_append(&b, ",");
            jbuf_append(&b, "\"quantizations\":");
            or_append_csv_array(&b, quantizations);
            wrote = true;
        }
        if (sort_by) {
            if (wrote) jbuf_append(&b, ",");
            jbuf_append(&b, "\"sort\":");
            jbuf_append_json_str(&b, sort_by);
            wrote = true;
        }
        if (max_price_in || max_price_out) {
            if (wrote) jbuf_append(&b, ",");
            jbuf_append(&b, "\"max_price\":{");
            bool mp_wrote = false;
            if (max_price_in) {
                jbuf_appendf(&b, "\"input\":%s", max_price_in);
                mp_wrote = true;
            }
            if (max_price_out) {
                if (mp_wrote) jbuf_append(&b, ",");
                jbuf_appendf(&b, "\"output\":%s", max_price_out);
            }
            jbuf_append(&b, "}");
            wrote = true;
        }
        (void)wrote;
        jbuf_append(&b, "}");
    }

    jbuf_append(&b, "}");
    return b.data;
}

/* ── OpenAI-compatible Provider ────────────────────────────────────────── */

typedef struct {
    char api_url[512];
} openai_data_t;

/* Check if a message has any tool_use content blocks */
static bool msg_has_tool_use(message_t *m) {
    for (int j = 0; j < m->content_count; j++) {
        if (m->content[j].type && strcmp(m->content[j].type, "tool_use") == 0)
            return true;
    }
    return false;
}

/* Check if a message has any tool_result content blocks */
static bool msg_has_tool_result(message_t *m) {
    for (int j = 0; j < m->content_count; j++) {
        if (m->content[j].type && strcmp(m->content[j].type, "tool_result") == 0)
            return true;
    }
    return false;
}

static const char *openai_last_user_context(conversation_t *conv) {
    if (!conv) return NULL;
    for (int i = conv->count - 1; i >= 0; i--) {
        if (conv->msgs[i].role != ROLE_USER) continue;
        for (int j = 0; j < conv->msgs[i].content_count; j++) {
            if (conv->msgs[i].content[j].text) {
                return conv->msgs[i].content[j].text;
            }
        }
    }
    return NULL;
}

static void openai_append_function_tool(jbuf_t *b, const char *name,
                                        const char *description,
                                        const char *schema_json) {
    jbuf_append(b, "{\"type\":\"function\",\"function\":{\"name\":");
    jbuf_append_json_str(b, name ? name : "");
    jbuf_append(b, ",\"description\":");
    jbuf_append_json_str(b, description ? description : "");
    jbuf_append(b, ",\"parameters\":");
    jbuf_append(b, schema_json ? schema_json : "{}");
    jbuf_append(b, "}}");
}

static bool openai_append_tools_json(jbuf_t *b, conversation_t *conv,
                                     session_state_t *session) {
    int max_tools_send = 128;
    const char *mt_env = getenv("DSCO_OR_MAX_TOOLS");
    if (mt_env && mt_env[0]) {
        max_tools_send = atoi(mt_env);
        if (max_tools_send < 0) max_tools_send = 0;
    } else {
        const char *model = session ? session->model : "";
        if (model && strstr(model, "/")) max_tools_send = 48; /* OpenRouter: tighter cap */
    }

    int filtered_count = 0;
    const tool_def_t **filtered = NULL;
    if (max_tools_send > 0) {
        filtered = tools_get_filtered(openai_last_user_context(conv),
                                      max_tools_send, &filtered_count);
    }

    if (filtered_count <= 0 && g_external_tool_count <= 0) {
        free((void *)filtered);
        return false;
    }

    jbuf_append(b, ",\"tools\":[");
    bool wrote_any = false;
    for (int i = 0; i < filtered_count; i++) {
        if (wrote_any) jbuf_append(b, ",");
        openai_append_function_tool(b, filtered[i]->name,
                                    filtered[i]->description,
                                    filtered[i]->input_schema_json);
        wrote_any = true;
    }
    free((void *)filtered);

    for (int i = 0; i < g_external_tool_count; i++) {
        if (wrote_any) jbuf_append(b, ",");
        openai_append_function_tool(b, g_external_tools[i].name,
                                    g_external_tools[i].description,
                                    g_external_tools[i].input_schema_json);
        wrote_any = true;
    }
    jbuf_append(b, "]");
    return wrote_any;
}

static void openai_append_tool_choice_json(jbuf_t *b, session_state_t *session,
                                           bool has_tools) {
    if (!has_tools) return;

    const char *choice = (session && session->tool_choice[0])
        ? session->tool_choice
        : "auto";

    if (strcmp(choice, "auto") == 0) {
        jbuf_append(b, ",\"tool_choice\":\"auto\"");
    } else if (strcmp(choice, "any") == 0) {
        jbuf_append(b, ",\"tool_choice\":\"required\"");
    } else if (strcmp(choice, "none") == 0) {
        jbuf_append(b, ",\"tool_choice\":\"none\"");
    } else if (strncmp(choice, "tool:", 5) == 0) {
        jbuf_append(b, ",\"tool_choice\":{\"type\":\"function\",\"function\":{\"name\":");
        jbuf_append_json_str(b, choice + 5);
        jbuf_append(b, "}}");
    }
}

static bool openrouter_should_disable_thinking(session_state_t *session) {
    if (!session || !session->model[0]) return false;
    if (session->thinking_budget > 0) return false;
    if (getenv("DSCO_OR_REASONING_EFFORT")) return false;
    if (strstr(session->model, "kimi") && !strstr(session->model, "thinking")) {
        return true;
    }
    return false;
}

/* Emit text+image content array (skipping tool_use and tool_result blocks) */
static void openai_append_text_content(jbuf_t *b, message_t *m) {
    jbuf_t text;
    jbuf_init(&text, 1024);

    for (int j = 0; j < m->content_count; j++) {
        msg_content_t *mc = &m->content[j];
        if (mc->type && strcmp(mc->type, "text") == 0 && mc->text) {
            if (text.len > 0) jbuf_append(&text, "\n");
            jbuf_append(&text, mc->text);
        }
    }

    if (text.len > 0) {
        jbuf_append_json_str(b, text.data);
    } else {
        jbuf_append(b, "\"\"");
    }
    jbuf_free(&text);
}

/* Emit an assistant message with tool_calls in OpenAI format */
static void openai_append_assistant_msg(jbuf_t *b, message_t *m) {
    jbuf_append(b, ",{\"role\":\"assistant\"");

    /* Collect text content */
    jbuf_t text;
    jbuf_init(&text, 1024);
    for (int j = 0; j < m->content_count; j++) {
        msg_content_t *mc = &m->content[j];
        if (mc->type && strcmp(mc->type, "text") == 0 && mc->text) {
            if (text.len > 0) jbuf_append(&text, "\n");
            jbuf_append(&text, mc->text);
        }
    }
    if (text.len > 0) {
        jbuf_append(b, ",\"content\":");
        jbuf_append_json_str(b, text.data);
    }
    jbuf_free(&text);

    /* Emit tool_calls array for tool_use blocks */
    if (msg_has_tool_use(m)) {
        jbuf_append(b, ",\"tool_calls\":[");
        bool first_tool = true;
        for (int j = 0; j < m->content_count; j++) {
            msg_content_t *mc = &m->content[j];
            if (!mc->type || strcmp(mc->type, "tool_use") != 0) continue;
            if (!first_tool) jbuf_append(b, ",");
            first_tool = false;
            jbuf_append(b, "{\"id\":");
            jbuf_append_json_str(b, mc->tool_id ? mc->tool_id : "call_0");
            jbuf_append(b, ",\"type\":\"function\",\"function\":{\"name\":");
            jbuf_append_json_str(b, mc->tool_name ? mc->tool_name : "unknown");
            jbuf_append(b, ",\"arguments\":");
            /* OpenAI/OpenRouter require arguments as a JSON *string*, not object */
            jbuf_append_json_str(b, mc->tool_input ? mc->tool_input : "{}");
            jbuf_append(b, "}}");
        }
        jbuf_append(b, "]");
    }

    jbuf_append(b, "}");
}

/* Emit tool_result blocks as separate {"role":"tool"} messages (OpenAI format) */
static void openai_append_tool_results(jbuf_t *b, message_t *m) {
    for (int j = 0; j < m->content_count; j++) {
        msg_content_t *mc = &m->content[j];
        if (!mc->type || strcmp(mc->type, "tool_result") != 0) continue;
        jbuf_append(b, ",{\"role\":\"tool\",\"tool_call_id\":");
        jbuf_append_json_str(b, mc->tool_id ? mc->tool_id : "call_0");
        jbuf_append(b, ",\"content\":");
        jbuf_append_json_str(b, mc->text ? mc->text : "");
        jbuf_append(b, "}");
    }
}

/* Emit a regular user message (text + images, no tool_results) */
static void openai_append_user_msg(jbuf_t *b, message_t *m) {
    jbuf_append(b, ",{\"role\":\"user\",\"content\":");

    /* Check if we have images */
    bool has_images = false;
    for (int j = 0; j < m->content_count; j++) {
        if (m->content[j].type && strcmp(m->content[j].type, "image") == 0) {
            has_images = true;
            break;
        }
    }

    if (!has_images) {
        /* Simple string content */
        openai_append_text_content(b, m);
    } else {
        /* Array content with text + images */
        jbuf_append(b, "[");
        bool wrote_any = false;

        /* Text block */
        jbuf_t text;
        jbuf_init(&text, 1024);
        for (int j = 0; j < m->content_count; j++) {
            msg_content_t *mc = &m->content[j];
            if (mc->type && strcmp(mc->type, "text") == 0 && mc->text) {
                if (text.len > 0) jbuf_append(&text, "\n");
                jbuf_append(&text, mc->text);
            }
        }
        if (text.len > 0) {
            jbuf_append(b, "{\"type\":\"text\",\"text\":");
            jbuf_append_json_str(b, text.data);
            jbuf_append(b, "}");
            wrote_any = true;
        }
        jbuf_free(&text);

        /* Image blocks */
        for (int j = 0; j < m->content_count; j++) {
            msg_content_t *mc = &m->content[j];
            if (!mc->type || strcmp(mc->type, "image") != 0) continue;
            if (wrote_any) jbuf_append(b, ",");
            jbuf_append(b, "{\"type\":\"image_url\",\"image_url\":{\"url\":");
            if (mc->image_url) {
                jbuf_append_json_str(b, mc->image_url);
            } else {
                const char *media_type = mc->image_media_type ? mc->image_media_type : "image/png";
                const char *data = mc->image_data ? mc->image_data : "";
                jbuf_append(b, "\"data:");
                jbuf_append(b, media_type);
                jbuf_append(b, ";base64,");
                jbuf_append(b, data);
                jbuf_append(b, "\"");
            }
            jbuf_append(b, "}}");
            wrote_any = true;
        }

        if (!wrote_any) {
            jbuf_append(b, "{\"type\":\"text\",\"text\":\"\"}");
        }
        jbuf_append(b, "]");
    }

    jbuf_append(b, "}");
}

static char *openai_build_request(provider_t *p, conversation_t *conv,
                                    session_state_t *session, int max_tokens) {
    openai_data_t *od = (openai_data_t *)p->data;
    (void)od;

    jbuf_t b;
    jbuf_init(&b, 8192);

    jbuf_append(&b, "{\"model\":");
    jbuf_append_json_str(&b, session ? session->model : DEFAULT_MODEL);
    jbuf_append(&b, ",\"max_tokens\":");
    jbuf_append_int(&b, max_tokens);
    jbuf_append(&b, ",\"stream\":true");

    /* System message */
    const char *custom = llm_get_custom_system_prompt();
    jbuf_append(&b, ",\"messages\":[{\"role\":\"system\",\"content\":");
    if (custom) {
        jbuf_t sys;
        jbuf_init(&sys, 4096);
        jbuf_append(&sys, custom);
        jbuf_append(&sys, "\n\n");
        jbuf_append(&sys, SYSTEM_PROMPT);
        jbuf_append_json_str(&b, sys.data);
        jbuf_free(&sys);
    } else {
        jbuf_append_json_str(&b, SYSTEM_PROMPT);
    }
    jbuf_append(&b, "}");

    /* Conversation messages — convert Anthropic format to OpenAI format:
     *   - assistant + tool_use  → {"role":"assistant","tool_calls":[...]}
     *   - user + tool_result    → {"role":"tool","tool_call_id":"...","content":"..."}
     *                             followed by any remaining user text as {"role":"user",...}
     *   - plain user/assistant  → {"role":"user/assistant","content":"..."}
     */
    for (int i = 0; i < conv->count; i++) {
        message_t *m = &conv->msgs[i];

        if (m->role == ROLE_ASSISTANT) {
            openai_append_assistant_msg(&b, m);
        } else {
            /* User message: emit tool_results first, then remaining user content */
            if (msg_has_tool_result(m)) {
                openai_append_tool_results(&b, m);
                /* If there's also text content beyond tool results, emit a user msg */
                bool has_text = false;
                for (int j = 0; j < m->content_count; j++) {
                    msg_content_t *mc = &m->content[j];
                    if (mc->type && strcmp(mc->type, "text") == 0 && mc->text && mc->text[0]) {
                        has_text = true;
                        break;
                    }
                }
                if (has_text) {
                    openai_append_user_msg(&b, m);
                }
            } else {
                openai_append_user_msg(&b, m);
            }
        }
    }
    jbuf_append(&b, "]");

    bool has_tools = openai_append_tools_json(&b, conv, session);
    openai_append_tool_choice_json(&b, session, has_tools);

    jbuf_append(&b, "}");
    return b.data;
}

static struct curl_slist *openai_build_headers(provider_t *p, const char *api_key) {
    (void)p;
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    hdrs = curl_slist_append(hdrs, auth);
    return hdrs;
}

/* ── OpenAI SSE streaming state ─────────────────────────────────────── */

typedef struct {
    bool   used;
    bool   announced;
    int    index;
    char  *tool_name;
    char  *tool_id;
    jbuf_t tool_args;
} oai_tool_call_state_t;

typedef struct {
    jbuf_t          line_buf;
    jbuf_t          text_accum;
    stream_text_cb  text_cb;
    stream_tool_start_cb tool_cb;
    void           *cb_ctx;
    usage_t         usage;
    char           *stop_reason;
    bool            got_error;
    char           *error_msg;
    /* OpenRouter-specific */
    char           *generation_id; /* x-generation-id from response */
    char           *actual_model;  /* model actually used (may differ from requested) */
    double          cost_usd;      /* total cost from usage.cost */
    int             cached_tokens; /* input_tokens_details.cached_tokens */
    int             reasoning_tokens; /* output_tokens_details.reasoning_tokens */
    oai_tool_call_state_t tool_calls[MAX_CONTENT_BLOCKS];
    int             tool_slots_used;
    /* Result building */
    content_block_t blocks[MAX_CONTENT_BLOCKS];
    int             block_count;
} oai_sse_state_t;

static oai_tool_call_state_t *oai_tool_state_find_by_id(oai_sse_state_t *s, const char *tool_id) {
    if (!tool_id || !tool_id[0]) return NULL;
    for (int i = 0; i < s->tool_slots_used; i++) {
        oai_tool_call_state_t *slot = &s->tool_calls[i];
        if (slot->used && slot->tool_id && strcmp(slot->tool_id, tool_id) == 0) {
            return slot;
        }
    }
    return NULL;
}

static oai_tool_call_state_t *oai_tool_state_for(oai_sse_state_t *s, int index,
                                                 const char *tool_id) {
    oai_tool_call_state_t *slot = NULL;

    if (index >= 0 && index < MAX_CONTENT_BLOCKS) {
        slot = &s->tool_calls[index];
        if (!slot->used) {
            memset(slot, 0, sizeof(*slot));
            slot->used = true;
            slot->index = index;
            jbuf_init(&slot->tool_args, 256);
            if (index + 1 > s->tool_slots_used) s->tool_slots_used = index + 1;
        }
        return slot;
    }

    slot = oai_tool_state_find_by_id(s, tool_id);
    if (slot) return slot;

    if (s->tool_slots_used >= MAX_CONTENT_BLOCKS) return NULL;
    slot = &s->tool_calls[s->tool_slots_used];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->index = s->tool_slots_used;
    jbuf_init(&slot->tool_args, 256);
    s->tool_slots_used++;
    return slot;
}

static const char *oai_tool_state_id(oai_tool_call_state_t *slot) {
    static char fallback[32];
    if (slot && slot->tool_id && slot->tool_id[0]) return slot->tool_id;
    snprintf(fallback, sizeof(fallback), "call_%d", slot ? slot->index : 0);
    return fallback;
}

typedef struct {
    oai_sse_state_t *state;
} oai_tool_delta_ctx_t;

static void oai_handle_tool_call_delta(const char *tc_elem, void *ctx) {
    oai_tool_delta_ctx_t *tool_ctx = (oai_tool_delta_ctx_t *)ctx;
    oai_sse_state_t *s = tool_ctx->state;

    char *idx_raw = json_get_raw(tc_elem, "index");
    int idx = idx_raw ? atoi(idx_raw) : -1;
    free(idx_raw);

    char *tid = json_get_str(tc_elem, "id");
    oai_tool_call_state_t *slot = oai_tool_state_for(s, idx, tid);
    if (!slot) {
        free(tid);
        return;
    }
    if (tid && (!slot->tool_id || !slot->tool_id[0])) {
        slot->tool_id = tid;
        tid = NULL;
    }
    free(tid);

    char *fn_raw = json_get_raw(tc_elem, "function");
    if (!fn_raw) return;

    char *fname = json_get_str(fn_raw, "name");
    if (fname && (!slot->tool_name || !slot->tool_name[0])) {
        slot->tool_name = fname;
        fname = NULL;
    }
    free(fname);

    char *fargs = json_get_str(fn_raw, "arguments");
    if (fargs && fargs[0]) {
        jbuf_append(&slot->tool_args, fargs);
    }
    free(fargs);
    free(fn_raw);

    if (slot->tool_name && !slot->announced) {
        slot->announced = true;
        if (s->tool_cb) {
            s->tool_cb(slot->tool_name, oai_tool_state_id(slot), s->cb_ctx);
        }
    }
}

static void oai_handle_sse_line(oai_sse_state_t *s, const char *line) {
    if (strncmp(line, "data: ", 6) != 0) return;
    const char *data = line + 6;
    if (strcmp(data, "[DONE]") == 0) return;

    /* Check for top-level error object (OpenRouter sends errors mid-stream) */
    char *err_raw = json_get_raw(data, "error");
    if (err_raw) {
        char *err_msg = json_get_str(err_raw, "message");
        int err_code = json_get_int(err_raw, "code", 0);
        if (err_msg) {
            s->got_error = true;
            free(s->error_msg);
            s->error_msg = err_msg;
            fprintf(stderr, "  \033[31mAPI error %d: %s\033[0m\n", err_code, err_msg);
        }
        free(err_raw);
        return;
    }

    /* Extract model actually used (first chunk usually has it) */
    if (!s->actual_model) {
        char *model = json_get_str(data, "model");
        if (model) s->actual_model = model;
    }

    /* Extract generation ID */
    if (!s->generation_id) {
        char *gid = json_get_str(data, "id");
        if (gid) s->generation_id = gid;
    }

    /* Parse usage (may appear in any chunk, usually the last) */
    char *usage_raw = json_get_raw(data, "usage");
    if (usage_raw) {
        s->usage.input_tokens = json_get_int(usage_raw, "prompt_tokens", s->usage.input_tokens);
        s->usage.output_tokens = json_get_int(usage_raw, "completion_tokens", s->usage.output_tokens);

        /* Cost tracking (OpenRouter includes cost in usage) */
        char *cost_str = json_get_str(usage_raw, "cost");
        if (cost_str) {
            s->cost_usd = atof(cost_str);
            free(cost_str);
        }

        /* Token detail breakdowns */
        char *in_detail = json_get_raw(usage_raw, "input_tokens_details");
        if (in_detail) {
            s->cached_tokens = json_get_int(in_detail, "cached_tokens", 0);
            free(in_detail);
        }
        char *out_detail = json_get_raw(usage_raw, "output_tokens_details");
        if (out_detail) {
            s->reasoning_tokens = json_get_int(out_detail, "reasoning_tokens", 0);
            free(out_detail);
        }
        free(usage_raw);
    }

    /* Parse choices array — extract the first element (an object) */
    char *choices_raw = json_get_raw(data, "choices");
    if (!choices_raw) return;
    /* choices_raw is "[{...}]" — skip into the first element object */
    const char *first_choice = choices_raw;
    while (*first_choice && (*first_choice == '[' || *first_choice == ' ' ||
           *first_choice == '\n' || *first_choice == '\r' || *first_choice == '\t'))
        first_choice++;

    /* Check finish_reason (including mid-stream errors and content filters) */
    char *fr = json_get_str(first_choice, "finish_reason");
    if (fr) {
        free(s->stop_reason);
        if (strcmp(fr, "stop") == 0)
            s->stop_reason = safe_strdup("end_turn");
        else if (strcmp(fr, "tool_calls") == 0)
            s->stop_reason = safe_strdup("tool_use");
        else if (strcmp(fr, "length") == 0)
            s->stop_reason = safe_strdup("max_tokens");
        else if (strcmp(fr, "error") == 0) {
            s->stop_reason = safe_strdup("error");
            s->got_error = true;
        } else if (strcmp(fr, "content_filter") == 0) {
            s->stop_reason = safe_strdup("content_filter");
            fprintf(stderr, "  \033[33mContent filter triggered\033[0m\n");
        } else {
            s->stop_reason = fr;
            fr = NULL; /* transferred ownership */
        }
        free(fr);
    }

    /* Extract delta from first choice */
    char *delta_raw = json_get_raw(first_choice, "delta");
    if (!delta_raw) {
        free(choices_raw);
        return;
    }

    /* Content delta — streaming text */
    char *content = json_get_str(delta_raw, "content");
    if (content && content[0]) {
        jbuf_append(&s->text_accum, content);
        if (s->text_cb) s->text_cb(content, s->cb_ctx);
    }
    free(content);

    /* Tool calls delta — accumulate every streamed call by index/id. */
    char *tool_calls_raw = json_get_raw(delta_raw, "tool_calls");
    if (tool_calls_raw) {
        oai_tool_delta_ctx_t tool_ctx = { .state = s };
        jbuf_t wrapped;
        jbuf_init(&wrapped, strlen(tool_calls_raw) + 32);
        jbuf_append(&wrapped, "{\"tool_calls\":");
        jbuf_append(&wrapped, tool_calls_raw);
        jbuf_append(&wrapped, "}");
        json_array_foreach(wrapped.data, "tool_calls",
                           oai_handle_tool_call_delta, &tool_ctx);
        jbuf_free(&wrapped);
        free(tool_calls_raw);
    }

    free(delta_raw);
    free(choices_raw);
}

static size_t oai_sse_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    oai_sse_state_t *s = (oai_sse_state_t *)userdata;

    const char *p = (const char *)ptr;
    for (size_t i = 0; i < total; i++) {
        if (p[i] == '\n') {
            if (s->line_buf.len > 0) {
                oai_handle_sse_line(s, s->line_buf.data);
                jbuf_reset(&s->line_buf);
            }
        } else if (p[i] != '\r') {
            jbuf_append_char(&s->line_buf, p[i]);
        }
    }
    return total;
}

static stream_result_t openai_stream(provider_t *p, const char *api_key,
                                       const char *request_json,
                                       stream_text_cb text_cb,
                                       stream_tool_start_cb tool_cb,
                                       stream_thinking_cb thinking_cb,
                                       void *cb_ctx) {
    (void)thinking_cb;
    openai_data_t *od = (openai_data_t *)p->data;
    stream_result_t result = {0};

    CURL *curl = curl_easy_init();
    if (!curl) { result.ok = false; return result; }

    struct curl_slist *hdrs = p->build_headers
        ? p->build_headers(p, api_key)
        : openai_build_headers(p, api_key);
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");

    oai_sse_state_t state = {0};
    jbuf_init(&state.line_buf, 4096);
    jbuf_init(&state.text_accum, 4096);
    state.text_cb = text_cb;
    state.tool_cb = tool_cb;
    state.cb_ctx = cb_ctx;

    curl_easy_setopt(curl, CURLOPT_URL, od->api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, oai_sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 100L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    result.http_status = (int)http_code;

    if (res == CURLE_OK && http_code == 200) {
        result.ok = true;

        /* Finalize every accumulated tool call in index order. */
        for (int i = 0; i < state.tool_slots_used; i++) {
            oai_tool_call_state_t *slot = &state.tool_calls[i];
            if (!slot->used || !slot->tool_name) continue;
            int bi = state.block_count++;
            if (bi < MAX_CONTENT_BLOCKS) {
                state.blocks[bi].type = safe_strdup("tool_use");
                state.blocks[bi].tool_name = safe_strdup(slot->tool_name);
                state.blocks[bi].tool_id = safe_strdup(oai_tool_state_id(slot));
                state.blocks[bi].tool_input = safe_strdup(
                    (slot->tool_args.data && slot->tool_args.data[0])
                        ? slot->tool_args.data
                        : "{}");
            }
        }

        /* Add text block if we accumulated text */
        if (state.text_accum.len > 0) {
            int bi = state.block_count++;
            if (bi < MAX_CONTENT_BLOCKS) {
                state.blocks[bi].type = safe_strdup("text");
                state.blocks[bi].text = safe_strdup(state.text_accum.data);
            }
        }

        /* Build result */
        result.parsed.count = state.block_count;
        result.parsed.blocks = safe_malloc(
            (state.block_count > 0 ? state.block_count : 1) * sizeof(content_block_t));
        memcpy(result.parsed.blocks, state.blocks,
               state.block_count * sizeof(content_block_t));
        result.parsed.stop_reason = state.stop_reason;
        result.usage = state.usage;

        /* Log model/cost info when available (OpenRouter provides these) */
        if (state.actual_model || state.cost_usd > 0) {
            fprintf(stderr, "  \033[2m");
            if (state.actual_model)
                fprintf(stderr, "model=%s ", state.actual_model);
            if (state.cost_usd > 0)
                fprintf(stderr, "$%.6f ", state.cost_usd);
            if (state.cached_tokens > 0)
                fprintf(stderr, "cached=%d ", state.cached_tokens);
            if (state.reasoning_tokens > 0)
                fprintf(stderr, "reasoning=%d ", state.reasoning_tokens);
            if (state.generation_id)
                fprintf(stderr, "gen=%s", state.generation_id);
            fprintf(stderr, "\033[0m\n");
        }

        /* Handle mid-stream errors that arrived on HTTP 200 */
        if (state.got_error) {
            result.ok = false;
            if (state.error_msg)
                fprintf(stderr, "dsco: stream error: %s\n", state.error_msg);
        }
    } else {
        result.ok = false;
        if (res != CURLE_OK) {
            fprintf(stderr, "dsco: curl error: %s (HTTP %d, url: %s)\n",
                    curl_easy_strerror(res), (int)http_code, od->api_url);
        } else if (state.got_error && state.error_msg) {
            fprintf(stderr, "dsco: HTTP %d: %s\n", (int)http_code, state.error_msg);
        } else if (state.line_buf.len > 0) {
            fprintf(stderr, "dsco: HTTP %d: %.*s\n",
                    (int)http_code, (int)(state.line_buf.len < 500 ? state.line_buf.len : 500),
                    state.line_buf.data);
        } else {
            fprintf(stderr, "dsco: request failed HTTP %d (url: %s)\n",
                    (int)http_code, od->api_url);
        }
        free(state.stop_reason);
    }

    /* Cleanup OpenRouter-specific state */
    free(state.error_msg);
    free(state.actual_model);
    free(state.generation_id);
    for (int i = 0; i < state.tool_slots_used; i++) {
        free(state.tool_calls[i].tool_name);
        free(state.tool_calls[i].tool_id);
        jbuf_free(&state.tool_calls[i].tool_args);
    }
    jbuf_free(&state.line_buf);
    jbuf_free(&state.text_accum);

    return result;
}

/* ── Provider endpoint table ───────────────────────────────────────────── */

typedef struct {
    const char *name;       /* provider key */
    const char *base_url;   /* API base URL */
    const char *env_key;    /* API key environment variable */
    const char *key_header; /* header format: "Authorization: Bearer" or "x-api-key:" */
} provider_endpoint_t;

static const provider_endpoint_t PROVIDER_ENDPOINTS[] = {
    { "openai",     "https://api.openai.com/v1",               "OPENAI_API_KEY",     "Bearer" },
    { "groq",       "https://api.groq.com/openai/v1",          "GROQ_API_KEY",       "Bearer" },
    { "deepseek",   "https://api.deepseek.com/v1",             "DEEPSEEK_API_KEY",   "Bearer" },
    { "together",   "https://api.together.xyz/v1",             "TOGETHER_API_KEY",   "Bearer" },
    { "mistral",    "https://api.mistral.ai/v1",               "MISTRAL_API_KEY",    "Bearer" },
    { "openrouter", "https://openrouter.ai/api/v1",            "OPENROUTER_API_KEY", "Bearer" },
    { "perplexity", "https://api.perplexity.ai",               "PERPLEXITY_API_KEY", "Bearer" },
    { "cerebras",   "https://api.cerebras.ai/v1",              "CEREBRAS_API_KEY",   "Bearer" },
    { "xai",        "https://api.x.ai/v1",                     "XAI_API_KEY",        "Bearer" },
    { "cohere",     "https://api.cohere.com/v2",               "COHERE_API_KEY",     "Bearer" },
    { NULL, NULL, NULL, NULL }
};

static const provider_endpoint_t *find_endpoint(const char *name) {
    for (int i = 0; PROVIDER_ENDPOINTS[i].name; i++) {
        if (strcmp(name, PROVIDER_ENDPOINTS[i].name) == 0)
            return &PROVIDER_ENDPOINTS[i];
    }
    return NULL;
}

/* ── Provider factory ──────────────────────────────────────────────────── */

static provider_t *create_openai_compat(const char *name, const char *base_url,
                                           const char *env_key) {
    provider_t *p = safe_malloc(sizeof(provider_t));
    memset(p, 0, sizeof(*p));
    p->name = name;
    openai_data_t *od = safe_malloc(sizeof(openai_data_t));

    /* Check for env override */
    char env_base[128];
    snprintf(env_base, sizeof(env_base), "%s_API_BASE", name);
    /* Convert to uppercase */
    for (char *c = env_base; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    const char *custom_base = getenv(env_base);
    if (!custom_base) custom_base = base_url;

    snprintf(od->api_url, sizeof(od->api_url), "%s/chat/completions", custom_base);
    p->api_url = od->api_url;
    p->data = od;
    p->build_request = openai_build_request;
    p->build_headers = openai_build_headers;
    p->stream = openai_stream;
    (void)env_key; /* key is resolved at request time via detect */
    return p;
}

provider_t *provider_create(const char *name) {
    if (strcmp(name, "anthropic") == 0 || strcmp(name, "claude") == 0) {
        provider_t *p = safe_malloc(sizeof(provider_t));
        memset(p, 0, sizeof(*p));
        p->name = "anthropic";
        p->api_url = API_URL_ANTHROPIC;
        p->build_request = anthropic_build_request;
        p->build_headers = anthropic_build_headers;
        p->stream = anthropic_stream;
        return p;
    }

    /* OpenRouter gets its own header/request builders */
    if (strcmp(name, "openrouter") == 0) {
        const provider_endpoint_t *ep = find_endpoint("openrouter");
        provider_t *p = create_openai_compat(ep->name, ep->base_url, ep->env_key);
        p->build_headers = openrouter_build_headers;
        p->build_request = openrouter_build_request;
        return p;
    }

    /* All other providers use OpenAI-compatible API */
    const provider_endpoint_t *ep = find_endpoint(name);
    if (ep) {
        return create_openai_compat(ep->name, ep->base_url, ep->env_key);
    }

    /* Fallback: treat as OpenAI-compatible with custom base */
    return create_openai_compat(name, "https://api.openai.com/v1", "OPENAI_API_KEY");
}

void provider_free(provider_t *p) {
    if (!p) return;
    free(p->data);
    free(p);
}

/* ── Provider detection from model name ────────────────────────────────── */

const char *provider_detect(const char *model, const char *api_key) {
    if (!model && !api_key) return "anthropic";

    if (model) {
        /* Explicit provider prefix: "openrouter:model/id" */
        if (strncmp(model, "openrouter:", 11) == 0)
            return "openrouter";
        /* OpenRouter auto-router */
        if (strncmp(model, "openrouter/", 11) == 0 || strcmp(model, "auto") == 0)
            return "openrouter";
        /* Any org/model ID with a slash routes to OpenRouter FIRST —
         * this must come before keyword checks (e.g. "openai/gpt-5.4"
         * contains "gpt" but must go through OpenRouter, not native OpenAI). */
        if (strstr(model, "/"))
            return "openrouter";
        /* Anthropic — bare model IDs only (no slash) */
        if (strstr(model, "claude") || strstr(model, "opus") ||
            strstr(model, "sonnet") || strstr(model, "haiku"))
            return "anthropic";
        /* OpenAI — bare model IDs only */
        if (strstr(model, "gpt") || strncmp(model, "o1", 2) == 0 ||
            strncmp(model, "o3", 2) == 0 || strncmp(model, "o4", 2) == 0 ||
            strstr(model, "codex-spark") || strstr(model, "chatgpt"))
            return "openai";
        /* Groq — only when no slash (native model IDs have no org prefix) */
        if (!strstr(model, "/") &&
            (strstr(model, "llama") || strstr(model, "mixtral") ||
             strstr(model, "gemma")))
            return "groq";
        /* DeepSeek native */
        if (!strstr(model, "/") && strstr(model, "deepseek"))
            return "deepseek";
        /* Mistral native */
        if (!strstr(model, "/") &&
            (strstr(model, "mistral") || strstr(model, "codestral") ||
             strstr(model, "pixtral")))
            return "mistral";
        /* Together native */
        if (!strstr(model, "/") &&
            (strstr(model, "Qwen") || strstr(model, "together")))
            return "together";
        /* Cohere */
        if (strstr(model, "command"))
            return "cohere";
        /* xAI — native only for bare "grok" IDs, x-ai/ prefix goes to OpenRouter */
        if (strstr(model, "grok") && !strstr(model, "/"))
            return "xai";
        /* Perplexity */
        if (strstr(model, "sonar") || strstr(model, "pplx"))
            return "perplexity";
        /* Cerebras */
        if (strstr(model, "cerebras"))
            return "cerebras";
        /* Any remaining slash-based model IDs already caught above */
    }

    /* Check API key patterns */
    if (api_key) {
        if (strncmp(api_key, "sk-ant-", 7) == 0) return "anthropic";
        if (strncmp(api_key, "gsk_", 4) == 0) return "groq";
        if (strncmp(api_key, "sk-or-", 6) == 0) return "openrouter";
        if (strncmp(api_key, "pplx-", 5) == 0) return "perplexity";
        if (strncmp(api_key, "sk-", 3) == 0) return "openai";
    }

    return "anthropic";
}

/* ── Resolve API key for a provider ────────────────────────────────────── */

const char *provider_resolve_api_key(const char *provider_name) {
    if (strcmp(provider_name, "anthropic") == 0)
        return getenv("ANTHROPIC_API_KEY");

    const provider_endpoint_t *ep = find_endpoint(provider_name);
    if (ep) return getenv(ep->env_key);

    return getenv("OPENAI_API_KEY");
}
