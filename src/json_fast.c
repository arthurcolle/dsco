/* json_fast.c — yyjson-backed parse-once "view" API (declared in json_util.h).
 *
 * The amalgamated yyjson is compiled directly into this translation unit, so it
 * propagates to every build variant (native/lite/cosmo/wasm/asan/ubsan) via
 * SRC_NAMES with no per-variant link surgery. Only this TU pulls in yyjson, so
 * there are no duplicate-symbol concerns elsewhere in the tree.
 *
 * Rationale + measured numbers live in json_util.h and bench/RESULTS.txt:
 * scan-to-key (json_get_*) wins one-shot single-field extraction; a parsed view
 * wins decisively (~66x) when many fields come from one document. */
#include "json_util.h"
#include <stdlib.h>
#include <string.h>

/* yyjson under DSCO's aggressive warning set: it is warning-clean upstream, but
 * pin the include locally so a future -Wextra bump can't fail this vendored TU. */
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#define YYJSON_DISABLE_WRITER 1 /* view is read-only; drops the writer code/size */
#include "../vendor/yyjson.c"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct json_view {
    yyjson_doc *doc;
    yyjson_val *root;
};

json_view_t *json_view_open(const char *json) {
    if (!json)
        return NULL;
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    if (!d)
        return NULL;
    yyjson_val *r = yyjson_doc_get_root(d);
    if (!yyjson_is_obj(r)) {
        yyjson_doc_free(d);
        return NULL;
    }
    json_view_t *v = malloc(sizeof(*v));
    if (!v) {
        yyjson_doc_free(d);
        return NULL;
    }
    v->doc = d;
    v->root = r;
    return v;
}

void json_view_close(json_view_t *v) {
    if (!v)
        return;
    yyjson_doc_free(v->doc);
    free(v);
}

bool json_view_ok(const json_view_t *v) {
    return v != NULL;
}

const char *json_view_str(json_view_t *v, const char *key) {
    if (!v)
        return NULL;
    yyjson_val *x = yyjson_obj_get(v->root, key);
    return (x && yyjson_is_str(x)) ? yyjson_get_str(x) : NULL;
}

long long json_view_i64(json_view_t *v, const char *key, long long def) {
    if (!v)
        return def;
    yyjson_val *x = yyjson_obj_get(v->root, key);
    if (!x)
        return def;
    if (yyjson_is_int(x))
        return yyjson_get_sint(x);
    if (yyjson_is_uint(x))
        return (long long)yyjson_get_uint(x);
    if (yyjson_is_real(x))
        return (long long)yyjson_get_real(x);
    return def;
}

double json_view_f64(json_view_t *v, const char *key, double def) {
    if (!v)
        return def;
    yyjson_val *x = yyjson_obj_get(v->root, key);
    return (x && yyjson_is_num(x)) ? yyjson_get_num(x) : def;
}

bool json_view_bool(json_view_t *v, const char *key, bool def) {
    if (!v)
        return def;
    yyjson_val *x = yyjson_obj_get(v->root, key);
    return (x && yyjson_is_bool(x)) ? yyjson_get_bool(x) : def;
}
