#include "config.h"
#include "wasm_core.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define DSCO_WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define DSCO_WASM_EXPORT
#endif

#define WASM_OUT_CAP (256 * 1024)
#define WASM_SESSION_CAP (128 * 1024)

static char g_out[WASM_OUT_CAP];
static char g_session[WASM_SESSION_CAP];
static size_t g_session_len;
static int g_turns;

static const char *const WASM_EXPORTED_FUNCTIONS[] = {
    "_dsco_wasm_version",       "_dsco_wasm_models_json",   "_dsco_wasm_tools_json",
    "_dsco_wasm_route_explain", "_dsco_wasm_tool_exec",     "_dsco_wasm_session_reset",
    "_dsco_wasm_session_add",   "_dsco_wasm_session_state", NULL,
};

static void out_reset(void) {
    g_out[0] = '\0';
}

static void out_append(const char *s) {
    if (!s)
        return;
    size_t cur = strlen(g_out);
    size_t rem = cur < sizeof(g_out) ? sizeof(g_out) - cur - 1 : 0;
    if (rem == 0)
        return;
    strncat(g_out, s, rem);
}

static void out_json_str(const char *s) {
    out_append("\"");
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            char tmp[8];
            switch (*p) {
                case '"':
                    out_append("\\\"");
                    break;
                case '\\':
                    out_append("\\\\");
                    break;
                case '\n':
                    out_append("\\n");
                    break;
                case '\r':
                    out_append("\\r");
                    break;
                case '\t':
                    out_append("\\t");
                    break;
                default:
                    if (*p < 0x20) {
                        snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                        out_append(tmp);
                    } else {
                        tmp[0] = (char)*p;
                        tmp[1] = '\0';
                        out_append(tmp);
                    }
                    break;
            }
        }
    }
    out_append("\"");
}

static const char *provider_for_model(const char *model) {
    if (!model || !*model)
        return "moonshot";
    if (strstr(model, "moonshot") || strstr(model, "kimi"))
        return "moonshot";
    if (strstr(model, "claude") || strstr(model, "anthropic/"))
        return "anthropic";
    if (strstr(model, "openai/") || strstr(model, "gpt") || strstr(model, "o3") ||
        strstr(model, "o4") || strstr(model, "o1"))
        return "openai";
    if (strchr(model, '/'))
        return "openrouter";
    return "openrouter";
}

static const model_info_t *model_lookup_local(const char *name) {
    if (!name || !*name)
        name = DEFAULT_MODEL;
    for (int i = 0; MODEL_REGISTRY[i].alias; i++) {
        const model_info_t *m = &MODEL_REGISTRY[i];
        if (strcmp(name, m->alias) == 0 || strcmp(name, m->model_id) == 0)
            return m;
    }
    return NULL;
}

static const char *json_field_string(const char *json, const char *key, char *buf, size_t buflen) {
    if (!json || !key || !buf || buflen == 0)
        return NULL;
    buf[0] = '\0';

    char needle[96];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p)
        return NULL;
    p += strlen(needle);
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '"')
        return NULL;
    p++;

    size_t n = 0;
    bool esc = false;
    while (*p && n + 1 < buflen) {
        char c = *p++;
        if (esc) {
            switch (c) {
                case 'n':
                    buf[n++] = '\n';
                    break;
                case 'r':
                    buf[n++] = '\r';
                    break;
                case 't':
                    buf[n++] = '\t';
                    break;
                default:
                    buf[n++] = c;
                    break;
            }
            esc = false;
            continue;
        }
        if (c == '\\') {
            esc = true;
            continue;
        }
        if (c == '"')
            break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return buf;
}

DSCO_WASM_EXPORT
const char *dsco_wasm_exports_json(void) {
    out_reset();
    out_append("[");
    for (int i = 0; WASM_EXPORTED_FUNCTIONS[i]; i++) {
        if (i)
            out_append(",");
        out_json_str(WASM_EXPORTED_FUNCTIONS[i] + 1);
    }
    out_append("]");
    return g_out;
}

DSCO_WASM_EXPORT
const char *dsco_wasm_version(void) {
    out_reset();
    snprintf(g_out, sizeof(g_out),
             "{\"name\":\"dsco-wasm\",\"version\":\"%s\",\"build_date\":\"%s\","
             "\"git_hash\":\"%s\",\"default_model\":\"%s\"}",
             DSCO_VERSION, BUILD_DATE, GIT_HASH, DEFAULT_MODEL);
    return g_out;
}

DSCO_WASM_EXPORT
const char *dsco_wasm_models_json(void) {
    out_reset();
    out_append("[");
    for (int i = 0; MODEL_REGISTRY[i].alias; i++) {
        const model_info_t *m = &MODEL_REGISTRY[i];
        if (i)
            out_append(",");
        char nums[256];
        out_append("{\"alias\":");
        out_json_str(m->alias);
        out_append(",\"model_id\":");
        out_json_str(m->model_id);
        snprintf(nums, sizeof(nums),
                 ",\"context_window\":%d,\"max_output\":%d,"
                 "\"input_price\":%.4f,\"output_price\":%.4f,"
                 "\"cache_read_price\":%.4f,\"cache_write_price\":%.4f,"
                 "\"supports_thinking\":%d}",
                 m->context_window, m->max_output, m->input_price, m->output_price,
                 m->cache_read_price, m->cache_write_price, m->supports_thinking);
        out_append(nums);
    }
    out_append("]");
    return g_out;
}

DSCO_WASM_EXPORT
const char *dsco_wasm_tools_json(void) {
    return "["
           "{\"name\":\"route_explain\",\"description\":\"Explain browser-local model routing.\","
           "\"input_schema\":{\"type\":\"object\",\"properties\":{\"model\":{\"type\":\"string\"}}}"
           "},"
           "{\"name\":\"session_add\",\"description\":\"Append a message to browser-local "
           "transcript state.\","
           "\"input_schema\":{\"type\":\"object\",\"properties\":{\"role\":{\"type\":\"string\"},"
           "\"content\":{\"type\":\"string\"}}}},"
           "{\"name\":\"session_state\",\"description\":\"Return browser-local transcript state.\","
           "\"input_schema\":{\"type\":\"object\",\"properties\":{}}},"
           "{\"name\":\"session_reset\",\"description\":\"Clear browser-local transcript state.\","
           "\"input_schema\":{\"type\":\"object\",\"properties\":{}}},"
           "{\"name\":\"echo\",\"description\":\"Return the input text.\","
           "\"input_schema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}}"
           "]";
}

DSCO_WASM_EXPORT
const char *dsco_wasm_route_explain(const char *model) {
    const char *requested = (model && *model) ? model : DEFAULT_MODEL;
    const model_info_t *m = model_lookup_local(requested);
    const char *resolved = m ? m->model_id : requested;
    const char *provider = provider_for_model(resolved);

    out_reset();
    out_append("{\"requested\":");
    out_json_str(requested);
    out_append(",\"resolved_model\":");
    out_json_str(resolved);
    out_append(",\"provider\":");
    out_json_str(provider);
    out_append(",\"runtime\":\"browser-wasm\",\"host_tools\":\"permissioned-bridge\","
               "\"note\":\"agent control plane is local to this browser; native shell/files/MCP "
               "require a bridge\"}");
    return g_out;
}

DSCO_WASM_EXPORT
const char *dsco_wasm_session_reset(void) {
    g_session[0] = '\0';
    g_session_len = 0;
    g_turns = 0;
    return "{\"ok\":true,\"turns\":0}";
}

DSCO_WASM_EXPORT
const char *dsco_wasm_session_add(const char *role, const char *content) {
    if (!role || !*role)
        role = "user";
    if (!content)
        content = "";

    char line[4096];
    snprintf(line, sizeof(line), "%s: %s\n", role, content);
    size_t n = strlen(line);
    if (g_session_len + n + 1 >= sizeof(g_session)) {
        return "{\"ok\":false,\"error\":\"browser session buffer full\"}";
    }
    memcpy(g_session + g_session_len, line, n + 1);
    g_session_len += n;
    g_turns++;

    out_reset();
    snprintf(g_out, sizeof(g_out), "{\"ok\":true,\"turns\":%d,\"bytes\":%zu}", g_turns,
             g_session_len);
    return g_out;
}

DSCO_WASM_EXPORT
const char *dsco_wasm_session_state(void) {
    out_reset();
    char head[96];
    snprintf(head, sizeof(head), "{\"turns\":%d,\"transcript\":", g_turns);
    out_append(head);
    out_json_str(g_session);
    out_append("}");
    return g_out;
}

DSCO_WASM_EXPORT
const char *dsco_wasm_tool_exec(const char *name, const char *input_json) {
    char model[256] = {0};
    char role[64] = {0};
    char content[4096] = {0};
    char text[4096] = {0};

    if (!name)
        name = "";
    if (strcmp(name, "route_explain") == 0) {
        json_field_string(input_json, "model", model, sizeof(model));
        return dsco_wasm_route_explain(model);
    }
    if (strcmp(name, "session_add") == 0) {
        json_field_string(input_json, "role", role, sizeof(role));
        json_field_string(input_json, "content", content, sizeof(content));
        return dsco_wasm_session_add(role[0] ? role : "user", content);
    }
    if (strcmp(name, "session_state") == 0)
        return dsco_wasm_session_state();
    if (strcmp(name, "session_reset") == 0)
        return dsco_wasm_session_reset();
    if (strcmp(name, "echo") == 0) {
        json_field_string(input_json, "text", text, sizeof(text));
        out_reset();
        out_append("{\"ok\":true,\"text\":");
        out_json_str(text);
        out_append("}");
        return g_out;
    }

    out_reset();
    out_append("{\"ok\":false,\"error\":\"tool unavailable in browser wasm core\",\"tool\":");
    out_json_str(name);
    out_append("}");
    return g_out;
}

#ifdef DSCO_WASM_STANDALONE
int main(void) {
    puts(dsco_wasm_version());
    puts(dsco_wasm_route_explain(NULL));
    puts(dsco_wasm_tool_exec("echo", "{\"text\":\"ok\"}"));
    return 0;
}
#endif
