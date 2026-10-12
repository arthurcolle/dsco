/* Bounded, pure JSON analysis. No filesystem, process, or network effects. */
#include "json_tools.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_LIMIT (1024u * 1024u)
#define NODE_LIMIT 20000u
#define DEPTH_LIMIT 64u
#define MEMBER_LIMIT 1024u
#define DIFF_LIMIT 128u

static bool error_result(char *out, size_t cap, const char *message) {
    if (cap) snprintf(out, cap, "{\"error\":\"%s\"}", message);
    return false;
}

/* Bound recursion/work and reject duplicate object keys, including NUL keys. */
static bool validate(yyjson_val *v, unsigned depth, size_t *nodes) {
    if (depth > DEPTH_LIMIT || ++*nodes > NODE_LIMIT) return false;
    if (yyjson_is_arr(v)) {
        size_t i, n; yyjson_val *item;
        yyjson_arr_foreach(v, i, n, item) {
            if (!validate(item, depth + 1, nodes)) return false;
        }
    } else if (yyjson_is_obj(v)) {
        if (yyjson_obj_size(v) > MEMBER_LIMIT) return false;
        size_t i, n; yyjson_val *key, *value;
        yyjson_obj_foreach(v, i, n, key, value) {
            yyjson_obj_iter iter = yyjson_obj_iter_with(v);
            yyjson_val *other;
            for (size_t j = 0; j < i; j++) {
                other = yyjson_obj_iter_next(&iter);
                if (yyjson_get_len(key) == yyjson_get_len(other) &&
                    !memcmp(yyjson_get_str(key), yyjson_get_str(other), yyjson_get_len(key)))
                    return false;
            }
            if (!validate(value, depth + 1, nodes)) return false;
        }
    }
    return true;
}

static yyjson_doc *parse_text(yyjson_val *text) {
    if (!yyjson_is_str(text)) return NULL;
    size_t len = yyjson_get_len(text);
    if (len > JSON_LIMIT || strlen(yyjson_get_str(text)) != len) return NULL;
    yyjson_doc *doc = yyjson_read(yyjson_get_str(text), len, 0);
    if (!doc) return NULL;
    size_t nodes = 0;
    if (!validate(yyjson_doc_get_root(doc), 0, &nodes)) {
        yyjson_doc_free(doc); return NULL;
    }
    return doc;
}

static bool emit(yyjson_mut_doc *doc, char *out, size_t cap) {
    size_t len = 0;
    char *json = yyjson_mut_write(doc, 0, &len);
    if (!json) return error_result(out, cap, "JSON serialization failed");
    if (len >= cap) {
        free(json); return error_result(out, cap, "Result exceeds output buffer");
    }
    memcpy(out, json, len + 1); free(json); return true;
}

static yyjson_doc *parse_args(const char *input) {
    if (!input || strlen(input) > 8u * JSON_LIMIT) return NULL;
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0);
    if (doc && !yyjson_is_obj(yyjson_doc_get_root(doc))) {
        yyjson_doc_free(doc); return NULL;
    }
    return doc;
}

bool tool_json_pointer(const char *input, char *out, size_t cap) {
    yyjson_doc *args = parse_args(input);
    if (!args) return error_result(out, cap, "Arguments must be a JSON object");
    yyjson_val *root = yyjson_doc_get_root(args);
    yyjson_val *pointer = yyjson_obj_get(root, "pointer");
    yyjson_doc *doc = parse_text(yyjson_obj_get(root, "json"));
    if (!doc || !yyjson_is_str(pointer) ||
        strlen(yyjson_get_str(pointer)) != yyjson_get_len(pointer)) {
        if (doc) yyjson_doc_free(doc);
        yyjson_doc_free(args);
        return error_result(out, cap, "Invalid JSON or pointer; limits: 1 MiB, depth 64, 20000 nodes, 1024 object keys; duplicate keys rejected");
    }
    const char *p = yyjson_get_str(pointer);
    /* Syntax validation is separate from a well-formed pointer with no match. */
    bool valid = !*p || *p == '/';
    for (size_t i = 0; valid && p[i]; i++) {
        if (p[i] == '~') {
            if (p[i + 1] != '0' && p[i + 1] != '1') valid = false;
            else i++;
        }
    }
    if (!valid) {
        yyjson_doc_free(doc); yyjson_doc_free(args);
        return error_result(out, cap, "Invalid RFC 6901 JSON pointer");
    }
    yyjson_val *value = yyjson_ptr_get(yyjson_doc_get_root(doc), p);
    yyjson_mut_doc *response = yyjson_mut_doc_new(NULL);
    bool ok = false;
    if (response) {
        yyjson_mut_val *obj = yyjson_mut_obj(response);
        yyjson_mut_doc_set_root(response, obj);
        if (obj && yyjson_mut_obj_add_bool(response, obj, "found", value != NULL) &&
            (!value || yyjson_mut_obj_add_val(response, obj, "value", yyjson_val_mut_copy(response, value))))
            ok = emit(response, out, cap);
        else error_result(out, cap, "JSON allocation failed");
        yyjson_mut_doc_free(response);
    } else error_result(out, cap, "JSON allocation failed");
    yyjson_doc_free(doc); yyjson_doc_free(args); return ok;
}

typedef struct {
    yyjson_mut_doc *doc;
    yyjson_mut_val *rows;
    size_t count, limit;
    bool different, truncated, failed;
} diff_state;

static void difference(diff_state *s, const char *path, size_t len, const char *kind) {
    s->different = true;
    if (s->count >= s->limit) { s->truncated = true; return; }
    yyjson_mut_val *row = yyjson_mut_obj(s->doc);
    if (!row || !yyjson_mut_obj_add_val(s->doc, row, "path", yyjson_mut_strncpy(s->doc, path, len)) ||
        !yyjson_mut_obj_add_str(s->doc, row, "kind", kind) || !yyjson_mut_arr_add_val(s->rows, row)) {
        s->failed = true; return;
    }
    s->count++;
}

static char *child_path(const char *parent, size_t plen, const char *key, size_t klen, size_t *len) {
    if (plen > 2u * JSON_LIMIT || klen > JSON_LIMIT) return NULL;
    char *path = malloc(plen + 2 * klen + 2);
    if (!path) return NULL;
    memcpy(path, parent, plen);
    size_t n = plen; path[n++] = '/';
    for (size_t i = 0; i < klen; i++) {
        if (key[i] == '~' || key[i] == '/') {
            path[n++] = '~'; path[n++] = key[i] == '~' ? '0' : '1';
        } else path[n++] = key[i];
    }
    path[n] = 0; *len = n; return path;
}

static void compare(diff_state *s, yyjson_val *a, yyjson_val *b, const char *path, size_t plen);
static void compare_child(diff_state *s, yyjson_val *a, yyjson_val *b, const char *path,
                          size_t plen, const char *key, size_t klen) {
    size_t len = 0; char *child = child_path(path, plen, key, klen, &len);
    if (!child) { s->failed = true; return; }
    compare(s, a, b, child, len); free(child);
}

static void compare(diff_state *s, yyjson_val *a, yyjson_val *b, const char *path, size_t plen) {
    if (s->failed || s->truncated) return;
    if (!a || !b) { difference(s, path, plen, !a ? "added" : "removed"); return; }
    if (yyjson_is_obj(a) && yyjson_is_obj(b)) {
        size_t i, n; yyjson_val *key, *value;
        yyjson_obj_foreach(a, i, n, key, value) {
            if (s->truncated || s->failed) break;
            compare_child(s, value, yyjson_obj_getn(b, yyjson_get_str(key), yyjson_get_len(key)),
                          path, plen, yyjson_get_str(key), yyjson_get_len(key));
        }
        yyjson_obj_foreach(b, i, n, key, value) {
            if (s->truncated || s->failed) break;
            if (!yyjson_obj_getn(a, yyjson_get_str(key), yyjson_get_len(key)))
                compare_child(s, NULL, value, path, plen, yyjson_get_str(key), yyjson_get_len(key));
        }
    } else if (yyjson_is_arr(a) && yyjson_is_arr(b)) {
        size_t an = yyjson_arr_size(a), bn = yyjson_arr_size(b), n = an > bn ? an : bn;
        yyjson_arr_iter ai = yyjson_arr_iter_with(a), bi = yyjson_arr_iter_with(b);
        for (size_t i = 0; i < n && !s->truncated && !s->failed; i++) {
            char index[32]; int len = snprintf(index, sizeof(index), "%zu", i);
            compare_child(s, yyjson_arr_iter_next(&ai), yyjson_arr_iter_next(&bi),
                          path, plen, index, (size_t)len);
        }
    } else if (!yyjson_equals(a, b)) difference(s, path, plen, "changed");
}

bool tool_json_diff(const char *input, char *out, size_t cap) {
    yyjson_doc *args = parse_args(input);
    if (!args) return error_result(out, cap, "Arguments must be a JSON object");
    yyjson_val *root = yyjson_doc_get_root(args), *limit = yyjson_obj_get(root, "max_differences");
    if (limit && (!yyjson_is_uint(limit) || yyjson_get_uint(limit) < 1 || yyjson_get_uint(limit) > DIFF_LIMIT)) {
        yyjson_doc_free(args); return error_result(out, cap, "max_differences must be an integer from 1 to 128");
    }
    yyjson_doc *a = parse_text(yyjson_obj_get(root, "left"));
    yyjson_doc *b = parse_text(yyjson_obj_get(root, "right"));
    if (!a || !b) {
        if (a) yyjson_doc_free(a);
        if (b) yyjson_doc_free(b);
        yyjson_doc_free(args);
        return error_result(out, cap, "Invalid JSON; limits: 1 MiB, depth 64, 20000 nodes, 1024 object keys; duplicate keys rejected");
    }
    diff_state s = {0}; s.limit = limit ? (size_t)yyjson_get_uint(limit) : DIFF_LIMIT;
    s.doc = yyjson_mut_doc_new(NULL);
    bool ok = false;
    if (s.doc) {
        yyjson_mut_val *obj = yyjson_mut_obj(s.doc);
        s.rows = yyjson_mut_arr(s.doc);
        yyjson_mut_doc_set_root(s.doc, obj);
        if (obj && s.rows) {
            compare(&s, yyjson_doc_get_root(a), yyjson_doc_get_root(b), "", 0);
            if (!s.failed && yyjson_mut_obj_add_bool(s.doc, obj, "equal", !s.different) &&
                yyjson_mut_obj_add_val(s.doc, obj, "differences", s.rows) &&
                yyjson_mut_obj_add_uint(s.doc, obj, "count", s.count) &&
                yyjson_mut_obj_add_bool(s.doc, obj, "truncated", s.truncated)) ok = emit(s.doc, out, cap);
            else error_result(out, cap, "JSON allocation failed");
        } else error_result(out, cap, "JSON allocation failed");
        yyjson_mut_doc_free(s.doc);
    } else error_result(out, cap, "JSON allocation failed");
    yyjson_doc_free(a); yyjson_doc_free(b); yyjson_doc_free(args); return ok;
}
