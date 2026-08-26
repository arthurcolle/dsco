#include "tool_call_normalizer.h"
#include "json_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NORMALIZER_INPUT_MAX (1024u * 1024u)
#define NORMALIZER_ARGS_MAX (256u * 1024u)

static bool valid_name(const char *s) {
    if (!s || !s[0]) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.' && *p != ':') return false;
    return strlen(s) < TOOL_CALL_NORMALIZER_NAME_MAX;
}

static char *slice(const char *p, size_t n) {
    char *s = safe_malloc(n + 1);
    memcpy(s, p, n); s[n] = '\0';
    return s;
}

static void set_error(normalized_tool_calls_t *out, const char *msg) {
    if (!out->error[0]) snprintf(out->error, sizeof(out->error), "%s", msg);
}

static bool add_call(normalized_tool_calls_t *out, const char *name, const char *args) {
    if (!valid_name(name)) { set_error(out, "invalid tool name"); return false; }
    if (!args || strlen(args) > NORMALIZER_ARGS_MAX || !json_is_valid_container(args) || args[0] != '{') {
        set_error(out, "tool arguments must be a bounded JSON object"); return false;
    }
    if (out->count == TOOL_CALL_NORMALIZER_MAX_CALLS) {
        out->truncated = true; set_error(out, "too many tool calls"); return false;
    }
    normalized_tool_call_t *c = &out->calls[out->count++];
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->arguments = safe_strdup(args);
    return true;
}

static const char *json_value_end(const char *p) {
    if (!p) return NULL;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '{' && *p != '[') return NULL;
    char open = *p, close = open == '{' ? '}' : ']';
    int depth = 0; bool str = false, esc = false;
    for (const char *q = p; *q; q++) {
        if (str) { if (esc) esc = false; else if (*q == '\\') esc = true; else if (*q == '"') str = false; continue; }
        if (*q == '"') { str = true; continue; }
        if (*q == open) depth++;
        else if (*q == close && --depth == 0) return q + 1;
    }
    return NULL;
}

static bool parse_call_object(const char *obj, normalized_tool_calls_t *out) {
    char *name = json_get_str(obj, "name");
    char *args = json_get_raw(obj, "arguments");
    if ((!name || !args)) {
        char *fn = json_get_raw(obj, "function");
        if (fn) { free(name); free(args); name = json_get_str(fn, "name"); args = json_get_raw(fn, "arguments"); free(fn); }
    }
    if (!name || !args) { free(name); free(args); return false; }
    if (args[0] == '"') {
        char *decoded = json_get_str(obj, "arguments");
        if (!decoded) { char *fn = json_get_raw(obj, "function"); decoded = fn ? json_get_str(fn, "arguments") : NULL; free(fn); }
        free(args); args = decoded;
    }
    bool ok = add_call(out, name, args);
    free(name); free(args); return ok;
}

static int parse_json_region(const char *p, size_t n, normalized_tool_calls_t *out) {
    int before = (int)out->count;
    const char *end = p + n;
    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == ',' || *p == '[' || *p == ']')) p++;
        if (p >= end) break;
        if (*p != '{') return (int)out->count - before;
        const char *q = json_value_end(p);
        if (!q || q > end) return (int)out->count - before;
        char *obj = slice(p, (size_t)(q - p));
        parse_call_object(obj, out); free(obj); p = q;
    }
    return (int)out->count - before;
}

static int parse_openai(const char *text, normalized_tool_calls_t *out) {
    int before = (int)out->count;
    const char *p = text;
    while ((p = strstr(p, "\"tool_calls\"")) != NULL) {
        p = strchr(p, '['); if (!p) break;
        const char *end = json_value_end(p); if (!end) break;
        parse_json_region(p + 1, (size_t)(end - p - 2), out); p = end;
    }
    return (int)out->count - before;
}

static int parse_blocks(const char *text, normalized_tool_calls_t *out) {
    int before = (int)out->count;
    const char *p = text;
    while ((p = strstr(p, "<tools>")) != NULL) {
        p += 7; const char *e = strstr(p, "</tools>"); if (!e) { set_error(out, "unterminated tools block"); break; }
        parse_json_region(p, (size_t)(e - p), out); p = e + 8;
    }
    return (int)out->count - before;
}

static int parse_fenced_or_bare_json(const char *text, normalized_tool_calls_t *out) {
    const char *p = strstr(text, "```");
    if (p) {
        p += 3; if (!strncmp(p, "json", 4)) p += 4;
        const char *e = strstr(p, "```");
        return e ? parse_json_region(p, (size_t)(e - p), out) : 0;
    }
    p = text; while (isspace((unsigned char)*p)) p++;
    return (*p == '[' || *p == '{') ? parse_json_region(*p == '[' ? p + 1 : p, strlen(p) - (*p == '[' ? 2 : 0), out) : 0;
}

static int parse_function_xml(const char *text, normalized_tool_calls_t *out) {
    int before = (int)out->count;
    const char *p = text;
    while ((p = strstr(p, "<function=")) != NULL) {
        const char *name_start = p + 10;
        const char *name_end = strchr(name_start, '>');
        if (!name_end) break;
        char *name = slice(name_start, (size_t)(name_end - name_start));
        char close[TOOL_CALL_NORMALIZER_NAME_MAX + 16];
        snprintf(close, sizeof(close), "</function>");
        const char *end = strstr(name_end + 1, close);
        if (!end) { free(name); break; }
        const char *body = name_end + 1;
        while (body < end && isspace((unsigned char)*body)) body++;
        char *args = body == end ? safe_strdup("{}") : slice(body, (size_t)(end - body));
        add_call(out, name, args);
        free(args); free(name); p = end + strlen(close);
    }
    return (int)out->count - before;
}

static int parse_xml_shorthand(const char *text, normalized_tool_calls_t *out) {
    int before = (int)out->count;
    for (const char *p = text; (p = strchr(p, '<')) != NULL; p++) {
        if (p[1] == '/' || p[1] == '!' || p[1] == '?' || !isalpha((unsigned char)p[1])) continue;
        const char *q = p + 1; while (isalnum((unsigned char)*q) || *q == '_' || *q == '-' || *q == '.') q++;
        if (q == p + 1) continue;
        const char *e = strstr(q, "/>"); if (!e) continue;
        char *name = slice(p + 1, (size_t)(q - p - 1));
        if (!strcmp(name, "tools") || !strcmp(name, "tool_call")) { free(name); continue; }
        jbuf_t args; jbuf_init(&args, 128); jbuf_append_char(&args, '{'); bool first = true, bad = false;
        const char *a = q;
        while (a < e) {
            while (a < e && isspace((unsigned char)*a)) a++;
            const char *ks = a; while (a < e && (isalnum((unsigned char)*a) || *a == '_')) a++;
            if (a == ks) break;
            const char *ke = a; while (a < e && isspace((unsigned char)*a)) a++;
            if (a >= e || *a++ != '=') { bad = true; break; }
            while (a < e && isspace((unsigned char)*a)) a++;
            if (a >= e || (*a != '"' && *a != '\'')) { bad = true; break; }
            char quote = *a++; const char *vs = a; while (a < e && *a != quote) a++;
            if (a >= e) { bad = true; break; }
            char *key = slice(ks, (size_t)(ke - ks)), *val = slice(vs, (size_t)(a - vs)); a++;
            if (!first) jbuf_append_char(&args, ','); first = false;
            jbuf_append_json_str(&args, key); jbuf_append_char(&args, ':');
            char *ve = NULL; double d = strtod(val, &ve);
            if (val[0] && ve && !*ve) jbuf_appendf(&args, "%.17g", d); else if (!strcmp(val, "true") || !strcmp(val, "false") || !strcmp(val, "null")) jbuf_append(&args, val); else jbuf_append_json_str(&args, val);
            free(key); free(val);
        }
        jbuf_append_char(&args, '}');
        if (!bad) add_call(out, name, args.data);
        jbuf_free(&args); free(name); p = e + 1;
    }
    return (int)out->count - before;
}

bool tool_calls_normalize(const char *text, normalized_tool_calls_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!text || strlen(text) > NORMALIZER_INPUT_MAX) { set_error(out, "empty or oversized model output"); return false; }
    if (parse_openai(text, out) == 0 && parse_blocks(text, out) == 0 &&
        parse_fenced_or_bare_json(text, out) == 0 && parse_function_xml(text, out) == 0)
        parse_xml_shorthand(text, out);
    return out->count > 0 && !out->truncated;
}

void tool_calls_normalized_free(normalized_tool_calls_t *out) {
    if (!out) return;
    for (size_t i = 0; i < out->count; i++) free(out->calls[i].arguments);
    memset(out, 0, sizeof(*out));
}
