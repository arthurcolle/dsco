/* Buffer identity belongs to storage; Kitty surfaces are disposable views. */
#include "buffer_view.h"
#include "buffer_textedit.h"
#include "tools.h"
#include "crypto.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLY_CAP (1024u * 1024u)
static bool eq(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static const char *str(yyjson_val *o, const char *k) { return yyjson_get_str(yyjson_obj_get(o, k)); }
static bool flag(yyjson_val *o, const char *k) { return yyjson_get_bool(yyjson_obj_get(o, k)); }
static bool emit(yyjson_mut_doc *d, char *out, size_t cap) {
    char *s = d ? yyjson_mut_write(d, 0, NULL) : NULL;
    bool ok = s && strlen(s) < cap;
    if (ok) memcpy(out, s, strlen(s) + 1);
    else if (cap) snprintf(out, cap, "{\"ok\":false,\"error\":\"output_limit\"}");
    free(s); return ok;
}
static bool fail(char *out, size_t cap, const char *code, const char *detail) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(d); yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_bool(d, o, "ok", false);
    yyjson_mut_obj_add_str(d, o, "error", code);
    yyjson_mut_obj_add_str(d, o, "detail", detail ? detail : "");
    emit(d, out, cap); yyjson_mut_doc_free(d); return false;
}
static yyjson_mut_doc *request(const char *action, const char *workspace) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(d); yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_str(d, o, "action", action);
    yyjson_mut_obj_add_str(d, o, "workspace", workspace); return d;
}
static void copy(yyjson_mut_doc *d, yyjson_val *src, const char *key) {
    yyjson_val *v = yyjson_obj_get(src, key);
    if (v) yyjson_mut_obj_add_val(d, yyjson_mut_doc_get_root(d), key, yyjson_val_mut_copy(d, v));
}
/* Never call implementation entrypoints directly: nested calls keep the
 * current principal, capability grants, taint and governance state. */
static yyjson_doc *call(const char *tool, yyjson_mut_doc *request_doc, char *out, size_t cap) {
    char *input = yyjson_mut_write(request_doc, 0, NULL), *reply = calloc(REPLY_CAP, 1);
    yyjson_mut_doc_free(request_doc);
    if (!input || !reply) { free(input); free(reply); fail(out, cap, "out_of_memory", NULL); return NULL; }
    bool ok = tools_execute_for_tier(tool, input, tools_execution_tier(), reply, REPLY_CAP);
    free(input);
    yyjson_doc *d = ok ? yyjson_read(reply, strlen(reply), 0) : NULL;
    if (!d || !yyjson_is_obj(yyjson_doc_get_root(d)) || !flag(yyjson_doc_get_root(d), "ok")) {
        fail(out, cap, "adapter_failed", reply);
        yyjson_doc_free(d); d = NULL;
    }
    free(reply); return d;
}
static const char *validate(yyjson_val *o) {
    yyjson_doc *schema = yyjson_read(BUFFER_VIEW_SCHEMA, strlen(BUFFER_VIEW_SCHEMA), 0);
    yyjson_val *props = yyjson_obj_get(yyjson_doc_get_root(schema), "properties");
    const char *error = NULL;
    if (!yyjson_is_obj(o) || !str(o, "action")) error = "object with action required";
    size_t i, n; yyjson_val *key, *value;
    if (!error) yyjson_obj_foreach(o, i, n, key, value) {
        const char *name = yyjson_get_str(key);
        yyjson_val *rule = yyjson_obj_get(props, name);
        if (strlen(name) != yyjson_get_len(key) || !rule) { error = "unknown field"; break; }
        size_t j, count; yyjson_val *earlier, *unused;
        yyjson_obj_foreach(o, j, count, earlier, unused) {
            (void)unused;
            if (i == j) break;
            if (yyjson_equals_str(earlier, name)) { error = "duplicate field"; break; }
        }
        if (error) break;
        const char *type = str(rule, "type");
        if (eq(type, "string")) {
            const char *s = yyjson_get_str(value);
            if (!s || !*s || strlen(s) != yyjson_get_len(value)) { error = "nonempty string without NUL required"; break; }
            yyjson_val *limit = yyjson_obj_get(rule, "maxLength");
            if (limit && strlen(s) > yyjson_get_uint(limit)) { error = "string limit exceeded"; break; }
            yyjson_val *choices = yyjson_obj_get(rule, "enum");
            if (choices) {
                bool found = false; size_t k, total; yyjson_val *choice;
                yyjson_arr_foreach(choices, k, total, choice) if (yyjson_equals_str(choice, s)) found = true;
                if (!found) { error = "unsupported enum value"; break; }
            }
        } else if (eq(type, "boolean") && !yyjson_is_bool(value)) { error = "boolean required"; break; }
        else if (eq(type, "integer") && (!yyjson_is_int(value) ||
            (yyjson_is_uint(value) && yyjson_get_uint(value) > 1000) ||
            yyjson_get_sint(value) < -1000 || yyjson_get_sint(value) > 1000)) {
            error = "integer must be between -1000 and 1000"; break;
        }
    }
    yyjson_doc_free(schema); return error;
}
static bool tag(yyjson_val *view, char id[37], const char **mode) {
    const char *run = str(view, "run_id");
    if (!run || strlen(run) < 44 || strncmp(run, "buffer:", 7) || run[43] != ':') return false;
    *mode = run + 44;
    if (!(eq(*mode, "edit") || eq(*mode, "view") || eq(*mode, "follow"))) return false;
    memcpy(id, run + 7, 36); id[36] = 0; return true;
}
static yyjson_val *find(yyjson_val *views, const char *surface_id) {
    size_t i, n; yyjson_val *v;
    yyjson_arr_foreach(views, i, n, v) if (eq(str(v, "surface_id"), surface_id)) return v;
    return NULL;
}
static bool finish(yyjson_val *inventory, yyjson_val *buffer, const char *action,
                   const char *workspace, const char *only, bool reused, char *out, size_t cap) {
    yyjson_mut_doc *d = request(action, workspace); yyjson_mut_val *o = yyjson_mut_doc_get_root(d);
    yyjson_mut_obj_add_bool(d, o, "ok", true);
    yyjson_mut_obj_add_str(d, o, "backend", "kitty");
    bool verified = flag(inventory, "verified"), open_observed = false;
    yyjson_mut_obj_add_bool(d, o, "reused", reused);
    if (buffer) yyjson_mut_obj_add_val(d, o, "buffer", yyjson_val_mut_copy(d, buffer));
    if (str(inventory, "detail")) copy(d, inventory, "detail");
    yyjson_mut_val *arr = yyjson_mut_arr(d); yyjson_mut_obj_add_val(d, o, "views", arr);
    size_t i, n; yyjson_val *v;
    yyjson_arr_foreach(yyjson_obj_get(inventory, "surfaces"), i, n, v) {
        char id[37]; const char *mode;
        if (!tag(v, id, &mode) || (buffer && !eq(id, str(buffer, "buffer_id"))) ||
            (only && !eq(only, str(v, "surface_id")))) continue;
        yyjson_mut_val *entry = yyjson_val_mut_copy(d, v);
        yyjson_mut_obj_add_strcpy(d, entry, "buffer_id", id);
        yyjson_mut_obj_add_str(d, entry, "mode", mode); yyjson_mut_arr_add_val(arr, entry);
        if (flag(v, "exists") && !flag(v, "closed") && !flag(v, "pending")) open_observed = true;
    }
    if (eq(action, "open") && !open_observed) {
        verified = false;
        if (!str(inventory, "detail")) yyjson_mut_obj_add_str(d, o, "detail",
            "View is closed, pending or unobserved; this receipt does not launch another view. Inspect before retrying.");
    }
    yyjson_mut_obj_add_bool(d, o, "verified", verified);
    bool ok = emit(d, out, cap); yyjson_mut_doc_free(d); return ok;
}

bool tool_buffer_view(const char *input, char *out, size_t cap) {
    if (!input || strlen(input) > 65536) return fail(out, cap, "invalid_arguments", "request exceeds 64 KiB");
    yyjson_doc *d = yyjson_read(input, strlen(input), 0), *state = NULL, *metadata = NULL, *changed = NULL;
    yyjson_val *o = d ? yyjson_doc_get_root(d) : NULL;
    const char *error = validate(o);
    bool ok = false;
    if (error) { fail(out, cap, "invalid_arguments", error); goto done; }
    const char *action = str(o, "action"), *workspace = str(o, "workspace");
    if (!workspace) workspace = "main";
    bool opening = eq(action, "open"), listing = eq(action, "list");
    if (!opening && (yyjson_obj_get(o, "new_view") || str(o, "request_id") || str(o, "source_surface_id") || str(o, "type") || str(o, "location"))) {
        fail(out, cap, "invalid_arguments", "launch options require action open"); goto done;
    }
    if (opening && str(o, "surface_id")) { fail(out, cap, "invalid_arguments", "open selects buffer_id/name, not surface_id"); goto done; }
    if (eq(str(o, "mode"), "textedit")) {
        ok = buffer_textedit_open(input, out, cap); goto done;
    }
    state = call("surface", request("status", workspace), out, cap);
    if (!state) goto done;
    yyjson_val *inventory = yyjson_doc_get_root(state), *views = yyjson_obj_get(inventory, "surfaces"), *selected = NULL;
    char selected_id[37] = "";
    if (str(o, "surface_id")) {
        selected = find(views, str(o, "surface_id")); const char *mode;
        if (!selected || !tag(selected, selected_id, &mode)) {
            fail(out, cap, "unknown_view", "surface_id must identify a registered buffer view"); goto done;
        }
    }
    if (str(o, "buffer_id") || str(o, "name") || selected_id[0]) {
        yyjson_mut_doc *r = request("inspect", workspace);
        copy(r, o, "name"); copy(r, o, "buffer_id");
        if (!str(o, "buffer_id") && !str(o, "name")) yyjson_mut_obj_add_str(r, yyjson_mut_doc_get_root(r), "buffer_id", selected_id);
        metadata = call("buffer", r, out, cap);
        if (!metadata) goto done;
    }
    yyjson_val *buffer = metadata ? yyjson_obj_get(yyjson_doc_get_root(metadata), "buffer") : NULL;
    const char *id = str(buffer, "buffer_id");
    if ((metadata && (!id || strlen(id) != 36 || !str(buffer, "content_path"))) ||
        (selected_id[0] && !eq(selected_id, id))) {
        fail(out, cap, "identity_mismatch", "buffer metadata and selected view do not agree"); goto done;
    }
    if (listing) { ok = finish(inventory, buffer, action, workspace, str(o, "surface_id"), false, out, cap); goto done; }
    if (!buffer) { fail(out, cap, "missing_buffer", "select a buffer by name or buffer_id"); goto done; }
    if (opening) {
        if (flag(buffer, "closed")) { fail(out, cap, "buffer_closed", "reopen the buffer before opening a view"); goto done; }
        const char *mode = str(o, "mode");
        if (!mode) mode = eq(str(buffer, "kind"), "log") ? "follow" : "edit";
        char run[96]; snprintf(run, sizeof(run), "buffer:%s:%s", id, mode);
        const char *request_id = str(o, "request_id");
        size_t i, n; yyjson_val *v;
        yyjson_arr_foreach(views, i, n, v) {
            if (request_id && eq(request_id, str(v, "request_id"))) {
                if (!eq(run, str(v, "run_id"))) { fail(out, cap, "request_conflict", "request_id belongs to a different buffer or mode"); goto done; }
                selected = v; break;
            }
            if (!request_id && !flag(o, "new_view") && eq(run, str(v, "run_id")) && !flag(v, "closed") &&
                (flag(v, "exists") || flag(v, "pending"))) selected = v;
        }
        bool reused = selected != NULL;
        if (!selected) {
            const char *path = str(buffer, "content_path");
            if (path[0] != '/' || strchr(path, '\n') || strchr(path, '\r')) { fail(out, cap, "invalid_content_path", "owned buffer path is not a launchable absolute path"); goto done; }
            bool online = flag(inventory, "verified");
            yyjson_mut_doc *r = request(online ? "create" : "start", workspace);
            yyjson_mut_val *root = yyjson_mut_doc_get_root(r), *args = yyjson_mut_arr(r);
            yyjson_mut_obj_add_str(r, root, "run_id", run);
            char retry[80];
            snprintf(retry, sizeof(retry), "buf-%s-%s-%zu", id, mode, yyjson_arr_size(views));
            if (!request_id && flag(o, "new_view")) {
                unsigned char bytes[16];
                if (!crypto_random_bytes(bytes, sizeof(bytes))) {
                    yyjson_mut_doc_free(r);
                    fail(out, cap, "random_unavailable", "could not create a unique view request"); goto done;
                }
                memcpy(retry, "new-", 4);
                for (size_t k = 0; k < sizeof(bytes); k++) snprintf(retry + 4 + k * 2, 3, "%02x", bytes[k]);
            }
            yyjson_mut_obj_add_str(r, root, "request_id", request_id ? request_id : retry);
            yyjson_mut_obj_add_str(r, root, "command", "/usr/bin/env");
            yyjson_mut_obj_add_val(r, root, "args", args);
            if (eq(mode, "edit")) {
                const char *argv[] = {"VIMINIT=", "EXINIT=", "/usr/bin/vi", "-u", "NONE", "-i", "NONE", "-n", "--cmd", "set nomodeline", "--", path};
                for (size_t a = 0; a < sizeof(argv)/sizeof(*argv); a++) yyjson_mut_arr_add_str(r, args, argv[a]);
            } else {
                const char *argv[] = {"LESSSECURE=1", "LESS=", "LESSOPEN=", "LESSCLOSE=", "LESSHISTFILE=/dev/null", "/usr/bin/less", "--follow-name"};
                for (size_t a = 0; a < sizeof(argv)/sizeof(*argv); a++) yyjson_mut_arr_add_str(r, args, argv[a]);
                if (eq(mode, "follow")) yyjson_mut_arr_add_str(r, args, "+F");
                yyjson_mut_arr_add_str(r, args, "--"); yyjson_mut_arr_add_str(r, args, path);
            }
            yyjson_mut_obj_add_str(r, root, "title", str(buffer, "name"));
            copy(r, o, "source_surface_id"); copy(r, o, "type"); copy(r, o, "location");
            yyjson_mut_obj_add_bool(r, root, "visible", !yyjson_obj_get(o, "visible") || flag(o, "visible"));
            changed = call("surface", r, out, cap);
            if (!changed) goto done;
            inventory = yyjson_doc_get_root(changed);
            yyjson_arr_foreach(yyjson_obj_get(inventory, "surfaces"), i, n, v)
                if (eq(run, str(v, "run_id")) && eq(request_id ? request_id : retry, str(v, "request_id"))) { selected = v; break; }
            if (!selected) { fail(out, cap, "launch_unresolved", "workspace changed during open; inspect views before retrying"); goto done; }
        }
        if (flag(o, "focus") && flag(selected, "exists") && !flag(selected, "closed")) {
            yyjson_mut_doc *r = request("focus", workspace);
            yyjson_mut_obj_add_strcpy(r, yyjson_mut_doc_get_root(r), "surface_id", str(selected, "surface_id"));
            yyjson_doc *focused = call("surface", r, out, cap);
            if (!focused) goto done;
            yyjson_doc_free(changed); changed = focused;
            inventory = yyjson_doc_get_root(changed);
            selected = yyjson_arr_get_first(yyjson_obj_get(inventory, "surfaces"));
        }
        ok = finish(inventory, buffer, action, workspace, str(selected, "surface_id"), reused, out, cap);
        goto done;
    }
    if (!selected) {
        size_t i, n, matches = 0; yyjson_val *v;
        yyjson_arr_foreach(views, i, n, v) {
            char buffer_id[37]; const char *mode;
            if (tag(v, buffer_id, &mode) && eq(id, buffer_id) && flag(v, "exists") && !flag(v, "closed") &&
                (!str(o, "mode") || eq(str(o, "mode"), mode))) { selected = v; matches++; }
        }
        if (matches != 1) { fail(out, cap, matches ? "ambiguous_view" : "view_not_open", "select one surface_id from buffer_view list"); goto done; }
    }
    yyjson_mut_doc *r = request(action, workspace);
    yyjson_mut_obj_add_str(r, yyjson_mut_doc_get_root(r), "surface_id", str(selected, "surface_id"));
    copy(r, o, "axis"); copy(r, o, "increment"); copy(r, o, "layout");
    changed = call("surface", r, out, cap);
    if (changed) ok = finish(yyjson_doc_get_root(changed), buffer, action, workspace, str(selected, "surface_id"), false, out, cap);
done:
    yyjson_doc_free(changed); yyjson_doc_free(metadata); yyjson_doc_free(state); yyjson_doc_free(d);
    return ok;
}
