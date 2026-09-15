#include "buffer_ui.h"
#include "../vendor/yyjson.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *out; size_t used, cap; bool truncated; } preview_t;

static void append(preview_t *p, const char *fmt, ...) {
    if (p->truncated || p->used + 1 >= p->cap) { p->truncated = true; return; }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(p->out + p->used, p->cap - p->used, fmt, ap);
    va_end(ap);
    if (n < 0) { p->truncated = true; return; }
    if ((size_t)n >= p->cap - p->used) { p->used = p->cap - 1; p->truncated = true; }
    else p->used += (size_t)n;
}

static const char *field(yyjson_val *obj, const char *key) {
    const char *value = yyjson_get_str(yyjson_obj_get(obj, key));
    return value ? value : "";
}

static void metadata(preview_t *p, yyjson_val *buffer) {
    append(p, "%s (%s, %llu bytes, %s)\nBuffer ID: %s\nRevision: %s\n",
        field(buffer, "name"), field(buffer, "kind"),
        (unsigned long long)yyjson_get_uint(yyjson_obj_get(buffer, "bytes")),
        yyjson_is_true(yyjson_obj_get(buffer, "closed")) ? "closed; content retained" : "open",
        field(buffer, "buffer_id"), field(buffer, "revision"));
}

void buffer_ui_format_result(const char *json, bool ok, char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    preview_t p = {.out = out, .cap = cap > 128 ? cap - 96 : cap};
    size_t len = json ? strnlen(json, 1024u * 1024u) : 0;
    yyjson_doc *doc = len < 1024u * 1024u ? yyjson_read(json ? json : "", len, 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *buffers = yyjson_obj_get(root, "buffers");
    yyjson_val *buffer = yyjson_obj_get(root, "buffer");
    yyjson_val *views = yyjson_obj_get(root, "views");
    if (!root || !yyjson_is_obj(root)) {
        append(&p, "Buffer command %s. %s", ok ? "completed" : "failed",
            len < 1024u * 1024u && json ? json : "Result unavailable.");
    } else if (!ok || yyjson_is_false(yyjson_obj_get(root, "ok"))) {
        append(&p, "Buffer command failed: %s\n%s", field(root, "error"), field(root, "detail"));
    } else if (yyjson_is_arr(buffers)) {
        size_t i, count; yyjson_val *item;
        append(&p, "Buffers (%zu)\n", yyjson_arr_size(buffers));
        yyjson_arr_foreach(buffers, i, count, item) {
            if (i == 64) { p.truncated = true; break; }
            append(&p, "- %s (%s, %llu bytes%s)\n  ID: %s\n", field(item, "name"),
                field(item, "kind"), (unsigned long long)yyjson_get_uint(yyjson_obj_get(item, "bytes")),
                yyjson_is_true(yyjson_obj_get(item, "closed")) ? ", closed" : "",
                field(item, "buffer_id"));
        }
        append(&p, "\n/buffer edit NAME — edit here in native mode; Ctrl-S saves, Esc returns to chat.\n");
        append(&p, "/buffer new NAME · /buffer open NAME · /buffer view NAME\n");
    } else if (yyjson_is_arr(yyjson_obj_get(root, "windows"))) {
        append(&p, "Native buffer editor opened. Click its text to edit; Ctrl-S saves; Esc returns to chat.\n");
    } else if (yyjson_is_arr(views)) {
        if (yyjson_is_obj(buffer)) { metadata(&p, buffer); append(&p, "\n"); }
        append(&p, "Views (%zu) · workspace %s\n", yyjson_arr_size(views), field(root, "workspace"));
        size_t i, count; yyjson_val *view;
        yyjson_arr_foreach(views, i, count, view) {
            if (i == 64) { p.truncated = true; break; }
            append(&p, "- %s · %s · %llux%llu%s%s%s\n  Buffer ID: %s\n",
                field(view, "surface_id"), field(view, "mode"),
                (unsigned long long)yyjson_get_uint(yyjson_obj_get(view, "columns")),
                (unsigned long long)yyjson_get_uint(yyjson_obj_get(view, "rows")),
                yyjson_is_true(yyjson_obj_get(view, "focused")) ? " · focused" : "",
                yyjson_is_true(yyjson_obj_get(view, "closed")) ? " · closed" : "",
                yyjson_is_true(yyjson_obj_get(view, "pending")) ? " · pending" : "",
                field(view, "buffer_id"));
        }
        if (*field(root, "detail")) append(&p, "\n%s\n", field(root, "detail"));
        append(&p, "\n/buffer focus {\"surface_id\":\"<ID>\"} · /buffer close-view {\"surface_id\":\"<ID>\"}\n");
    } else if (yyjson_is_obj(buffer)) {
        metadata(&p, buffer);
        append(&p, "\nEdit here: /buffer edit NAME (use the buffer name above). Ctrl-S saves; Esc returns to chat.\n");
        if (*field(root, "detail")) append(&p, "\n%s\n", field(root, "detail"));
        yyjson_val *text = yyjson_obj_get(root, "text");
        if (!yyjson_is_str(text)) text = yyjson_obj_get(root, "output");
        if (yyjson_is_str(text)) {
            append(&p, "\n%s", yyjson_get_str(text));
            if (yyjson_is_true(yyjson_obj_get(root, "truncated")))
                append(&p, "\n\nMore content at byte offset %llu; use /buffer read with offset.\n",
                    (unsigned long long)yyjson_get_uint(yyjson_obj_get(root, "next_offset")));
        }
    } else if (*field(root, "usage")) {
        append(&p, "%s\n%s\n\n", field(root, "usage"), field(root, "description"));
        yyjson_val *examples = yyjson_obj_get(root, "examples");
        size_t i, count; yyjson_val *example;
        yyjson_arr_foreach(examples, i, count, example) {
            const char *line = yyjson_get_str(example);
            if (line) append(&p, "%s%s\n", !strncmp(line, "dsco buffer", 11) ? "/" : "",
                !strncmp(line, "dsco buffer", 11) ? line + 5 : line);
        }
    } else {
        char *pretty = yyjson_write(doc, YYJSON_WRITE_PRETTY, NULL);
        append(&p, "%s", pretty ? pretty : "Buffer command completed.");
        free(pretty);
    }
    yyjson_doc_free(doc);
    if (p.truncated) {
        /* A bounded preview must not leave half of a UTF-8 character. */
        size_t n = strlen(out), start = n;
        while (start && ((unsigned char)out[start - 1] & 0xc0) == 0x80) start--;
        if (start) {
            start--;
            unsigned char c = (unsigned char)out[start];
            size_t width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            if (n - start < width) out[start] = '\0';
        }
        if (cap > 128) {
            n = strlen(out);
            snprintf(out + n, cap - n, "\n[Preview truncated; use dsco buffer with JSON options for full output.]\n");
        }
    }
}
