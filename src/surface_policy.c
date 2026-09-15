#include "surface_policy.h"
#include "capability.h"
#include "json_util.h"
#include "buffer_store.h"
#include "native_windows.h"
#include <stdlib.h>
#include <string.h>

static bool eq(const char *a, const char *b) { return a && !strcmp(a, b); }
static _Thread_local bool owned_read;
bool surface_policy_owned_read_scope(bool enabled) {
    bool previous = owned_read;
    owned_read = enabled;
    return previous;
}
bool surface_policy_caps(const char *name, const char *json, unsigned *caps) {
    if (!name || !caps) return false;
    bool kitty = eq(name, "kitty_remote"), kitten = eq(name, "kitten");
    bool surface = eq(name, "surface"), pty = eq(name, "pty_session");
    bool desktop = eq(name, "desktop"), computer = eq(name, "computer");
    bool browser = eq(name, "browser_session");
    bool buffer = eq(name, "buffer"), buffer_view = eq(name, "buffer_view");
    bool native_window = eq(name, "native_window");
    if (!(kitty || kitten || surface || pty || desktop || computer || browser || buffer || buffer_view || native_window)) return false;
    char *action = json_get_str(json ? json : "{}", kitty || kitten ? "command" : "action");
    unsigned c = CAP_FS_READ;
    if (native_window) {
        if (!(eq(action, "list") || eq(action, "events"))) c |= CAP_FS_WRITE;
        /* Retained sensitive snapshots cannot be re-read under a weaker grant.
         * Initial attachments also pass the nested buffer read's own gate. */
        if (native_windows_sensitive()) c |= CAP_SECRETS;
    } else if (buffer) {
        if (!(eq(action, "list") || eq(action, "inspect") || eq(action, "read"))) c |= CAP_FS_WRITE;
        if (eq(action, "read") || eq(action, "fork") || eq(action, "save")) c |= CAP_UNTRUSTED_IN;
        char *source = json_get_str(json ? json : "{}", "source_path");
        if (eq(action, "create") && source) c |= CAP_UNTRUSTED_IN;
        free(source);
        if (buffer_store_sensitive(json ? json : "{}")) c |= CAP_SECRETS;
    } else if (buffer_view) {
        if (!eq(action, "list")) c |= CAP_EXEC | CAP_FS_WRITE;
        if (eq(action, "open")) c |= CAP_NET;
        /* View-only fields are not valid store arguments. Resolve persisted
         * sensitivity using the same minimal selector as the nested inspect.
         * Surface-only selection is checked by that governed nested call. */
        char *id = json_get_str(json ? json : "{}", "buffer_id");
        char *name_arg = json_get_str(json ? json : "{}", "name");
        char *workspace = json_get_str(json ? json : "{}", "workspace");
        if (id || name_arg) {
            jbuf_t selector; jbuf_init(&selector, 256);
            jbuf_append(&selector, "{\"action\":\"inspect\",\"workspace\":");
            jbuf_append_json_str(&selector, workspace ? workspace : "main");
            if (id) { jbuf_append(&selector, ",\"buffer_id\":"); jbuf_append_json_str(&selector, id); }
            if (name_arg) { jbuf_append(&selector, ",\"name\":"); jbuf_append_json_str(&selector, name_arg); }
            jbuf_append_char(&selector, '}');
            if (buffer_store_sensitive(selector.data)) c |= CAP_SECRETS;
            jbuf_free(&selector);
        }
        free(id); free(name_arg); free(workspace);
    } else if (kitty) {
        c |= CAP_EXEC;
        if (eq(action, "get-text")) {
            c |= CAP_UNTRUSTED_IN;
            if (!owned_read) c |= CAP_SECRETS;
        }
        if (eq(action, "launch") || eq(action, "run") || eq(action, "action") ||
            eq(action, "kitten") || eq(action, "new-window") ||
            eq(action, "send-text") || eq(action, "send-key")) c |= CAP_NET | CAP_FS_WRITE;
        char *to = json_get_str(json ? json : "{}", "to");
        if (!to) { const char *env = getenv("KITTY_LISTEN_ON"); if (env) to = strdup(env); }
        if (to && strncmp(to, "unix:", 5)) c |= CAP_NET;
        free(to);
    } else if (kitten) {
        c |= CAP_EXEC;
        if (eq(action, "clipboard")) c |= CAP_SECRETS | CAP_UNTRUSTED_IN;
        if (eq(action, "ssh") || eq(action, "transfer") || eq(action, "update-self"))
            c |= CAP_NET | CAP_UNTRUSTED_IN | CAP_FS_WRITE;
        if (eq(action, "run-shell") || eq(action, "edit-in-kitty") || eq(action, "panel") ||
            eq(action, "quick-access-terminal") || eq(action, "kitten"))
            c |= CAP_NET | CAP_FS_WRITE;
    } else if (computer) {
        bool observation = eq(action, "screenshot") || eq(action, "cursor_position") || eq(action, "wait");
        if (!observation) c |= CAP_EXEC | CAP_NET | CAP_FS_WRITE;
        if (eq(action, "screenshot") || (!eq(action, "cursor_position") &&
            json_get_bool(json ? json : "{}", "screenshot", true)))
            c |= CAP_SECRETS | CAP_UNTRUSTED_IN;
    } else if (desktop) {
        if (eq(action, "snapshot")) c |= CAP_UNTRUSTED_IN;
        else if (!(eq(action, "status") || eq(action, "list") || eq(action, "inspect"))) c |= CAP_EXEC;
    } else if (pty) {
        if (eq(action, "read") || eq(action, "wait")) c |= CAP_UNTRUSTED_IN;
        else if (!(eq(action, "status") || eq(action, "list"))) c |= CAP_EXEC;
        if (eq(action, "spawn") || eq(action, "write")) c |= CAP_NET | CAP_FS_WRITE;
    } else if (browser) {
        if (eq(action, "snapshot") || eq(action, "screenshot") || eq(action, "tabs")) c |= CAP_UNTRUSTED_IN;
        else if (!eq(action, "status")) c |= CAP_EXEC;
        if (eq(action, "launch") || eq(action, "navigate") || eq(action, "click") ||
            eq(action, "type") || eq(action, "evaluate")) c |= CAP_NET | CAP_FS_WRITE | CAP_UNTRUSTED_IN;
        if (eq(action, "close")) c |= CAP_FS_WRITE;
    } else if (surface) {
        if (eq(action, "read")) c |= CAP_EXEC | CAP_UNTRUSTED_IN;
        else if (!(eq(action, "status") || eq(action, "list") || eq(action, "inspect"))) c |= CAP_EXEC | CAP_FS_WRITE;
        if (eq(action, "start") || eq(action, "create") || eq(action, "send_text") || eq(action, "send_key"))
            c |= CAP_NET | CAP_FS_WRITE;
    }
    free(action); *caps = c; return true;
}
