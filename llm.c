#include "llm.h"
#include "tools.h"
#include "semantic.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <curl/curl.h>

/* Global interrupt flag — set by SIGINT handler in agent.c.
   Declared extern here so the streaming code can check it. */
extern volatile int g_interrupted;

/* ── Semantic tool index (initialized lazily) ──────────────────────────── */
static tfidf_index_t  s_tool_index;
static bool           s_tool_index_built = false;

static void ensure_tool_index(void) {
    if (s_tool_index_built) return;
    int count;
    const tool_def_t *tools = tools_get_all(&count);

    const char *names[SEM_MAX_DOCS];
    const char *descs[SEM_MAX_DOCS];
    for (int i = 0; i < count && i < SEM_MAX_DOCS; i++) {
        names[i] = tools[i].name;
        descs[i] = tools[i].description;
    }
    sem_tools_index_build(&s_tool_index, names, descs, count);
    s_tool_index_built = true;
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
        }
        free(c->msgs[i].content);
    }
    free(c->msgs);
    c->msgs = NULL;
    c->count = c->cap = 0;
}

static message_t *conv_add(conversation_t *c, msg_role_t role) {
    if (c->count >= MAX_MESSAGES) {
        fprintf(stderr, "dsco: conversation limit reached (%d messages), dropping oldest\n", MAX_MESSAGES);
        /* Free the oldest message */
        message_t *old = &c->msgs[0];
        for (int j = 0; j < old->content_count; j++) {
            free(old->content[j].type);
            free(old->content[j].text);
            free(old->content[j].tool_name);
            free(old->content[j].tool_id);
            free(old->content[j].tool_input);
        }
        free(old->content);
        memmove(&c->msgs[0], &c->msgs[1], (c->count - 1) * sizeof(message_t));
        c->count--;
    }
    if (c->count >= c->cap) {
        c->cap *= 2;
        if (c->cap > MAX_MESSAGES) c->cap = MAX_MESSAGES;
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
                int len = (int)strlen(mc->text);
                if (len > max_chars) {
                    char *trimmed = safe_malloc(max_chars + 64);
                    snprintf(trimmed, max_chars + 64,
                             "[truncated %d→%d chars] %.*s",
                             len, max_chars, max_chars, mc->text);
                    free(mc->text);
                    mc->text = trimmed;
                }
            }
        }
    }
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

void conv_add_tool_result(conversation_t *c, const char *tool_id,
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
    mc->text = safe_strdup(result);
    mc->is_error = is_error;
}

void conv_add_assistant_raw(conversation_t *c, parsed_response_t *resp) {
    message_t *m = conv_add(c, ROLE_ASSISTANT);
    for (int i = 0; i < resp->count; i++) {
        msg_content_t *mc = msg_add_content(m);
        mc->type = safe_strdup(resp->blocks[i].type);
        if (resp->blocks[i].text) mc->text = safe_strdup(resp->blocks[i].text);
        if (resp->blocks[i].tool_name) mc->tool_name = safe_strdup(resp->blocks[i].tool_name);
        if (resp->blocks[i].tool_id) mc->tool_id = safe_strdup(resp->blocks[i].tool_id);
        if (resp->blocks[i].tool_input) mc->tool_input = safe_strdup(resp->blocks[i].tool_input);
    }
}

/* ── Build JSON request ────────────────────────────────────────────────── */

static void append_content_block(jbuf_t *b, msg_content_t *mc) {
    if (strcmp(mc->type, "text") == 0) {
        jbuf_append(b, "{\"type\":\"text\",\"text\":");
        jbuf_append_json_str(b, mc->text ? mc->text : "");
        jbuf_append(b, "}");
    } else if (strcmp(mc->type, "tool_use") == 0) {
        jbuf_append(b, "{\"type\":\"tool_use\",\"id\":");
        jbuf_append_json_str(b, mc->tool_id);
        jbuf_append(b, ",\"name\":");
        jbuf_append_json_str(b, mc->tool_name);
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
    } else if (strcmp(mc->type, "tool_result") == 0) {
        jbuf_append(b, "{\"type\":\"tool_result\",\"tool_use_id\":");
        jbuf_append_json_str(b, mc->tool_id);
        if (mc->is_error) jbuf_append(b, ",\"is_error\":true");
        jbuf_append(b, ",\"content\":");
        jbuf_append_json_str(b, mc->text ? mc->text : "");
        jbuf_append(b, "}");
    }
}

/* Essential tools always included regardless of semantic score */
static const char *ALWAYS_INCLUDE_TOOLS[] = {
    "bash", "run_command", "read_file", "write_file", "edit_file",
    "list_directory", "find_files", "grep_files",
    "spawn_agent", "create_swarm", "swarm_collect", "agent_status",
    "self_inspect", "inspect_file",
    NULL
};

static bool is_always_included(const char *name) {
    for (int i = 0; ALWAYS_INCLUDE_TOOLS[i]; i++) {
        if (strcmp(name, ALWAYS_INCLUDE_TOOLS[i]) == 0) return true;
    }
    return false;
}

/* Append ALL tools (used for first turn or when no context available) */
static void append_tools_json_all(jbuf_t *b) {
    int count;
    const tool_def_t *tools = tools_get_all(&count);
    jbuf_append(b, ",\"tools\":[");
    for (int i = 0; i < count; i++) {
        if (i > 0) jbuf_append(b, ",");
        jbuf_append(b, "{\"name\":");
        jbuf_append_json_str(b, tools[i].name);
        jbuf_append(b, ",\"description\":");
        jbuf_append_json_str(b, tools[i].description);
        jbuf_append(b, ",\"input_schema\":");
        jbuf_append(b, tools[i].input_schema_json);
        if (i == count - 1) {
            jbuf_append(b, ",\"cache_control\":{\"type\":\"ephemeral\"}");
        }
        jbuf_append(b, "}");
    }
    jbuf_append(b, "]");
}

/* Append semantically-selected tools based on query relevance.
   Includes essential tools + top-K relevant tools by BM25+cosine score.
   Max tools = SEM_TOOL_BUDGET (default 40). */
#define SEM_TOOL_BUDGET 40

static void append_tools_json_semantic(jbuf_t *b, const char *query) {
    ensure_tool_index();

    int count;
    const tool_def_t *tools = tools_get_all(&count);

    /* Rank tools by relevance to query */
    tool_score_t scores[SEM_MAX_DOCS];
    int ranked = sem_tools_rank(&s_tool_index, query, scores, count, count);

    /* Build inclusion set: essential + top-K by score */
    bool include[SEM_MAX_DOCS];
    memset(include, 0, sizeof(bool) * count);

    /* Always include essential tools */
    int included = 0;
    for (int i = 0; i < count; i++) {
        if (is_always_included(tools[i].name)) {
            include[i] = true;
            included++;
        }
    }

    /* Add top-ranked tools up to budget */
    for (int r = 0; r < ranked && included < SEM_TOOL_BUDGET; r++) {
        int ti = scores[r].tool_index;
        if (!include[ti]) {
            include[ti] = true;
            included++;
        }
    }

    /* Serialize selected tools */
    jbuf_append(b, ",\"tools\":[");
    int written = 0;
    int last_written = -1;
    for (int i = 0; i < count; i++) {
        if (!include[i]) continue;
        if (written > 0) jbuf_append(b, ",");
        jbuf_append(b, "{\"name\":");
        jbuf_append_json_str(b, tools[i].name);
        jbuf_append(b, ",\"description\":");
        jbuf_append_json_str(b, tools[i].description);
        jbuf_append(b, ",\"input_schema\":");
        jbuf_append(b, tools[i].input_schema_json);
        jbuf_append(b, "}");
        last_written = i;
        written++;
    }
    /* Add cache_control to the last tool */
    if (last_written >= 0 && written > 0) {
        /* Back up over closing "}" to add cache_control */
        b->len--;
        b->data[b->len] = '\0';
        jbuf_append(b, ",\"cache_control\":{\"type\":\"ephemeral\"}}");
    }
    jbuf_append(b, "]");

    /* Log tool selection info */
    fprintf(stderr, "  %s[semantic: %d/%d tools selected]%s\n",
            "\033[2m", written, count, "\033[0m");
}

char *llm_build_request(conversation_t *c, const char *model, int max_tokens) {
    jbuf_t b;
    jbuf_init(&b, 16384);

    jbuf_append(&b, "{\"model\":");
    jbuf_append_json_str(&b, model);
    jbuf_append(&b, ",\"max_tokens\":");
    jbuf_append_int(&b, max_tokens);
    jbuf_append(&b, ",\"stream\":true");

    /* System prompt with cache breakpoint */
    jbuf_append(&b, ",\"system\":[{\"type\":\"text\",\"text\":");
    jbuf_append_json_str(&b, SYSTEM_PROMPT);
    jbuf_append(&b, ",\"cache_control\":{\"type\":\"ephemeral\"}}]");

    append_tools_json_all(&b);

    /* Messages — merge consecutive same-role messages for API compliance */
    jbuf_append(&b, ",\"messages\":[");
    int msg_written = 0;
    for (int i = 0; i < c->count; i++) {
        message_t *m = &c->msgs[i];
        /* Check if this message has the same role as the previous;
           if so, merge content blocks into the previous message object */
        bool merge_with_prev = (i > 0 && c->msgs[i - 1].role == m->role && msg_written > 0);
        if (merge_with_prev) {
            /* Back up over the closing "]}" of the previous message */
            b.len -= 2;
            b.data[b.len] = '\0';
            /* Append a comma and the new content blocks */
            for (int j = 0; j < m->content_count; j++) {
                jbuf_append(&b, ",");
                append_content_block(&b, &m->content[j]);
            }
            jbuf_append(&b, "]}");
        } else {
            if (msg_written > 0) jbuf_append(&b, ",");
            jbuf_append(&b, "{\"role\":");
            jbuf_append_json_str(&b, m->role == ROLE_USER ? "user" : "assistant");
            jbuf_append(&b, ",\"content\":[");
            for (int j = 0; j < m->content_count; j++) {
                if (j > 0) jbuf_append(&b, ",");
                append_content_block(&b, &m->content[j]);
            }
            jbuf_append(&b, "]}");
            msg_written++;
        }
    }
    jbuf_append(&b, "]}");

    return b.data;
}

char *llm_build_request_semantic(conversation_t *c, const char *model,
                                  int max_tokens, const char *query_hint) {
    jbuf_t b;
    jbuf_init(&b, 16384);

    jbuf_append(&b, "{\"model\":");
    jbuf_append_json_str(&b, model);
    jbuf_append(&b, ",\"max_tokens\":");
    jbuf_append_int(&b, max_tokens);
    jbuf_append(&b, ",\"stream\":true");

    /* System prompt with cache breakpoint */
    jbuf_append(&b, ",\"system\":[{\"type\":\"text\",\"text\":");
    jbuf_append_json_str(&b, SYSTEM_PROMPT);
    jbuf_append(&b, ",\"cache_control\":{\"type\":\"ephemeral\"}}]");

    /* Semantic tool selection: pick relevant tools based on query */
    if (query_hint && query_hint[0]) {
        append_tools_json_semantic(&b, query_hint);
    } else {
        append_tools_json_all(&b);
    }

    /* Messages — merge consecutive same-role messages for API compliance */
    jbuf_append(&b, ",\"messages\":[");
    int msg_written = 0;
    for (int i = 0; i < c->count; i++) {
        message_t *m = &c->msgs[i];
        bool merge_with_prev = (i > 0 && c->msgs[i - 1].role == m->role && msg_written > 0);
        if (merge_with_prev) {
            b.len -= 2;
            b.data[b.len] = '\0';
            for (int j = 0; j < m->content_count; j++) {
                jbuf_append(&b, ",");
                append_content_block(&b, &m->content[j]);
            }
            jbuf_append(&b, "]}");
        } else {
            if (msg_written > 0) jbuf_append(&b, ",");
            jbuf_append(&b, "{\"role\":");
            jbuf_append_json_str(&b, m->role == ROLE_USER ? "user" : "assistant");
            jbuf_append(&b, ",\"content\":[");
            for (int j = 0; j < m->content_count; j++) {
                if (j > 0) jbuf_append(&b, ",");
                append_content_block(&b, &m->content[j]);
            }
            jbuf_append(&b, "]}");
            msg_written++;
        }
    }
    jbuf_append(&b, "]}");

    return b.data;
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

    /* Callbacks */
    stream_text_cb       text_cb;
    stream_tool_start_cb tool_cb;
    void                *cb_ctx;

    /* SSE line buffer */
    jbuf_t          line_buf;
    bool            got_error;
    char           *error_msg;

    /* Repetition detection */
    char            rep_window[256];   /* sliding window of recent text */
    int             rep_window_len;
    int             rep_score;         /* consecutive repetition hits */
    bool            rep_abort;         /* set true to abort stream */
} sse_state_t;

/* Detect repetitive output by checking if recent text repeats a short pattern.
   Returns true if the stream should be aborted. */
static void rep_detect_feed(sse_state_t *s, const char *text) {
    int tlen = (int)strlen(text);
    for (int i = 0; i < tlen; i++) {
        if (s->rep_window_len < (int)sizeof(s->rep_window) - 1) {
            s->rep_window[s->rep_window_len++] = text[i];
        } else {
            /* Shift window left by half */
            int half = s->rep_window_len / 2;
            memmove(s->rep_window, s->rep_window + half, s->rep_window_len - half);
            s->rep_window_len -= half;
            s->rep_window[s->rep_window_len++] = text[i];
        }
    }
    s->rep_window[s->rep_window_len] = '\0';

    /* Check for repeating pattern: if any substring of length 4-30
       repeats 6+ times consecutively in the window, flag it */
    if (s->rep_window_len < 32) return;

    for (int plen = 4; plen <= 30 && plen * 6 <= s->rep_window_len; plen++) {
        const char *pat = s->rep_window + s->rep_window_len - plen;
        int reps = 0;
        int pos = s->rep_window_len - plen;
        while (pos >= plen) {
            pos -= plen;
            if (memcmp(s->rep_window + pos, pat, plen) == 0) {
                reps++;
            } else {
                break;
            }
        }
        if (reps >= 5) {
            s->rep_abort = true;
            fprintf(stderr, "\n\033[33m⚠ repetition detected, aborting stream\033[0m\n");
            return;
        }
    }
}

/* Curl progress callback — allows Ctrl+C to abort transfers */
static int stream_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                                curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    sse_state_t *s = (sse_state_t *)clientp;
    if (g_interrupted || s->rep_abort) return 1;  /* non-zero aborts transfer */
    return 0;
}

static void sse_finalize_block(sse_state_t *s) {
    if (s->current_index < 0) return;
    int idx = s->block_count++;
    if (idx >= MAX_CONTENT_BLOCKS) return;

    content_block_t *blk = &s->blocks[idx];
    blk->type = s->cur_type ? safe_strdup(s->cur_type) : safe_strdup("text");

    if (s->cur_type && strcmp(s->cur_type, "text") == 0) {
        blk->text = safe_strdup(s->text_buf.data ? s->text_buf.data : "");
    } else if (s->cur_type && strcmp(s->cur_type, "tool_use") == 0) {
        blk->tool_name = s->cur_tool_name ? safe_strdup(s->cur_tool_name) : NULL;
        blk->tool_id = s->cur_tool_id ? safe_strdup(s->cur_tool_id) : NULL;
        if (s->input_buf.data && s->input_buf.data[0] != '\0') {
            blk->tool_input = safe_strdup(s->input_buf.data);
        } else {
            blk->tool_input = safe_strdup("{}");
        }
    }

    free(s->cur_type); s->cur_type = NULL;
    free(s->cur_tool_name); s->cur_tool_name = NULL;
    free(s->cur_tool_id); s->cur_tool_id = NULL;
    jbuf_reset(&s->text_buf);
    jbuf_reset(&s->input_buf);
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
                    jbuf_append(&s->text_buf, text);
                    if (s->text_cb) s->text_cb(text, s->cb_ctx);
                    rep_detect_feed(s, text);
                    free(text);
                }
            } else if (delta_type && strcmp(delta_type, "input_json_delta") == 0) {
                char *partial = json_get_str(delta_raw, "partial_json");
                if (partial) {
                    jbuf_append(&s->input_buf, partial);
                    free(partial);
                }
            }
            free(delta_type);
            free(delta_raw);
        }
    } else if (strcmp(event_type, "content_block_stop") == 0) {
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
            free(err_raw);
        }
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

    const char *p = (const char *)ptr;
    for (size_t i = 0; i < total; i++) {
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

stream_result_t llm_stream(const char *api_key, const char *request_json,
                           stream_text_cb text_cb,
                           stream_tool_start_cb tool_cb,
                           void *cb_ctx) {
    stream_result_t result = {0};

    /* Init SSE state */
    sse_state_t state = {0};
    state.current_index = -1;
    state.text_cb = text_cb;
    state.tool_cb = tool_cb;
    state.cb_ctx = cb_ctx;
    jbuf_init(&state.text_buf, 4096);
    jbuf_init(&state.input_buf, 4096);
    jbuf_init(&state.line_buf, 4096);

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "dsco: curl_easy_init failed\n");
        jbuf_free(&state.text_buf);
        jbuf_free(&state.input_buf);
        jbuf_free(&state.line_buf);
        return result;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");

    char auth[512];
    snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);
    headers = curl_slist_append(headers, auth);

    char ver[128];
    snprintf(ver, sizeof(ver), "anthropic-version: %s", ANTHROPIC_VERSION);
    headers = curl_slist_append(headers, ver);

    char beta[256];
    snprintf(beta, sizeof(beta), "anthropic-beta: %s", ANTHROPIC_BETAS);
    headers = curl_slist_append(headers, beta);

    headers = curl_slist_append(headers, "Expect:");

    curl_easy_setopt(curl, CURLOPT_URL, API_URL_ANTHROPIC);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    /* Progress callback for Ctrl+C abort and repetition detection */
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stream_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    /* No hard CURLOPT_TIMEOUT — streaming responses can legitimately take
       10+ minutes for large tool-use generations.  Instead, use low-speed
       detection: abort only if fewer than 100 bytes arrive in 120 seconds,
       which catches genuine stalls without killing long streams. */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 100L);   /* bytes/sec */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);    /* seconds   */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);     /* connect phase only */
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    result.http_status = (int)http_code;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res == CURLE_ABORTED_BY_CALLBACK) {
        /* Aborted by Ctrl+C or repetition detection — treat partial stream
           as ok so we can still process whatever content blocks arrived */
        result.ok = (state.block_count > 0 || state.text_buf.len > 0);
    } else if (res != CURLE_OK) {
        fprintf(stderr, "dsco: stream failed: %s\n", curl_easy_strerror(res));
        result.ok = false;
    } else if (state.got_error) {
        fprintf(stderr, "dsco: API error: %s\n", state.error_msg ? state.error_msg : "unknown");
        result.ok = false;
    } else if (http_code != 200) {
        /* Non-streaming error — line_buf may contain JSON error */
        if (state.line_buf.len > 0) {
            fprintf(stderr, "dsco: HTTP %d: %s\n", (int)http_code, state.line_buf.data);
        } else {
            fprintf(stderr, "dsco: HTTP %d\n", (int)http_code);
        }
        result.ok = false;
    } else {
        result.ok = true;
    }

    /* Finalize any incomplete block (connection dropped mid-stream) */
    if (state.current_index >= 0) {
        sse_finalize_block(&state);
    }

    /* Build parsed response from accumulated blocks */
    result.parsed.count = state.block_count;
    result.parsed.blocks = safe_malloc((state.block_count > 0 ? state.block_count : 1)
                                       * sizeof(content_block_t));
    memset(result.parsed.blocks, 0, (state.block_count > 0 ? state.block_count : 1)
                                     * sizeof(content_block_t));
    for (int i = 0; i < state.block_count; i++) {
        result.parsed.blocks[i] = state.blocks[i]; /* move ownership */
    }
    result.parsed.stop_reason = state.stop_reason; /* move ownership */
    result.usage = state.usage;

    /* Cleanup — cur_type etc. already freed by sse_finalize_block */
    free(state.error_msg);
    free(state.cur_type);
    free(state.cur_tool_name);
    free(state.cur_tool_id);
    jbuf_free(&state.text_buf);
    jbuf_free(&state.input_buf);
    jbuf_free(&state.line_buf);

    return result;
}
