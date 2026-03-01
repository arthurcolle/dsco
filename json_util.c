#include "json_util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ── jbuf: dynamic string builder ──────────────────────────────────────── */

void jbuf_init(jbuf_t *b, size_t cap) {
    b->data = malloc(cap);
    b->len = 0;
    b->cap = cap;
    if (b->data) b->data[0] = '\0';
}

void jbuf_free(jbuf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void jbuf_reset(jbuf_t *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

static void jbuf_grow(jbuf_t *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    size_t newcap = b->cap * 2;
    if (newcap < b->len + need + 1) newcap = b->len + need + 1;
    char *p = realloc(b->data, newcap);
    if (!p) { fprintf(stderr, "dsco: out of memory\n"); exit(1); }
    b->data = p;
    b->cap = newcap;
}

void jbuf_append(jbuf_t *b, const char *s) {
    size_t n = strlen(s);
    jbuf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void jbuf_append_len(jbuf_t *b, const char *s, size_t n) {
    jbuf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void jbuf_append_char(jbuf_t *b, char c) {
    jbuf_grow(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void jbuf_append_json_str(jbuf_t *b, const char *s) {
    jbuf_append_char(b, '"');
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  jbuf_append(b, "\\\""); break;
            case '\\': jbuf_append(b, "\\\\"); break;
            case '\b': jbuf_append(b, "\\b");  break;
            case '\f': jbuf_append(b, "\\f");  break;
            case '\n': jbuf_append(b, "\\n");  break;
            case '\r': jbuf_append(b, "\\r");  break;
            case '\t': jbuf_append(b, "\\t");  break;
            default:
                if ((unsigned char)*p < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
                    jbuf_append(b, esc);
                } else {
                    jbuf_append_char(b, *p);
                }
        }
    }
    jbuf_append_char(b, '"');
}

void jbuf_append_int(jbuf_t *b, int v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%d", v);
    jbuf_append(b, tmp);
}

/* ── Minimal JSON parser ───────────────────────────────────────────────── */

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *parse_string(const char *p, char **out) {
    if (*p != '"') return NULL;
    p++;
    jbuf_t b;
    jbuf_init(&b, 256);
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"': case '\\': case '/': jbuf_append_char(&b, *p); break;
                case 'b': jbuf_append_char(&b, '\b'); break;
                case 'f': jbuf_append_char(&b, '\f'); break;
                case 'n': jbuf_append_char(&b, '\n'); break;
                case 'r': jbuf_append_char(&b, '\r'); break;
                case 't': jbuf_append_char(&b, '\t'); break;
                case 'u': {
                    unsigned int cp = 0;
                    for (int i = 0; i < 4 && p[1]; i++) {
                        p++;
                        cp = cp * 16;
                        if (*p >= '0' && *p <= '9') cp += *p - '0';
                        else if (*p >= 'a' && *p <= 'f') cp += *p - 'a' + 10;
                        else if (*p >= 'A' && *p <= 'F') cp += *p - 'A' + 10;
                    }
                    if (cp < 0x80) {
                        jbuf_append_char(&b, (char)cp);
                    } else if (cp < 0x800) {
                        jbuf_append_char(&b, (char)(0xC0 | (cp >> 6)));
                        jbuf_append_char(&b, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        jbuf_append_char(&b, (char)(0xE0 | (cp >> 12)));
                        jbuf_append_char(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        jbuf_append_char(&b, (char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: jbuf_append_char(&b, *p); break;
            }
        } else {
            jbuf_append_char(&b, *p);
        }
        p++;
    }
    if (*p == '"') p++;
    *out = b.data;
    return p;
}

static const char *skip_value(const char *p) {
    p = skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        if (*p == '"') p++;
        return p;
    }
    if (*p == '{') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
        return p;
    }
    if (*p == '[') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
    return p;
}

static const char *extract_raw_value(const char *p, const char **start, const char **end) {
    p = skip_ws(p);
    *start = p;
    *end = skip_value(p);
    return *end;
}

static const char *find_key(const char *p, const char *key) {
    p = skip_ws(p);
    while (*p && *p != '}') {
        if (*p == '"') {
            char *k = NULL;
            const char *after = parse_string(p, &k);
            if (!after) { free(k); return NULL; }
            after = skip_ws(after);
            if (*after == ':') after++;
            after = skip_ws(after);
            if (k && strcmp(k, key) == 0) { free(k); return after; }
            free(k);
            after = skip_value(after);
            after = skip_ws(after);
            if (*after == ',') after++;
            p = after;
        } else {
            p++;
        }
    }
    return NULL;
}

char *json_get_str(const char *json, const char *key) {
    const char *p = skip_ws(json);
    if (*p != '{') return NULL;
    p = find_key(p + 1, key);
    if (!p) return NULL;
    p = skip_ws(p);
    if (*p != '"') return NULL;
    char *val = NULL;
    parse_string(p, &val);
    return val;
}

char *json_get_raw(const char *json, const char *key) {
    const char *p = skip_ws(json);
    if (*p != '{') return NULL;
    p = find_key(p + 1, key);
    if (!p) return NULL;
    const char *start, *end;
    extract_raw_value(p, &start, &end);
    size_t len = (size_t)(end - start);
    char *r = malloc(len + 1);
    memcpy(r, start, len);
    r[len] = '\0';
    return r;
}

int json_get_int(const char *json, const char *key, int def) {
    char *raw = json_get_raw(json, key);
    if (!raw) return def;
    int v = atoi(raw);
    free(raw);
    return v;
}

bool json_get_bool(const char *json, const char *key, bool def) {
    char *raw = json_get_raw(json, key);
    if (!raw) return def;
    bool v = (strncmp(raw, "true", 4) == 0);
    free(raw);
    return v;
}

int json_array_foreach(const char *json, const char *key, json_array_cb cb, void *ctx) {
    const char *p = skip_ws(json);
    if (*p != '{') return 0;
    p = find_key(p + 1, key);
    if (!p) return 0;
    p = skip_ws(p);
    if (*p != '[') return 0;
    p++; /* past [ */
    int count = 0;
    p = skip_ws(p);
    while (*p && *p != ']') {
        cb(p, ctx);
        count++;
        p = skip_value(p);
        p = skip_ws(p);
        if (*p == ',') p++;
        p = skip_ws(p);
    }
    return count;
}

/* ── Parse Anthropic Messages API response ─────────────────────────────── */

static content_block_t parse_content_block(const char *p) {
    content_block_t blk = {0};
    if (*p != '{') return blk;

    const char *obj_start = p + 1;
    const char *vp = find_key(obj_start, "type");
    if (vp && *vp == '"') parse_string(vp, &blk.type);

    if (blk.type && strcmp(blk.type, "text") == 0) {
        vp = find_key(obj_start, "text");
        if (vp && *vp == '"') parse_string(vp, &blk.text);
    } else if (blk.type && strcmp(blk.type, "tool_use") == 0) {
        vp = find_key(obj_start, "name");
        if (vp && *vp == '"') parse_string(vp, &blk.tool_name);
        vp = find_key(obj_start, "id");
        if (vp && *vp == '"') parse_string(vp, &blk.tool_id);
        vp = find_key(obj_start, "input");
        if (vp) {
            const char *start, *end;
            extract_raw_value(vp, &start, &end);
            size_t len = (size_t)(end - start);
            blk.tool_input = malloc(len + 1);
            memcpy(blk.tool_input, start, len);
            blk.tool_input[len] = '\0';
        }
    }
    return blk;
}

bool json_parse_response(const char *json, parsed_response_t *out) {
    memset(out, 0, sizeof(*out));
    const char *p = skip_ws(json);
    if (*p != '{') return false;
    p++;

    const char *vp = find_key(p, "stop_reason");
    if (vp && *vp == '"') parse_string(vp, &out->stop_reason);

    vp = find_key(p, "content");
    if (!vp || *vp != '[') return false;
    vp++;

    int cap = 8;
    out->blocks = calloc(cap, sizeof(content_block_t));
    out->count = 0;

    vp = skip_ws(vp);
    while (*vp && *vp != ']') {
        if (*vp == '{') {
            if (out->count >= cap) {
                cap *= 2;
                out->blocks = realloc(out->blocks, cap * sizeof(content_block_t));
            }
            out->blocks[out->count] = parse_content_block(vp);
            out->count++;
            vp = skip_value(vp);
        }
        vp = skip_ws(vp);
        if (*vp == ',') vp++;
        vp = skip_ws(vp);
    }
    return true;
}

void json_free_response(parsed_response_t *r) {
    for (int i = 0; i < r->count; i++) {
        free(r->blocks[i].type);
        free(r->blocks[i].text);
        free(r->blocks[i].tool_name);
        free(r->blocks[i].tool_id);
        free(r->blocks[i].tool_input);
    }
    free(r->blocks);
    free(r->stop_reason);
    memset(r, 0, sizeof(*r));
}
