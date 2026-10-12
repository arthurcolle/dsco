#include "presentation_json.h"
#include "json_util.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *sanitize(char *display) {
    for (char *p = display; p && *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c < 32 && c != '\n' && c != '\t') || c == 127) *p = '?';
    }
    return display;
}

/* Both ANSI and native tool views use this projection. Preserve the original
 * JSON in the transcript; render provenance and actual diffs for inspection. */
static char *harness_receipt(yyjson_val *root, bool expanded) {
    yyjson_val *kind = yyjson_obj_get(root, "kind");
    bool edit = yyjson_equals_str(kind, "code_edit"), skill = yyjson_equals_str(kind, "skill");
    if (!edit && !skill) return NULL;
    const char *path = yyjson_get_str(yyjson_obj_get(root, "path"));
    const char *error = yyjson_get_str(yyjson_obj_get(root, "error"));
    const char *status = yyjson_get_str(yyjson_obj_get(root, edit ? "status" : "action"));
    if (error || !path || !status) return NULL;
    jbuf_t b; jbuf_init(&b, 512);
    if (edit) {
        const char *before = yyjson_get_str(yyjson_obj_get(root, "before_sha256"));
        const char *after = yyjson_get_str(yyjson_obj_get(root, "after_sha256"));
        jbuf_appendf(&b, "Edit %s · %s", status, path);
        if (expanded) {
            jbuf_appendf(&b, "\nVersion: %s → %s", before ? before : "unknown", after ? after : "unknown");
            const char *verify = yyjson_get_bool(yyjson_obj_get(root, "verified")) ? "verified" : "unverified";
            jbuf_appendf(&b, "\nReadback: %s · directory sync: %s", verify,
                         yyjson_get_bool(yyjson_obj_get(root, "durable")) ? "confirmed" : "not confirmed");
            const char *warning = yyjson_get_str(yyjson_obj_get(root, "warning"));
            if (warning) jbuf_appendf(&b, "\nWarning: %s", warning);
            const char *diff = yyjson_get_str(yyjson_obj_get(root, "diff"));
            if (diff && *diff) { jbuf_append(&b, "\n\n"); jbuf_append(&b, diff); }
        }
    } else {
        const char *name = yyjson_get_str(yyjson_obj_get(root, "name"));
        const char *hash = yyjson_get_str(yyjson_obj_get(root, "sha256"));
        jbuf_appendf(&b, "Skill %s · %s · %.12s", status, name ? name : "unknown", hash ? hash : "unknown");
        if (expanded) {
            jbuf_appendf(&b, "\nPath: %s\nVersion: %s\nScope: this invocation", path, hash ? hash : "unknown");
            const char *previous = yyjson_get_str(yyjson_obj_get(root, "previous_path"));
            if (previous) jbuf_appendf(&b, "\nPrevious revision: %s", previous);
            const char *instructions = yyjson_get_str(yyjson_obj_get(root, "instructions"));
            if (instructions) { jbuf_append(&b, "\n\n"); jbuf_append(&b, instructions); }
            yyjson_val *receipt = yyjson_obj_get(root, "edit_receipt");
            if (receipt) {
                char *detail = harness_receipt(receipt, true);
                if (detail) { jbuf_append(&b, "\n\n"); jbuf_append(&b, detail); free(detail); }
            }
        }
    }
    return b.data;
}

/* Tool protocols often wrap a human-readable JSON/text result in output/text.
 * Unbox exactly one wrapper only for display; stored tool bytes are untouched. */
static char *unbox_display(yyjson_val *root, bool expanded, int depth) {
    if (depth >= 3 || !yyjson_is_obj(root)) return NULL;
    const char *keys[] = {"output", "stdout", "text", "result", "content", "message", "error"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        yyjson_val *field = yyjson_obj_get(root, keys[i]);
        const char *value = yyjson_get_str(field);
        if (!value) continue;
        /* A decoded JSON string may itself be JSON. Recurse only on valid
         * containers; ordinary source/text stays literal and keeps newlines. */
        if (expanded) {
            char *nested = presentation_json(value, true);
            if (nested) return nested;
        }
        return strdup(value);
    }
    return NULL;
}

char *presentation_json(const char *text, bool expanded) {
    if (!text) return NULL;
    size_t bytes = strnlen(text, 65537);
    if (bytes > 65536) return NULL;
    /* Preserve numeric lexemes, including integers wider than native types. */
    yyjson_doc *doc = yyjson_read(text, bytes, YYJSON_READ_NUMBER_AS_RAW);
    if (!doc) return NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root) && !yyjson_is_arr(root)) { yyjson_doc_free(doc); return NULL; }
    char *display = harness_receipt(root, expanded);
    if (expanded && !display) display = unbox_display(root, true, 0);
    if (!expanded && !display) {
        const char *keys[] = {"error", "message", "summary", "output", "stdout", "status"};
        for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
            const char *value = yyjson_get_str(yyjson_obj_get(root, keys[i]));
            if (value && value[0]) {
                yyjson_val *field = yyjson_obj_get(root, keys[i]);
                display = strlen(value) == yyjson_get_len(field) ? strdup(value)
                          : yyjson_write(doc, YYJSON_WRITE_PRETTY, NULL);
                break;
            }
        }
        if (!display) {
            char label[128];
            snprintf(label, sizeof(label), "Structured result (%zu bytes) · Ctrl-T to inspect", bytes);
            display = strdup(label);
        }
    } else if (!display) display = yyjson_write(doc, YYJSON_WRITE_PRETTY, NULL);
    yyjson_doc_free(doc);
    return sanitize(display);
}
