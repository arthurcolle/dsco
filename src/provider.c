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
#include <stdio.h>
#include <curl/curl.h>

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

/* ── OpenAI-compatible Provider ────────────────────────────────────────── */

typedef struct {
    char api_url[512];
} openai_data_t;

static void openai_append_message_content(jbuf_t *b, message_t *m) {
    jbuf_append(b, "[");

    bool wrote_any = false;
    jbuf_t text;
    jbuf_init(&text, 1024);

    for (int j = 0; j < m->content_count; j++) {
        msg_content_t *mc = &m->content[j];
        if (mc->type && strcmp(mc->type, "text") == 0 && mc->text) {
            if (text.len > 0) jbuf_append(&text, "\n");
            jbuf_append(&text, mc->text);
        } else if (mc->type && strcmp(mc->type, "tool_result") == 0 && mc->text) {
            if (text.len > 0) jbuf_append(&text, "\n");
            jbuf_append(&text, "[tool_result] ");
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

    /* Conversation messages */
    for (int i = 0; i < conv->count; i++) {
        message_t *m = &conv->msgs[i];
        jbuf_append(&b, ",{\"role\":");
        jbuf_append_json_str(&b, m->role == ROLE_USER ? "user" : "assistant");
        jbuf_append(&b, ",\"content\":");
        openai_append_message_content(&b, m);
        jbuf_append(&b, "}");
    }
    jbuf_append(&b, "]");

    /* Tools — convert to OpenAI format */
    int tool_count;
    const tool_def_t *tools = tools_get_all(&tool_count);
    if (tool_count > 0) {
        jbuf_append(&b, ",\"tools\":[");
        for (int i = 0; i < tool_count && i < 128; i++) {
            if (i > 0) jbuf_append(&b, ",");
            jbuf_append(&b, "{\"type\":\"function\",\"function\":{\"name\":");
            jbuf_append_json_str(&b, tools[i].name);
            jbuf_append(&b, ",\"description\":");
            jbuf_append_json_str(&b, tools[i].description);
            jbuf_append(&b, ",\"parameters\":");
            jbuf_append(&b, tools[i].input_schema_json);
            jbuf_append(&b, "}}");
        }
        jbuf_append(&b, "]");
    }

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
    jbuf_t          line_buf;
    jbuf_t          text_accum;
    jbuf_t          tool_args;     /* accumulates tool call arguments */
    char           *tool_name;
    char           *tool_id;
    int             tool_index;    /* current tool_call index */
    stream_text_cb  text_cb;
    stream_tool_start_cb tool_cb;
    void           *cb_ctx;
    usage_t         usage;
    char           *stop_reason;
    bool            got_error;
    /* Result building */
    content_block_t blocks[MAX_CONTENT_BLOCKS];
    int             block_count;
} oai_sse_state_t;

static void oai_handle_sse_line(oai_sse_state_t *s, const char *line) {
    if (strncmp(line, "data: ", 6) != 0) return;
    const char *data = line + 6;
    if (strcmp(data, "[DONE]") == 0) return;

    /* Parse the SSE chunk */
    char *choices_raw = json_get_raw(data, "choices");
    if (!choices_raw) {
        /* Check for usage in final message */
        char *usage_raw = json_get_raw(data, "usage");
        if (usage_raw) {
            s->usage.input_tokens = json_get_int(usage_raw, "prompt_tokens", 0);
            s->usage.output_tokens = json_get_int(usage_raw, "completion_tokens", 0);
            free(usage_raw);
        }
        return;
    }

    /* Extract delta from first choice */
    char *delta_raw = json_get_raw(choices_raw, "delta");
    if (!delta_raw) {
        /* Try finish_reason */
        char *fr = json_get_str(choices_raw, "finish_reason");
        if (fr) {
            free(s->stop_reason);
            if (strcmp(fr, "stop") == 0)
                s->stop_reason = safe_strdup("end_turn");
            else if (strcmp(fr, "tool_calls") == 0)
                s->stop_reason = safe_strdup("tool_use");
            else
                s->stop_reason = fr;
        }
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

    /* Tool calls delta */
    char *tool_calls_raw = json_get_raw(delta_raw, "tool_calls");
    if (tool_calls_raw) {
        /* Each chunk has index, function.name (first chunk), function.arguments (subsequent) */
        int idx = json_get_int(tool_calls_raw, "index", 0);
        char *fn_raw = json_get_raw(tool_calls_raw, "function");
        if (fn_raw) {
            char *fname = json_get_str(fn_raw, "name");
            char *fargs = json_get_str(fn_raw, "arguments");
            char *tid = json_get_str(tool_calls_raw, "id");

            if (fname) {
                /* New tool call starting */
                /* Finalize previous tool if any */
                if (s->tool_name && s->tool_index >= 0) {
                    int bi = s->block_count++;
                    if (bi < MAX_CONTENT_BLOCKS) {
                        s->blocks[bi].type = safe_strdup("tool_use");
                        s->blocks[bi].tool_name = s->tool_name;
                        s->blocks[bi].tool_id = s->tool_id;
                        s->blocks[bi].tool_input = safe_strdup(
                            s->tool_args.data ? s->tool_args.data : "{}");
                    }
                    s->tool_name = NULL;
                    s->tool_id = NULL;
                }

                s->tool_name = fname;
                s->tool_id = tid ? tid : safe_strdup("call_0");
                s->tool_index = idx;
                jbuf_reset(&s->tool_args);

                if (s->tool_cb) s->tool_cb(fname, s->tool_id, s->cb_ctx);

                if (tid && tid != s->tool_id) { /* already assigned */ }
            } else {
                free(tid);
            }

            if (fargs) {
                jbuf_append(&s->tool_args, fargs);
                free(fargs);
            }
            free(fn_raw);
        }
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

    struct curl_slist *hdrs = openai_build_headers(p, api_key);
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");

    oai_sse_state_t state = {0};
    jbuf_init(&state.line_buf, 4096);
    jbuf_init(&state.text_accum, 4096);
    jbuf_init(&state.tool_args, 1024);
    state.text_cb = text_cb;
    state.tool_cb = tool_cb;
    state.cb_ctx = cb_ctx;
    state.tool_index = -1;

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

        /* Finalize any pending tool call */
        if (state.tool_name && state.tool_index >= 0) {
            int bi = state.block_count++;
            if (bi < MAX_CONTENT_BLOCKS) {
                state.blocks[bi].type = safe_strdup("tool_use");
                state.blocks[bi].tool_name = state.tool_name;
                state.blocks[bi].tool_id = state.tool_id;
                state.blocks[bi].tool_input = safe_strdup(
                    state.tool_args.data ? state.tool_args.data : "{}");
            }
            state.tool_name = NULL;
            state.tool_id = NULL;
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
    } else {
        result.ok = false;
        if (state.line_buf.len > 0) {
            fprintf(stderr, "dsco: OpenAI HTTP %d: %.*s\n",
                    (int)http_code, 200, state.line_buf.data);
        }
        free(state.stop_reason);
        free(state.tool_name);
        free(state.tool_id);
    }

    jbuf_free(&state.line_buf);
    jbuf_free(&state.text_accum);
    jbuf_free(&state.tool_args);

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
        /* Anthropic */
        if (strstr(model, "claude") || strstr(model, "opus") ||
            strstr(model, "sonnet") || strstr(model, "haiku"))
            return "anthropic";
        /* OpenAI */
        if (strstr(model, "gpt") || strncmp(model, "o1", 2) == 0 ||
            strncmp(model, "o3", 2) == 0 || strstr(model, "chatgpt"))
            return "openai";
        /* Groq */
        if (strstr(model, "llama") || strstr(model, "mixtral") ||
            strstr(model, "gemma"))
            return "groq";
        /* DeepSeek */
        if (strstr(model, "deepseek"))
            return "deepseek";
        /* Mistral */
        if (strstr(model, "mistral") || strstr(model, "codestral") ||
            strstr(model, "pixtral"))
            return "mistral";
        /* Together */
        if (strstr(model, "Qwen") || strstr(model, "together") ||
            strstr(model, "/"))  /* HF-style org/model IDs */
            return "together";
        /* Cohere */
        if (strstr(model, "command"))
            return "cohere";
        /* xAI */
        if (strstr(model, "grok"))
            return "xai";
        /* Perplexity */
        if (strstr(model, "sonar") || strstr(model, "pplx"))
            return "perplexity";
        /* Cerebras */
        if (strstr(model, "cerebras"))
            return "cerebras";
        /* OpenRouter */
        if (strstr(model, "openrouter") || strstr(model, "auto"))
            return "openrouter";
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
