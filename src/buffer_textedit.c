/* Explicit TextEdit route for selectable/editable macOS buffer documents. */
#include "buffer_textedit.h"
#include "tools.h"
#include "../vendor/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *str(yyjson_val *o, const char *key) {
    return yyjson_get_str(yyjson_obj_get(o, key));
}
static bool fail(char *out, size_t cap, const char *code, const char *detail) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(d); yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_bool(d, o, "ok", false);
    yyjson_mut_obj_add_str(d, o, "error", code);
    yyjson_mut_obj_add_str(d, o, "detail", detail ? detail : "");
    char *s = yyjson_mut_write(d, 0, NULL);
    if (cap) snprintf(out, cap, "%s", s ? s : "{}");
    free(s); yyjson_mut_doc_free(d); return false;
}

bool buffer_textedit_open(const char *input, char *out, size_t cap) {
#ifndef __APPLE__
    (void)input;
    return fail(out, cap, "unsupported_platform", "TextEdit requires macOS; use edit/view/follow here.");
#else
    yyjson_doc *d = yyjson_read(input, strlen(input), 0), *metadata = NULL;
    yyjson_val *o = d ? yyjson_doc_get_root(d) : NULL;
    bool ok = false;
    /* Kitty lifetime controls have no meaning for a separately owned app.
     * Never silently ignore a request to hide a view or reconcile a launch. */
    const char *allowed[] = {"action", "mode", "workspace", "buffer_id", "name"};
    size_t i, n; yyjson_val *k, *v;
    yyjson_obj_foreach(o, i, n, k, v) {
        (void)v; bool found = false;
        for (size_t j = 0; j < sizeof(allowed)/sizeof(*allowed); j++)
            if (yyjson_equals_str(k, allowed[j])) found = true;
        if (!found) { fail(out, cap, "invalid_arguments", "TextEdit accepts only action=open, mode=textedit, workspace, buffer_id/name; manage its window in TextEdit."); goto done; }
    }
    if (!str(o, "action") || strcmp(str(o, "action"), "open")) {
        fail(out, cap, "invalid_arguments", "TextEdit supports open only."); goto done;
    }
    if (!str(o, "buffer_id") && !str(o, "name")) {
        fail(out, cap, "missing_buffer", "Select buffer_id or name."); goto done;
    }
    yyjson_mut_doc *r = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(r); yyjson_mut_doc_set_root(r, root);
    yyjson_mut_obj_add_str(r, root, "action", "read");
    yyjson_mut_obj_add_uint(r, root, "max_bytes", 1);
    const char *workspace = str(o, "workspace");
    yyjson_mut_obj_add_str(r, root, "workspace", workspace ? workspace : "main");
    const char *selectors[] = {"buffer_id", "name"};
    for (size_t j = 0; j < 2; j++)
        if (str(o, selectors[j])) yyjson_mut_obj_add_str(r, root, selectors[j], str(o, selectors[j]));
    char *req = yyjson_mut_write(r, 0, NULL), reply[32768];
    yyjson_mut_doc_free(r);
    if (!req) { fail(out, cap, "out_of_memory", NULL); goto done; }
    bool accessed = tools_execute_for_tier("buffer", req, tools_execution_tier(), reply, sizeof(reply));
    free(req);
    if (!accessed) { fail(out, cap, "adapter_failed", reply); goto done; }
    metadata = yyjson_read(reply, strlen(reply), 0);
    yyjson_val *m = metadata ? yyjson_doc_get_root(metadata) : NULL;
    yyjson_val *buffer = yyjson_obj_get(m, "buffer");
    const char *path = str(buffer, "content_path");
    if (!yyjson_is_true(yyjson_obj_get(m, "ok")) || !path || path[0] != '/' ||
        strlen(path) > 4096 || strchr(path, '\n') || strchr(path, '\r')) {
        fail(out, cap, "invalid_content_path", "Expected an absolute owned buffer path."); goto done;
    }
    if (yyjson_is_true(yyjson_obj_get(buffer, "closed"))) {
        fail(out, cap, "buffer_closed", "Reopen the buffer before opening its editor."); goto done;
    }
    /* Quote argv, not AppleScript source: even quotes/$()/backticks in a
     * workspace path remain inert data. Existing unsaved documents are reused. */
    char quoted[16400], command[18000], expected[4200];
    size_t used = 0; quoted[used++] = '\'';
    for (const char *p = path; *p; ++p) {
        if (*p == '\'') { memcpy(quoted + used, "'\\''", 4); used += 4; }
        else quoted[used++] = *p;
    }
    quoted[used++] = '\''; quoted[used] = 0;
    snprintf(command, sizeof(command),
        "/usr/bin/osascript - %s <<'DSCO_TEXTEDIT'\n"
        "on run argv\n tell application \"TextEdit\"\n"
        "  set d to open (POSIX file (item 1 of argv))\n"
        "  activate\n  return \"DSCO_TEXTEDIT_OPENED\" & linefeed & (path of d)\n"
        " end tell\nend run\nDSCO_TEXTEDIT\n", quoted);
    r = yyjson_mut_doc_new(NULL); root = yyjson_mut_obj(r); yyjson_mut_doc_set_root(r, root);
    yyjson_mut_obj_add_str(r, root, "command", command);
    yyjson_mut_obj_add_uint(r, root, "timeout", 20);
    req = yyjson_mut_write(r, 0, NULL); yyjson_mut_doc_free(r);
    if (!req) { fail(out, cap, "out_of_memory", NULL); goto done; }
    bool launched = tools_execute_for_tier("bash", req, tools_execution_tier(), reply, sizeof(reply));
    free(req);
    if (!launched) { fail(out, cap, "adapter_failed", reply); goto done; }
    size_t len = strlen(reply);
    while (len && (reply[len-1] == '\n' || reply[len-1] == '\r')) reply[--len] = 0;
    snprintf(expected, sizeof(expected), "DSCO_TEXTEDIT_OPENED\n%s", path);
    if (strcmp(reply, expected)) {
        fail(out, cap, "textedit_unverified", "Launch returned without the matching document path. Inspect TextEdit before retrying; an existing unsaved document must not be overwritten."); goto done;
    }
    r = yyjson_mut_doc_new(NULL); root = yyjson_mut_obj(r); yyjson_mut_doc_set_root(r, root);
    yyjson_mut_obj_add_bool(r, root, "ok", true);
    yyjson_mut_obj_add_bool(r, root, "verified", true);
    yyjson_mut_obj_add_str(r, root, "backend", "textedit");
    yyjson_mut_obj_add_str(r, root, "mode", "textedit");
    yyjson_mut_obj_add_val(r, root, "buffer", yyjson_val_mut_copy(r, buffer));
    yyjson_mut_obj_add_str(r, root, "detail", "Document observed in TextEdit. Cmd-A selects all, Cmd-C copies, Cmd-V pastes. Cmd-S saves. Existing unsaved edits are retained; disk revision does not describe unsaved text. Clipboard was not changed.");
    req = yyjson_mut_write(r, 0, NULL); yyjson_mut_doc_free(r);
    if (req && strlen(req) < cap) { memcpy(out, req, strlen(req)+1); ok = true; }
    else fail(out, cap, "output_limit", "TextEdit opened; receipt did not fit. Inspect before retrying.");
    free(req);
done:
    yyjson_doc_free(metadata); yyjson_doc_free(d); return ok;
#endif
}
