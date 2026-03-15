#include "json_util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>

/* ── Safe allocation wrappers ──────────────────────────────────────────── */

void *safe_malloc(size_t size) {
    if (size == 0) size = 1;
    void *p = malloc(size);
    if (!p) {
        fprintf(stderr, "dsco: fatal: malloc(%zu) failed\n", size);
        abort();
    }
    return p;
}

void *safe_realloc(void *ptr, size_t size) {
    if (size == 0) size = 1;
    void *p = realloc(ptr, size);
    if (!p) {
        fprintf(stderr, "dsco: fatal: realloc(%zu) failed\n", size);
        abort();
    }
    return p;
}

char *safe_strdup(const char *s) {
    if (!s) return NULL;
    char *p = strdup(s);
    if (!p) {
        fprintf(stderr, "dsco: fatal: strdup failed\n");
        abort();
    }
    return p;
}

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
    /* Guard against size_t overflow */
    size_t newcap = b->cap * 2;
    if (newcap < b->cap) newcap = b->len + need + 1; /* overflow: use exact */
    if (newcap < b->len + need + 1) newcap = b->len + need + 1;
    b->data = safe_realloc(b->data, newcap);
    b->cap = newcap;
}

void jbuf_append(jbuf_t *b, const char *s) {
    size_t n = strlen(s);
    jbuf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void jbuf_appendf(jbuf_t *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) jbuf_append_len(b, tmp, (size_t)(n < (int)sizeof(tmp) ? n : (int)sizeof(tmp)-1));
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
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p == '"')       { jbuf_append(b, "\\\""); p++; }
        else if (*p == '\\') { jbuf_append(b, "\\\\"); p++; }
        else if (*p == '\b') { jbuf_append(b, "\\b");  p++; }
        else if (*p == '\f') { jbuf_append(b, "\\f");  p++; }
        else if (*p == '\n') { jbuf_append(b, "\\n");  p++; }
        else if (*p == '\r') { jbuf_append(b, "\\r");  p++; }
        else if (*p == '\t') { jbuf_append(b, "\\t");  p++; }
        else if (*p < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", *p);
            jbuf_append(b, esc);
            p++;
        } else if (*p >= 0x80) {
            /* Validate UTF-8 and reject surrogates / overlong / invalid sequences */
            unsigned int cp = 0;
            int expect = 0;
            if ((*p & 0xE0) == 0xC0)      { cp = *p & 0x1F; expect = 1; }
            else if ((*p & 0xF0) == 0xE0)  { cp = *p & 0x0F; expect = 2; }
            else if ((*p & 0xF8) == 0xF0)  { cp = *p & 0x07; expect = 3; }
            else { jbuf_append(b, "\xEF\xBF\xBD"); p++; continue; } /* invalid lead byte -> U+FFFD */

            const unsigned char *start = p;
            bool valid = true;
            p++;
            for (int i = 0; i < expect; i++, p++) {
                if ((*p & 0xC0) != 0x80) { valid = false; break; }
                cp = (cp << 6) | (*p & 0x3F);
            }
            /* Reject surrogates (U+D800..U+DFFF) and overlong encodings */
            if (!valid || (cp >= 0xD800 && cp <= 0xDFFF) ||
                (expect == 1 && cp < 0x80) ||
                (expect == 2 && cp < 0x800) ||
                (expect == 3 && cp < 0x10000) ||
                cp > 0x10FFFF) {
                jbuf_append(b, "\xEF\xBF\xBD"); /* U+FFFD */
                if (!valid) continue; /* p already advanced past the bad continuation */
            } else {
                jbuf_append_len(b, (const char *)start, (size_t)(p - start));
            }
        } else {
            jbuf_append_char(b, (char)*p);
            p++;
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
            if (!*p) break;  /* truncated escape at end of input */
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
                    /* Handle UTF-16 surrogate pairs: \uD800-\uDBFF followed by \uDC00-\uDFFF */
                    if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
                        unsigned int lo = 0;
                        const char *q = p + 3; /* skip \u */
                        for (int i = 0; i < 4 && *q; i++, q++) {
                            lo = lo * 16;
                            if (*q >= '0' && *q <= '9') lo += *q - '0';
                            else if (*q >= 'a' && *q <= 'f') lo += *q - 'a' + 10;
                            else if (*q >= 'A' && *q <= 'F') lo += *q - 'A' + 10;
                        }
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p = q - 1; /* advance past the low surrogate */
                        }
                    }
                    /* Drop lone surrogates — they produce invalid UTF-8 */
                    if (cp >= 0xD800 && cp <= 0xDFFF) {
                        cp = 0xFFFD; /* replacement character */
                    }
                    if (cp < 0x80) {
                        jbuf_append_char(&b, (char)cp);
                    } else if (cp < 0x800) {
                        jbuf_append_char(&b, (char)(0xC0 | (cp >> 6)));
                        jbuf_append_char(&b, (char)(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        jbuf_append_char(&b, (char)(0xE0 | (cp >> 12)));
                        jbuf_append_char(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        jbuf_append_char(&b, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        jbuf_append_char(&b, (char)(0xF0 | (cp >> 18)));
                        jbuf_append_char(&b, (char)(0x80 | ((cp >> 12) & 0x3F)));
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
    if (!*p) return p;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        if (*p == '"') p++;
        return p;
    }
    if (*p == '{') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
                /* If unterminated string, don't advance past \0 */
                if (!*p) return p;
            }
            p++;
        }
        return p;
    }
    if (*p == '[') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
                /* If unterminated string, don't advance past \0 */
                if (!*p) return p;
            }
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
            if (!after || after <= p) { free(k); return NULL; }
            after = skip_ws(after);
            if (*after == ':') after++;
            after = skip_ws(after);
            if (k && strcmp(k, key) == 0) { free(k); return after; }
            free(k);
            const char *before_skip = after;
            after = skip_value(after);
            /* Guard: if skip_value didn't advance, bail to prevent infinite loop */
            if (after <= before_skip && *after) { return NULL; }
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
        const char *before = p;
        p = skip_value(p);
        /* Guard: if skip_value didn't advance, bail to prevent infinite loop */
        if (p <= before && *p) break;
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
    out->blocks = safe_malloc(cap * sizeof(content_block_t));
    memset(out->blocks, 0, cap * sizeof(content_block_t));
    out->count = 0;

    vp = skip_ws(vp);
    while (*vp && *vp != ']') {
        if (*vp == '{') {
            if (out->count >= cap) {
                cap *= 2;
                out->blocks = safe_realloc(out->blocks, cap * sizeof(content_block_t));
            }
            out->blocks[out->count] = parse_content_block(vp);
            out->count++;
            const char *before = vp;
            vp = skip_value(vp);
            if (vp <= before && *vp) break;
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

/* ── Arena allocator ──────────────────────────────────────────────────── */

void arena_init(arena_t *a) {
    memset(a, 0, sizeof(*a));
}

static arena_chunk_t *arena_new_chunk(size_t cap) {
    arena_chunk_t *c = malloc(sizeof(arena_chunk_t) + cap);
    if (!c) {
        fprintf(stderr, "dsco: fatal: arena chunk alloc(%zu) failed\n", cap);
        abort();
    }
    c->next = NULL;
    c->cap = cap;
    c->used = 0;
    return c;
}

void *arena_alloc(arena_t *a, size_t size) {
    /* 8-byte alignment */
    size = (size + 7) & ~(size_t)7;

    /* Oversized allocs go to separate malloc chain */
    if (size > ARENA_OVERSIZE) {
        arena_oversize_t *ov = malloc(sizeof(arena_oversize_t));
        void *ptr = malloc(size);
        if (!ov || !ptr) {
            fprintf(stderr, "dsco: fatal: arena oversize alloc(%zu) failed\n", size);
            abort();
        }
        ov->ptr = ptr;
        ov->next = a->oversized;
        a->oversized = ov;
        a->total_allocated += size;
        return ptr;
    }

    /* Try current head chunk */
    if (a->head && (a->head->used + size <= a->head->cap)) {
        void *ptr = a->head->data + a->head->used;
        a->head->used += size;
        a->total_allocated += size;
        return ptr;
    }

    /* Need new chunk */
    size_t chunk_cap = ARENA_CHUNK_SIZE;
    if (size > chunk_cap) chunk_cap = size;
    arena_chunk_t *c = arena_new_chunk(chunk_cap);
    c->next = a->head;
    a->head = c;

    void *ptr = c->data + c->used;
    c->used += size;
    a->total_allocated += size;
    return ptr;
}

char *arena_strdup(arena_t *a, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *p = arena_alloc(a, len);
    memcpy(p, s, len);
    return p;
}

void arena_reset(arena_t *a) {
    /* Reset all chunks to empty (but keep allocated) */
    for (arena_chunk_t *c = a->head; c; c = c->next) {
        c->used = 0;
    }
    /* Free oversized allocs */
    arena_oversize_t *ov = a->oversized;
    while (ov) {
        arena_oversize_t *next = ov->next;
        free(ov->ptr);
        free(ov);
        ov = next;
    }
    a->oversized = NULL;
    a->total_allocated = 0;
}

void arena_free(arena_t *a) {
    arena_chunk_t *c = a->head;
    while (c) {
        arena_chunk_t *next = c->next;
        free(c);
        c = next;
    }
    arena_oversize_t *ov = a->oversized;
    while (ov) {
        arena_oversize_t *next = ov->next;
        free(ov->ptr);
        free(ov);
        ov = next;
    }
    memset(a, 0, sizeof(*a));
}

/* Arena-backed parse: all string allocs come from arena.
   json_free_response becomes a no-op when arena is used. */
static content_block_t parse_content_block_arena(const char *p, arena_t *arena) {
    content_block_t blk = {0};
    if (*p != '{') return blk;

    const char *obj_start = p + 1;
    const char *vp = find_key(obj_start, "type");
    if (vp && *vp == '"') {
        char *tmp = NULL;
        parse_string(vp, &tmp);
        blk.type = arena_strdup(arena, tmp);
        free(tmp);
    }

    if (blk.type && strcmp(blk.type, "text") == 0) {
        vp = find_key(obj_start, "text");
        if (vp && *vp == '"') {
            char *tmp = NULL;
            parse_string(vp, &tmp);
            blk.text = arena_strdup(arena, tmp);
            free(tmp);
        }
    } else if (blk.type && strcmp(blk.type, "tool_use") == 0) {
        vp = find_key(obj_start, "name");
        if (vp && *vp == '"') {
            char *tmp = NULL;
            parse_string(vp, &tmp);
            blk.tool_name = arena_strdup(arena, tmp);
            free(tmp);
        }
        vp = find_key(obj_start, "id");
        if (vp && *vp == '"') {
            char *tmp = NULL;
            parse_string(vp, &tmp);
            blk.tool_id = arena_strdup(arena, tmp);
            free(tmp);
        }
        vp = find_key(obj_start, "input");
        if (vp) {
            const char *start, *end;
            extract_raw_value(vp, &start, &end);
            size_t len = (size_t)(end - start);
            blk.tool_input = arena_alloc(arena, len + 1);
            memcpy(blk.tool_input, start, len);
            blk.tool_input[len] = '\0';
        }
    }
    return blk;
}

bool json_parse_response_arena(const char *json, parsed_response_t *out, arena_t *arena) {
    if (!arena) return json_parse_response(json, out);

    memset(out, 0, sizeof(*out));
    const char *p = skip_ws(json);
    if (*p != '{') return false;
    p++;

    const char *vp = find_key(p, "stop_reason");
    if (vp && *vp == '"') {
        char *tmp = NULL;
        parse_string(vp, &tmp);
        out->stop_reason = arena_strdup(arena, tmp);
        free(tmp);
    }

    vp = find_key(p, "content");
    if (!vp || *vp != '[') return false;
    vp++;

    int cap = 8;
    out->blocks = arena_alloc(arena, cap * sizeof(content_block_t));
    memset(out->blocks, 0, cap * sizeof(content_block_t));
    out->count = 0;

    vp = skip_ws(vp);
    while (*vp && *vp != ']') {
        if (*vp == '{') {
            if (out->count >= cap) {
                int new_cap = cap * 2;
                content_block_t *new_blocks = arena_alloc(arena, new_cap * sizeof(content_block_t));
                memcpy(new_blocks, out->blocks, cap * sizeof(content_block_t));
                out->blocks = new_blocks;
                cap = new_cap;
            }
            out->blocks[out->count] = parse_content_block_arena(vp, arena);
            out->count++;
            const char *before = vp;
            vp = skip_value(vp);
            if (vp <= before && *vp) break;
        }
        vp = skip_ws(vp);
        if (*vp == ',') vp++;
        vp = skip_ws(vp);
    }
    return true;
}

/* ── JSON schema validator ────────────────────────────────────────────── */

/* Peek at first non-whitespace char to infer JSON type */
static char json_peek_type(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return *p;
}

/* Check if a key exists at the top level of a JSON object */
static bool json_has_key(const char *json, const char *key) {
    const char *p = skip_ws(json);
    if (*p != '{') return false;
    return find_key(p + 1, key) != NULL;
}

json_validation_t json_validate_schema(const char *json, const char *schema_json) {
    json_validation_t result = { .valid = true, .error = "", .field = "" };

    if (!json || !schema_json) {
        result.valid = false;
        snprintf(result.error, sizeof(result.error), "null input or schema");
        return result;
    }

    /* Quick check: input should be an object */
    const char *jp = skip_ws(json);
    if (*jp != '{') {
        result.valid = false;
        snprintf(result.error, sizeof(result.error), "input is not a JSON object");
        return result;
    }

    const char *sp = skip_ws(schema_json);
    if (*sp != '{') return result; /* can't validate without schema */

    /* Extract "required" array from schema */
    const char *req = find_key(sp + 1, "required");
    if (req) {
        req = skip_ws(req);
        if (*req == '[') {
            req++;
            req = skip_ws(req);
            while (*req && *req != ']') {
                if (*req == '"') {
                    char *field = NULL;
                    const char *after = parse_string(req, &field);
                    if (field) {
                        if (!json_has_key(json, field)) {
                            result.valid = false;
                            snprintf(result.field, sizeof(result.field), "%s", field);
                            snprintf(result.error, sizeof(result.error),
                                     "missing required field: %s", field);
                            free(field);
                            return result;
                        }
                        free(field);
                    }
                    if (!after || after <= req) break;
                    req = skip_ws(after);
                    if (*req == ',') req++;
                    req = skip_ws(req);
                } else {
                    break;
                }
            }
        }
    }

    /* Extract "properties" from schema and type-check present fields */
    const char *props = find_key(sp + 1, "properties");
    if (props) {
        props = skip_ws(props);
        if (*props == '{') {
            props++;
            props = skip_ws(props);
            while (*props && *props != '}') {
                if (*props == '"') {
                    char *prop_name = NULL;
                    const char *after = parse_string(props, &prop_name);
                    if (!after || after <= props) { free(prop_name); break; }
                    after = skip_ws(after);
                    if (*after == ':') after++;
                    after = skip_ws(after);

                    /* Get expected type from property schema */
                    char *expected_type = NULL;
                    if (*after == '{') {
                        const char *type_val = find_key(after + 1, "type");
                        if (type_val && *type_val == '"') {
                            parse_string(type_val, &expected_type);
                        }
                    }

                    /* Check actual value type if field exists */
                    if (prop_name && expected_type && json_has_key(json, prop_name)) {
                        const char *val = find_key(jp + 1, prop_name);
                        if (val) {
                            char actual = json_peek_type(val);
                            bool type_ok = true;
                            if (strcmp(expected_type, "string") == 0 && actual != '"')
                                type_ok = false;
                            else if (strcmp(expected_type, "object") == 0 && actual != '{')
                                type_ok = false;
                            else if (strcmp(expected_type, "array") == 0 && actual != '[')
                                type_ok = false;
                            else if (strcmp(expected_type, "boolean") == 0 &&
                                     actual != 't' && actual != 'f')
                                type_ok = false;
                            else if (strcmp(expected_type, "number") == 0 &&
                                     actual != '-' && !(actual >= '0' && actual <= '9'))
                                type_ok = false;
                            else if (strcmp(expected_type, "integer") == 0 &&
                                     actual != '-' && !(actual >= '0' && actual <= '9'))
                                type_ok = false;

                            if (!type_ok) {
                                result.valid = false;
                                snprintf(result.field, sizeof(result.field), "%s", prop_name);
                                snprintf(result.error, sizeof(result.error),
                                         "field '%s': expected %s", prop_name, expected_type);
                                free(prop_name);
                                free(expected_type);
                                return result;
                            }
                        }
                    }

                    free(prop_name);
                    free(expected_type);

                    /* Skip past property schema value */
                    const char *before = after;
                    after = skip_value(after);
                    if (after <= before && *after) break;
                    after = skip_ws(after);
                    if (*after == ',') after++;
                    props = skip_ws(after);
                } else {
                    break;
                }
            }
        }
    }

    return result;
}
