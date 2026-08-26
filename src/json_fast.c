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
/* Writer stays enabled: json_schema_ensure_property_types() rewrites tool
 * schemas via the mutable doc API and serializes the patched copy. */
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

/* ── Kimi/Moonshot tool-schema normalizer ─────────────────────────────────
 * Moonshot's validator rejects tool parameter schemas whose property schemas
 * omit "type" (e.g. enum-only properties from MCP servers): HTTP 400
 * "At path 'properties.X': type is not defined". JSON Schema permits the
 * omission and OpenAI/Anthropic accept it, so patch schemas client-side the
 * same way the official Kimi CLI does (kosong ensure_property_types). */

static const char *const k_schema_combinators[] = {
    "anyOf", "oneOf", "allOf", "not", "if", "then", "else", "$ref", NULL,
};
static const char *const k_schema_object_keys[] = {
    "properties", "additionalProperties", "patternProperties", "propertyNames",
    "required",   "minProperties",        "maxProperties",     NULL,
};
static const char *const k_schema_array_keys[] = {
    "items", "prefixItems", "minItems", "maxItems", "uniqueItems", "contains", NULL,
};
static const char *const k_schema_string_keys[] = {
    "minLength", "maxLength", "pattern", "format", NULL,
};
static const char *const k_schema_numeric_keys[] = {
    "minimum", "maximum", "multipleOf", "exclusiveMinimum", "exclusiveMaximum", NULL,
};

static bool schema_obj_has_any(yyjson_mut_val *obj, const char *const keys[]) {
    for (int i = 0; keys[i]; i++)
        if (yyjson_mut_obj_get(obj, keys[i]))
            return true;
    return false;
}

/* Classify one concrete JSON value into a JSON Schema type bucket. */
typedef struct {
    bool str, boolean, integer, real, other;
} schema_value_kinds_t;

static void schema_classify_value(yyjson_mut_val *v, schema_value_kinds_t *k) {
    if (yyjson_mut_is_str(v))
        k->str = true;
    else if (yyjson_mut_is_bool(v))
        k->boolean = true;
    else if (yyjson_mut_is_int(v))
        k->integer = true;
    else if (yyjson_mut_is_real(v))
        k->real = true;
    else
        k->other = true;
}

/* Single type -> that type; {integer, number} -> number; anything else
 * mixed -> "string", which Moonshot tolerates without cross-checking enum
 * values against the declared type. */
static const char *schema_kinds_to_type(const schema_value_kinds_t *k) {
    int buckets = (k->str ? 1 : 0) + (k->boolean ? 1 : 0) +
                  ((k->integer || k->real) ? 1 : 0) + (k->other ? 1 : 0);
    if (k->other || buckets != 1)
        return "string";
    if (k->str)
        return "string";
    if (k->boolean)
        return "boolean";
    if (k->real)
        return "number";
    return "integer";
}

static const char *schema_infer_type_from_structure(yyjson_mut_val *obj) {
    if (schema_obj_has_any(obj, k_schema_object_keys))
        return "object";
    if (schema_obj_has_any(obj, k_schema_array_keys))
        return "array";
    if (schema_obj_has_any(obj, k_schema_string_keys))
        return "string";
    if (schema_obj_has_any(obj, k_schema_numeric_keys))
        return "number";
    return "string";
}

static void schema_recurse(yyjson_mut_doc *doc, yyjson_mut_val *node, bool *changed);

static void schema_normalize_property(yyjson_mut_doc *doc, yyjson_mut_val *node, bool *changed) {
    if (!yyjson_mut_is_obj(node))
        return;
    if (!yyjson_mut_obj_get(node, "type") && !schema_obj_has_any(node, k_schema_combinators)) {
        const char *type = NULL;
        yyjson_mut_val *en = yyjson_mut_obj_get(node, "enum");
        yyjson_mut_val *cv = yyjson_mut_obj_get(node, "const");
        if (en && yyjson_mut_is_arr(en) && yyjson_mut_arr_size(en) > 0) {
            schema_value_kinds_t kinds = {0};
            size_t idx, max;
            yyjson_mut_val *v;
            yyjson_mut_arr_foreach(en, idx, max, v) schema_classify_value(v, &kinds);
            type = schema_kinds_to_type(&kinds);
        } else if (cv) {
            schema_value_kinds_t kinds = {0};
            schema_classify_value(cv, &kinds);
            type = schema_kinds_to_type(&kinds);
        } else {
            type = schema_infer_type_from_structure(node);
        }
        yyjson_mut_obj_add_str(doc, node, "type", type);
        *changed = true;
    }
    schema_recurse(doc, node, changed);
}

/* Walk property-schema positions under `node`; `node` itself is a container
 * and is not normalized (mirrors kosong's _recurse_schema). */
static void schema_recurse(yyjson_mut_doc *doc, yyjson_mut_val *node, bool *changed) {
    if (!yyjson_mut_is_obj(node))
        return;

    yyjson_mut_val *props = yyjson_mut_obj_get(node, "properties");
    if (props && yyjson_mut_is_obj(props)) {
        size_t idx, max;
        yyjson_mut_val *key, *val;
        yyjson_mut_obj_foreach(props, idx, max, key, val)
            schema_normalize_property(doc, val, changed);
    }

    yyjson_mut_val *items = yyjson_mut_obj_get(node, "items");
    if (items && yyjson_mut_is_obj(items)) {
        schema_normalize_property(doc, items, changed);
    } else if (items && yyjson_mut_is_arr(items)) {
        size_t idx, max;
        yyjson_mut_val *v;
        yyjson_mut_arr_foreach(items, idx, max, v) schema_normalize_property(doc, v, changed);
    }

    yyjson_mut_val *additional = yyjson_mut_obj_get(node, "additionalProperties");
    if (additional && yyjson_mut_is_obj(additional))
        schema_normalize_property(doc, additional, changed);

    static const char *const branch_keys[] = {"anyOf", "oneOf", "allOf", NULL};
    for (int i = 0; branch_keys[i]; i++) {
        yyjson_mut_val *branches = yyjson_mut_obj_get(node, branch_keys[i]);
        if (branches && yyjson_mut_is_arr(branches)) {
            size_t idx, max;
            yyjson_mut_val *v;
            yyjson_mut_arr_foreach(branches, idx, max, v)
                schema_normalize_property(doc, v, changed);
        }
    }
}

char *json_schema_ensure_property_types(const char *schema_json) {
    if (!schema_json || !schema_json[0])
        return NULL;
    yyjson_doc *doc = yyjson_read(schema_json, strlen(schema_json), 0);
    if (!doc)
        return NULL;
    yyjson_mut_doc *mdoc = yyjson_doc_mut_copy(doc, NULL);
    yyjson_doc_free(doc);
    if (!mdoc)
        return NULL;
    bool changed = false;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(mdoc);
    if (yyjson_mut_is_obj(root))
        schema_recurse(mdoc, root, &changed);
    char *out = changed ? yyjson_mut_write(mdoc, 0, NULL) : NULL;
    yyjson_mut_doc_free(mdoc);
    return out;
}
