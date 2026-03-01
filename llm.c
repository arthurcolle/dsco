#include "llm.h"
#include "tools.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>

/* ── Conversation management ───────────────────────────────────────────── */

void conv_init(conversation_t *c) {
    c->cap = 32;
    c->count = 0;
    c->msgs = calloc(c->cap, sizeof(message_t));
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
    if (c->count >= c->cap) {
        c->cap *= 2;
        c->msgs = realloc(c->msgs, c->cap * sizeof(message_t));
    }
    message_t *m = &c->msgs[c->count++];
    memset(m, 0, sizeof(*m));
    m->role = role;
    return m;
}

static msg_content_t *msg_add_content(message_t *m) {
    m->content = realloc(m->content, (m->content_count + 1) * sizeof(msg_content_t));
    msg_content_t *mc = &m->content[m->content_count++];
    memset(mc, 0, sizeof(*mc));
    return mc;
}

void conv_add_user_text(conversation_t *c, const char *text) {
    message_t *m = conv_add(c, ROLE_USER);
    msg_content_t *mc = msg_add_content(m);
    mc->type = strdup("text");
    mc->text = strdup(text);
}

void conv_add_assistant_text(conversation_t *c, const char *text) {
    message_t *m = conv_add(c, ROLE_ASSISTANT);
    msg_content_t *mc = msg_add_content(m);
    mc->type = strdup("text");
    mc->text = strdup(text);
}

void conv_add_assistant_tool_use(conversation_t *c, const char *tool_id,
                                  const char *tool_name, const char *tool_input) {
    message_t *m = conv_add(c, ROLE_ASSISTANT);
    msg_content_t *mc = msg_add_content(m);
    mc->type = strdup("tool_use");
    mc->tool_id = strdup(tool_id);
    mc->tool_name = strdup(tool_name);
    mc->tool_input = tool_input ? strdup(tool_input) : strdup("{}");
}

void conv_add_tool_result(conversation_t *c, const char *tool_id,
                          const char *result, bool is_error) {
    message_t *m = conv_add(c, ROLE_USER);
    msg_content_t *mc = msg_add_content(m);
    mc->type = strdup("tool_result");
    mc->tool_id = strdup(tool_id);
    mc->text = strdup(result);
    mc->is_error = is_error;
}

void conv_add_assistant_raw(conversation_t *c, parsed_response_t *resp) {
    message_t *m = conv_add(c, ROLE_ASSISTANT);
    for (int i = 0; i < resp->count; i++) {
        msg_content_t *mc = msg_add_content(m);
        mc->type = strdup(resp->blocks[i].type);
        if (resp->blocks[i].text) mc->text = strdup(resp->blocks[i].text);
        if (resp->blocks[i].tool_name) mc->tool_name = strdup(resp->blocks[i].tool_name);
        if (resp->blocks[i].tool_id) mc->tool_id = strdup(resp->blocks[i].tool_id);
        if (resp->blocks[i].tool_input) mc->tool_input = strdup(resp->blocks[i].tool_input);
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
        jbuf_append(b, mc->tool_input ? mc->tool_input : "{}");
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

static void append_tools_json(jbuf_t *b) {
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

    append_tools_json(&b);

    /* Messages */
    jbuf_append(&b, ",\"messages\":[");
    for (int i = 0; i < c->count; i++) {
        if (i > 0) jbuf_append(&b, ",");
        message_t *m = &c->msgs[i];
        jbuf_append(&b, "{\"role\":");
        jbuf_append_json_str(&b, m->role == ROLE_USER ? "user" : "assistant");
        jbuf_append(&b, ",\"content\":[");
        for (int j = 0; j < m->content_count; j++) {
            if (j > 0) jbuf_append(&b, ",");
            append_content_block(&b, &m->content[j]);
        }
        jbuf_append(&b, "]}");
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
} sse_state_t;

static void sse_finalize_block(sse_state_t *s) {
    if (s->current_index < 0) return;
    int idx = s->block_count++;
    if (idx >= MAX_CONTENT_BLOCKS) return;

    content_block_t *blk = &s->blocks[idx];
    blk->type = s->cur_type ? strdup(s->cur_type) : strdup("text");

    if (s->cur_type && strcmp(s->cur_type, "text") == 0) {
        blk->text = strdup(s->text_buf.data ? s->text_buf.data : "");
    } else if (s->cur_type && strcmp(s->cur_type, "tool_use") == 0) {
        blk->tool_name = s->cur_tool_name ? strdup(s->cur_tool_name) : NULL;
        blk->tool_id = s->cur_tool_id ? strdup(s->cur_tool_id) : NULL;
        blk->tool_input = strdup(s->input_buf.data ? s->input_buf.data : "{}");
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
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

    if (res != CURLE_OK) {
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

    /* Build parsed response from accumulated blocks */
    result.parsed.count = state.block_count;
    result.parsed.blocks = calloc(state.block_count > 0 ? state.block_count : 1,
                                   sizeof(content_block_t));
    for (int i = 0; i < state.block_count; i++) {
        result.parsed.blocks[i] = state.blocks[i]; /* move ownership */
    }
    result.parsed.stop_reason = state.stop_reason; /* move ownership */
    result.usage = state.usage;

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
