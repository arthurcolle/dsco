/* Expose O_NOFOLLOW and flock under the build's POSIX feature flags. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "surface_registry.h"
#include "surface_policy.h"
#include "json_util.h"
#include "process_capture.h"
#include "../vendor/yyjson.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Nested adapters retain the calling principal's tier and normal gate. */
extern bool tools_execute_for_tier(const char *, const char *, const char *, char *, size_t);
extern const char *tools_execution_tier(void);
#define VIEW_CAP 256
#define OBS_CAP 512
#define REMOTE_CAP (1024 * 1024)
typedef struct {
    char id[80], request[80], run[128];
    int window;
    bool closed, pending, startup;
} view_t;
typedef struct {
    int id, tab, os, cols, rows;
    bool focused;
    char workspace[64], surface[80], layout[24];
} observation_t;
typedef struct {
    char workspace[64], dir[PATH_MAX], registry[PATH_MAX], socket[PATH_MAX];
    view_t views[VIEW_CAP];
    int count, obs_count, parse_os, parse_tab;
    char parse_layout[24];
    bool parse_error;
    double observed_at;
    observation_t obs[OBS_CAP];
} workspace_t;
static pthread_mutex_t surface_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool same(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static bool safe_name(const char *s, size_t cap) {
    if (!s || !*s || strlen(s) >= cap) return false;
    for (; *s; s++) if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-')) return false;
    return true;
}
static void copy_field(const char *json, const char *key, char *out, size_t cap) {
    char *s = json_get_str(json, key); snprintf(out, cap, "%s", s ? s : ""); free(s);
}
static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000 + ts.tv_nsec / 1e6;
}
static void token(char *out, size_t cap) {
    unsigned char random[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, random, sizeof(random)) : -1;
    if (fd >= 0) close(fd);
    if (n != sizeof(random)) { out[0] = 0; return; }
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", random[i]);
    snprintf(out, cap, "view-%s", hex);
}
static bool fail(char *result, size_t cap, const char *reason) {
    jbuf_t b; jbuf_init(&b, 128);
    jbuf_append(&b, "{\"ok\":false,\"verified\":false,\"error\":");
    jbuf_append_json_str(&b, reason); jbuf_append(&b, "}");
    if (b.len < cap) memcpy(result, b.data, b.len + 1);
    else if (cap) snprintf(result, cap, "{}");
    jbuf_free(&b); return false;
}
static bool sync_directory(const char *path);
static bool sync_parent(const char *path) {
    char parent[PATH_MAX];
    if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent)) return false;
    size_t len = strlen(parent);
    while (len > 1 && parent[len - 1] == '/') parent[--len] = 0;
    char *slash = strrchr(parent, '/');
    if (!slash) return sync_directory(".");
    if (slash == parent) slash[1] = 0; else *slash = 0;
    return sync_directory(parent);
}
static bool directory_private(const char *path, bool create) {
    bool created = false;
    if (create) {
        if (!mkdir(path, 0700)) created = true;
        else if (errno != EEXIST) return false;
    }
    struct stat st;
    return !lstat(path, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
           !(st.st_mode & 0077) && (!created || sync_parent(path));
}
static bool paths(workspace_t *w, const char *name, bool create) {
    snprintf(w->workspace, sizeof(w->workspace), "%s", name);
    const char *base = getenv("DSCO_SURFACE_DIR");
    char parent[PATH_MAX];
    if (!base || !*base) {
        const char *home = getenv("HOME");
        if (!home || snprintf(parent, sizeof(parent), "%s/.dsco", home) >= (int)sizeof(parent)) return false;
        struct stat st;
        if (create) {
            if (!mkdir(parent, 0700)) { if (!sync_parent(parent)) return false; }
            else if (errno != EEXIST) return false;
        }
        if (lstat(parent, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return false;
        if (snprintf(parent, sizeof(parent), "%s/.dsco/surfaces", home) >= (int)sizeof(parent)) return false;
        base = parent;
    }
    if (!directory_private(base, create)) return false;
    if (snprintf(w->dir, sizeof(w->dir), "%s/%s", base, name) >= (int)sizeof(w->dir) || !directory_private(w->dir, create)) return false;
    if (snprintf(w->socket, sizeof(w->socket), "unix:%s/kitty.sock", w->dir) >= 104) return false;
    return snprintf(w->registry, sizeof(w->registry), "%s/views.json", w->dir) < (int)sizeof(w->registry);
}
static bool string_field(yyjson_val *obj, const char *key, char *out, size_t cap) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (!yyjson_is_str(v) || yyjson_get_len(v) >= cap ||
        strlen(yyjson_get_str(v)) != yyjson_get_len(v)) return false;
    memcpy(out, yyjson_get_str(v), yyjson_get_len(v) + 1); return true;
}
static bool read_all(int fd, char *data, size_t size) {
    size_t used = 0;
    while (used < size) {
        ssize_t n = read(fd, data + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        used += (size_t)n;
    }
    return true;
}
static bool write_all(int fd, const char *data, size_t size) {
    size_t used = 0;
    while (used < size) {
        ssize_t n = write(fd, data + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        used += (size_t)n;
    }
    return true;
}
static bool sync_directory(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    bool ok = !fstat(fd, &st) && S_ISDIR(st.st_mode) && !fsync(fd);
    close(fd); return ok;
}
static bool load_registry(workspace_t *w) {
    int fd = open(w->registry, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 0077) || st.st_size <= 0 || st.st_size > 1024 * 1024) { close(fd); return false; }
    char *data = calloc((size_t)st.st_size + 1, 1);
    bool ok = data && read_all(fd, data, (size_t)st.st_size);
    close(fd);
    yyjson_doc *doc = ok ? yyjson_read(data, (size_t)st.st_size, 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *version = yyjson_obj_get(root, "version"), *views = yyjson_obj_get(root, "views");
    ok = yyjson_is_obj(root) && yyjson_obj_size(root) == 2 && yyjson_is_int(version) &&
         yyjson_get_sint(version) == 1 && yyjson_is_arr(views) && yyjson_arr_size(views) <= VIEW_CAP;
    size_t index, count; yyjson_val *record;
    if (ok) yyjson_arr_foreach(views, index, count, record) {
        view_t v = {0};
        yyjson_val *window = yyjson_obj_get(record, "window_id"), *closed = yyjson_obj_get(record, "closed"),
                   *pending = yyjson_obj_get(record, "pending"), *startup = yyjson_obj_get(record, "startup");
        ok = yyjson_is_obj(record) && yyjson_obj_size(record) == (startup ? 7 : 6) &&
             string_field(record, "id", v.id, sizeof(v.id)) && safe_name(v.id, sizeof(v.id)) &&
             string_field(record, "request_id", v.request, sizeof(v.request)) &&
             (!*v.request || safe_name(v.request, sizeof(v.request))) &&
             string_field(record, "run_id", v.run, sizeof(v.run)) &&
             yyjson_is_int(window) && yyjson_get_sint(window) >= 0 && yyjson_get_sint(window) <= INT_MAX &&
             yyjson_is_bool(closed) && yyjson_is_bool(pending) && (!startup || yyjson_is_bool(startup));
        if (!ok) break;
        v.window = (int)yyjson_get_sint(window); v.closed = yyjson_get_bool(closed);
        v.pending = yyjson_get_bool(pending); v.startup = yyjson_get_bool(startup);
        if ((v.closed && v.pending) || (!v.closed && !v.pending && !v.window)) { ok = false; break; }
        for (int i = 0; i < w->count; i++) {
            view_t *prev = &w->views[i];
            if (same(prev->id, v.id) || (*v.request && same(prev->request, v.request)) ||
                (!v.closed && !prev->closed && v.window && v.window == prev->window)) ok = false;
        }
        if (!ok) break;
        w->views[w->count++] = v;
    }
    if (doc) yyjson_doc_free(doc);
    if (!ok) w->count = 0;
    free(data); return ok;
}
static bool save_registry(workspace_t *w) {
    jbuf_t b; jbuf_init(&b, 2048); jbuf_append(&b, "{\"version\":1,\"views\":[");
    for (int i = 0; i < w->count; i++) {
        view_t *v = &w->views[i];
        if (i) jbuf_append_char(&b, ',');
        jbuf_append(&b, "{\"id\":"); jbuf_append_json_str(&b, v->id);
        jbuf_append(&b, ",\"request_id\":"); jbuf_append_json_str(&b, v->request);
        jbuf_append(&b, ",\"run_id\":"); jbuf_append_json_str(&b, v->run);
        jbuf_appendf(&b, ",\"window_id\":%d,\"closed\":%s,\"pending\":%s,\"startup\":%s}", v->window, v->closed ? "true" : "false", v->pending ? "true" : "false", v->startup ? "true" : "false");
    }
    jbuf_append(&b, "]}");
    char temp[PATH_MAX]; snprintf(temp, sizeof(temp), "%s/write-XXXXXX", w->dir);
    int fd = mkstemp(temp);
    bool ok = fd >= 0;
    if (ok) { ok = write_all(fd, b.data, b.len) && fsync(fd) == 0; close(fd); }
    if (ok) ok = rename(temp, w->registry) == 0;
    if (ok) ok = sync_directory(w->dir);
    if (!ok && fd >= 0) unlink(temp);
    jbuf_free(&b); return ok;
}
static bool remote(workspace_t *w, const char *command, jbuf_t *args, char *out, size_t cap) {
    jbuf_t input; jbuf_init(&input, 256);
    jbuf_append(&input, "{\"command\":"); jbuf_append_json_str(&input, command);
    jbuf_append(&input, ",\"to\":"); jbuf_append_json_str(&input, w->socket);
    jbuf_append(&input, ",\"timeout_seconds\":2,\"args\":");
    jbuf_append(&input, args ? args->data : "[]"); jbuf_append(&input, "}");
    bool ok = tools_execute_for_tier("kitty_remote", input.data, tools_execution_tier(), out, cap);
    jbuf_free(&input); return ok;
}
static void arg(jbuf_t *b, const char *value) {
    if (b->len > 1) jbuf_append_char(b, ',');
    jbuf_append_json_str(b, value);
}
static void observe_window(const char *json, void *ctx) {
    workspace_t *w = ctx;
    if (w->obs_count >= OBS_CAP) { w->parse_error = true; return; }
    observation_t *o = &w->obs[w->obs_count++];
    o->id = json_get_int(json, "id", 0); o->tab = w->parse_tab; o->os = w->parse_os;
    if (o->id <= 0 || o->tab <= 0 || o->os <= 0) w->parse_error = true;
    for (int i = 0; i < w->obs_count - 1; i++) if (w->obs[i].id == o->id) w->parse_error = true;
    o->cols = json_get_int(json, "columns", 0); o->rows = json_get_int(json, "lines", 0);
    o->focused = json_get_bool(json, "is_focused", false);
    snprintf(o->layout, sizeof(o->layout), "%s", w->parse_layout);
    char *vars = json_get_raw(json, "user_vars");
    if (vars) {
        copy_field(vars, "dsco_workspace", o->workspace, sizeof(o->workspace));
        copy_field(vars, "dsco_surface", o->surface, sizeof(o->surface));
        free(vars);
    }
}
static void observe_tab(const char *json, void *ctx) {
    workspace_t *w = ctx; w->parse_tab = json_get_int(json, "id", 0);
    copy_field(json, "layout", w->parse_layout, sizeof(w->parse_layout));
    json_array_foreach(json, "windows", observe_window, w);
}
static void observe_os(const char *json, void *ctx) {
    workspace_t *w = ctx; w->parse_os = json_get_int(json, "id", 0);
    json_array_foreach(json, "tabs", observe_tab, w);
}
static bool valid_inventory(const char *output) {
    yyjson_doc *doc = yyjson_read(output, strlen(output), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool ok = yyjson_is_arr(root);
    size_t i, n, j, m, k, total, windows = 0;
    yyjson_val *os, *tab, *window;
    if (ok) yyjson_arr_foreach(root, i, n, os) {
        yyjson_val *os_id = yyjson_obj_get(os, "id"), *tabs = yyjson_obj_get(os, "tabs");
        if (!yyjson_is_obj(os) || !yyjson_is_int(os_id) || yyjson_get_sint(os_id) <= 0 ||
            yyjson_get_sint(os_id) > INT_MAX || !yyjson_is_arr(tabs)) { ok = false; break; }
        yyjson_arr_foreach(tabs, j, m, tab) {
            yyjson_val *tab_id = yyjson_obj_get(tab, "id"), *layout = yyjson_obj_get(tab, "layout"),
                       *list = yyjson_obj_get(tab, "windows");
            if (!yyjson_is_obj(tab) || !yyjson_is_int(tab_id) || yyjson_get_sint(tab_id) <= 0 ||
                yyjson_get_sint(tab_id) > INT_MAX || !yyjson_is_str(layout) ||
                yyjson_get_len(layout) >= 24 || !yyjson_is_arr(list)) { ok = false; break; }
            yyjson_arr_foreach(list, k, total, window) {
                yyjson_val *id = yyjson_obj_get(window, "id");
                if (!yyjson_is_obj(window) || !yyjson_is_int(id) || yyjson_get_sint(id) <= 0 ||
                    yyjson_get_sint(id) > INT_MAX || ++windows > OBS_CAP) { ok = false; break; }
            }
            if (!ok) break;
        }
        if (!ok) break;
    }
    if (doc) yyjson_doc_free(doc);
    return ok;
}
static bool observe(workspace_t *w, char *error, size_t error_cap) {
    w->obs_count = 0; w->observed_at = 0; w->parse_error = false;
    char *response = malloc(REMOTE_CAP);
    if (!response) { snprintf(error, error_cap, "observation allocation failed"); return false; }
    bool ok = remote(w, "ls", NULL, response, REMOTE_CAP);
    char *output = ok ? json_get_str(response, "output") : NULL;
    if (!ok) snprintf(error, error_cap, "%s", response);
    else snprintf(error, error_cap, "Kitty inventory is malformed, oversized or truncated");
    ok = output && valid_inventory(output) && !json_get_bool(response, "truncated", true);
    if (ok) {
        jbuf_t wrap; jbuf_init(&wrap, strlen(output) + 16);
        jbuf_append(&wrap, "{\"items\":"); jbuf_append(&wrap, output); jbuf_append(&wrap, "}");
        json_array_foreach(wrap.data, "items", observe_os, w); jbuf_free(&wrap);
        ok = !w->parse_error;
        if (ok) w->observed_at = now_ms();
    }
    if (!ok) w->obs_count = 0;
    free(output); free(response); return ok;
}
static observation_t *observed(workspace_t *w, view_t *v) {
    observation_t *found = NULL;
    for (int i = 0; i < w->obs_count; i++) {
        observation_t *o = &w->obs[i];
        if (same(o->workspace, w->workspace) && same(o->surface, v->id) && (!v->window || o->id == v->window)) {
            if (found) return NULL;
            found = o;
        }
    }
    return found;
}
static view_t *find_view(workspace_t *w, const char *id) {
    for (int i = 0; i < w->count; i++) if (same(w->views[i].id, id)) return &w->views[i];
    return NULL;
}
static void reconcile(workspace_t *w) {
    for (int i = 0; i < w->count; i++) {
        view_t *v = &w->views[i];
        if (v->closed) continue;
        observation_t *o = observed(w, v);
        if (o) { v->window = o->id; v->pending = false; }
        else if (!v->pending) v->closed = true;
    }
}
static bool render(workspace_t *w, view_t *only, const char *action, bool verified,
                   const char *detail, char *result, size_t cap) {
    jbuf_t b; jbuf_init(&b, 2048);
    jbuf_appendf(&b, "{\"ok\":true,\"verified\":%s,\"action\":", verified ? "true" : "false");
    jbuf_append_json_str(&b, action); jbuf_append(&b, ",\"workspace\":"); jbuf_append_json_str(&b, w->workspace);
    jbuf_append(&b, ",\"to\":"); jbuf_append_json_str(&b, w->socket);
    jbuf_appendf(&b, ",\"observed_at_ms\":%.3f,\"surfaces\":[", w->observed_at);
    int n = 0;
    for (int i = 0; i < w->count; i++) {
        view_t *v = &w->views[i]; if (only && only != v) continue;
        observation_t *o = observed(w, v);
        if (n++) jbuf_append_char(&b, ',');
        jbuf_append(&b, "{\"surface_id\":"); jbuf_append_json_str(&b, v->id);
        jbuf_append(&b, ",\"request_id\":"); jbuf_append_json_str(&b, v->request);
        jbuf_append(&b, ",\"run_id\":"); jbuf_append_json_str(&b, v->run);
        jbuf_appendf(&b, ",\"window_id\":%d,\"closed\":%s,\"pending\":%s,\"exists\":%s",
                     v->window, v->closed ? "true" : "false", v->pending ? "true" : "false", o ? "true" : "false");
        if (o) {
            jbuf_appendf(&b, ",\"tab_id\":%d,\"os_window_id\":%d,\"columns\":%d,\"rows\":%d,\"focused\":%s,\"layout\":",
                         o->tab, o->os, o->cols, o->rows, o->focused ? "true" : "false");
            jbuf_append_json_str(&b, o->layout);
        }
        jbuf_append_char(&b, '}');
    }
    jbuf_append(&b, "]");
    if (detail) { jbuf_append(&b, ",\"detail\":"); jbuf_append_json_str(&b, detail); }
    jbuf_append(&b, "}");
    bool ok = b.len < cap;
    if (ok) memcpy(result, b.data, b.len + 1);
    jbuf_free(&b);
    return ok ? true : fail(result, cap, "result too large; inspect one surface");
}
typedef struct { jbuf_t *args; int count; bool bad; } extra_args_t;
static void extra_arg(const char *json, void *ctx) {
    extra_args_t *a = ctx;
    if (*json != '"' || a->count++ >= 24) { a->bad = true; return; }
    const char *end = json + 1;
    while (*end && *end != '"') { if (*end == '\\' && end[1]) end++; end++; }
    if (*end != '"' || end - json > 16384) { a->bad = true; return; }
    jbuf_t wrap; jbuf_init(&wrap, 64); jbuf_append(&wrap, "{\"v\":");
    jbuf_append_len(&wrap, json, (size_t)(end - json + 1)); jbuf_append_char(&wrap, '}');
    char *v = !strstr(wrap.data, "\\u0000") ? json_get_str(wrap.data, "v") : NULL;
    if (v) arg(a->args, v); else a->bad = true;
    free(v); jbuf_free(&wrap);
}
static bool tab_owned(workspace_t *w, int tab) {
    for (int i = 0; i < w->obs_count; i++) {
        observation_t *o = &w->obs[i];
        if (o->tab != tab) continue;
        view_t *v = find_view(w, o->surface);
        if (!v || v->closed || observed(w, v) != o) return false;
    }
    return true;
}
static void session_arg(jbuf_t *session, const char *value) {
    jbuf_append(session, " '");
    for (const char *p = value; *p; p++) {
        if (*p == '\'') jbuf_append(session, "'\"'\"'");
        else jbuf_append_char(session, *p);
    }
    jbuf_append_char(session, '\'');
}
static bool prepare_start(workspace_t *w, view_t *v, const char *command, yyjson_val *args,
                          char *path, size_t cap) {
    if (snprintf(path, cap, "%s/%s.kitty-session", w->dir, v->id) >= (int)cap) return false;
    jbuf_t session; jbuf_init(&session, 256);
    jbuf_appendf(&session, "new_tab DSCO workspace\nlayout splits\nlaunch --var dsco_workspace=%s --var dsco_surface=%s --", w->workspace, v->id);
    session_arg(&session, command);
    size_t index, count; yyjson_val *value;
    yyjson_arr_foreach(args, index, count, value) session_arg(&session, yyjson_get_str(value));
    jbuf_append_char(&session, '\n');
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    bool ok = fd >= 0;
    if (ok) {
        ok = write_all(fd, session.data, session.len) && !fsync(fd);
        close(fd);
        if (!ok) unlink(path);
    }
    if (ok) ok = sync_directory(w->dir);
    jbuf_free(&session); return ok;
}
static bool surface_call(workspace_t *w, const char *json, const char *action, char *result, size_t cap) {
    char error[1024] = "Kitty transport unavailable; call start to create an owned workspace";
    bool online = observe(w, error, sizeof(error));
    if (online) reconcile(w);
    if (same(action, "status")) return render(w, NULL, action, online, online ? NULL : error, result, cap);
    if (same(action, "start") && !online) {
        /* A transport/permission error is not evidence that launch is safe.
         * Persisted uncertain starts and extant sockets are never replaced. */
        for (int i = 0; i < w->count; i++) if (w->views[i].startup && w->views[i].pending) {
            return render(w, &w->views[i], action, false,
                          "startup outcome uncertain; existing pending startup will not be replayed", result, cap);
        }
        struct stat socket_state;
        if (!lstat(w->socket + 5, &socket_state) || errno != ENOENT)
            return fail(result, cap, "workspace socket exists but cannot be observed; refusing to launch another instance");
        if (w->count >= VIEW_CAP) return fail(result, cap, "ownership registry full; no workspace launched");
        const char *kitty = "/Applications/kitty.app/Contents/MacOS/kitty";
#ifndef __APPLE__
        kitty = "/usr/bin/kitty";
#endif
        char *command = json_get_str(json, "command");
        if (!command) command = strdup("/bin/sh");
        char *raw = json_get_raw(json, "args");
        yyjson_doc *args_doc = yyjson_read(raw ? raw : "[]", raw ? strlen(raw) : 2, 0);
        yyjson_val *start_args = args_doc ? yyjson_doc_get_root(args_doc) : NULL;
        bool args_valid = yyjson_is_arr(start_args) && yyjson_arr_size(start_args) <= 24;
        size_t index, count; yyjson_val *value;
        if (args_valid) yyjson_arr_foreach(start_args, index, count, value) {
            const char *text = yyjson_get_str(value);
            if (!text || yyjson_get_len(value) > 16384 || strlen(text) != yyjson_get_len(value) ||
                strchr(text, '\n') || strchr(text, '\r')) { args_valid = false; break; }
        }
        if (command[0] != '/' || strchr(command, '\n') || strchr(command, '\r') ||
            access(command, X_OK) || !args_valid) {
            free(command); free(raw); if (args_doc) yyjson_doc_free(args_doc);
            return fail(result, cap, "start requires an absolute executable and at most 24 string args without line breaks");
        }
        free(raw);
        view_t *v = &w->views[w->count]; token(v->id, sizeof(v->id));
        if (!v->id[0]) { free(command); yyjson_doc_free(args_doc); return fail(result, cap, "random identity unavailable"); }
        v->pending = v->startup = true;
        snprintf(v->request, sizeof(v->request), "%s", v->id);
        char *start_request = json_get_str(json, "request_id");
        if (start_request && !safe_name(start_request, sizeof(v->request))) {
            free(start_request); free(command); yyjson_doc_free(args_doc);
            return fail(result, cap, "invalid request_id");
        }
        if (start_request) snprintf(v->request, sizeof(v->request), "%s", start_request);
        free(start_request);
        copy_field(json, "run_id", v->run, sizeof(v->run));
        char session_path[PATH_MAX];
        if (!prepare_start(w, v, command, start_args, session_path, sizeof(session_path))) {
            free(command); yyjson_doc_free(args_doc); return fail(result, cap, "private startup session persistence failed; nothing launched");
        }
        yyjson_doc_free(args_doc);
        w->count++;
        if (!save_registry(w)) { free(command); return fail(result, cap, "startup tombstone persistence failed; nothing launched"); }
        char *argv[] = {(char *)kitty, "--detach", "--listen-on", w->socket,
                        "-o", "allow_remote_control=socket-only", "-o", "enabled_layouts=splits,tall,grid,stack",
                        "--title", "DSCO workspace", "--start-as", json_get_bool(json, "visible", false) ? "normal" : "minimized",
                        "--session", session_path, NULL};
        process_capture_t capture;
        process_capture(kitty, argv, 3000, 2048, &capture);
        int spawn_error = capture.spawn_error;
        process_capture_free(&capture); free(command);
        if (spawn_error) {
            v->pending = false; v->closed = true;
            if (!save_registry(w)) return fail(result, cap, "spawn failed; startup outcome persistence failed");
            return fail(result, cap, "owned Kitty process could not be spawned");
        }
        double deadline = now_ms() + 4000;
        do {
            if (observe(w, error, sizeof(error))) { online = true; break; }
            struct timespec pause = {.tv_nsec = 50000000}; nanosleep(&pause, NULL);
        } while (now_ms() < deadline);
        if (!online) return render(w, v, action, false, "startup outcome uncertain; reconcile before any further launch", result, cap);
        reconcile(w);
        if (!observed(w, v)) return render(w, v, action, false, "Kitty online but startup ownership tag not observed; launch will not be replayed", result, cap);
        if (!save_registry(w)) return fail(result, cap, "ownership persistence failed");
        return render(w, v, action, true, NULL, result, cap);
    }
    if (!online) return fail(result, cap, error);
    if (same(action, "start") || same(action, "list")) {
        if (same(action, "start") && !save_registry(w)) return fail(result, cap, "registry persistence failed");
        return render(w, NULL, action, true, NULL, result, cap);
    }
    char *id = json_get_str(json, "surface_id");
    view_t *v = find_view(w, id); free(id);
    if (same(action, "create")) {
        char *request = json_get_str(json, "request_id");
        if (request && !safe_name(request, 80)) { free(request); return fail(result, cap, "invalid request_id"); }
        for (int i = 0; request && i < w->count; i++) if (same(w->views[i].request, request)) {
            free(request);
            if (!save_registry(w)) return fail(result, cap, "request reconciliation persistence failed");
            return render(w, &w->views[i], action, observed(w, &w->views[i]) != NULL, "reconciled existing request; never replayed", result, cap);
        }
        char *source_id = json_get_str(json, "source_surface_id");
        view_t *source = source_id ? find_view(w, source_id) : NULL;
        if (!source_id) for (int i = 0; i < w->count; i++) if (!w->views[i].closed && observed(w, &w->views[i])) { source = &w->views[i]; break; }
        free(source_id);
        observation_t *src = source && !source->closed ? observed(w, source) : NULL;
        if (!src || w->count >= VIEW_CAP) { free(request); return fail(result, cap, "owned source surface required or registry full"); }
        char *command = json_get_str(json, "command"), *kind = json_get_str(json, "type"), *location = json_get_str(json, "location"), *title = json_get_str(json, "title");
        if (!command) command = strdup("/bin/sh");
        if (!kind) kind = strdup("window");
        if (!location) location = strdup("vsplit");
        bool valid = command[0] == '/' && !access(command, X_OK) &&
                     (same(kind, "window") || same(kind, "tab") || same(kind, "os-window")) &&
                     (same(location, "split") || same(location, "vsplit") || same(location, "hsplit"));
        jbuf_t args; jbuf_init(&args, 512); jbuf_append_char(&args, '[');
        view_t draft = {0}; token(draft.id, sizeof(draft.id));
        snprintf(draft.request, sizeof(draft.request), "%s", request ? request : draft.id);
        copy_field(json, "run_id", draft.run, sizeof(draft.run)); draft.pending = true;
        char match[48], next[48], workspace_var[100], surface_var[110];
        snprintf(match, sizeof(match), "--match=window_id:%d", src->id);
        snprintf(next, sizeof(next), "id:%d", src->id);
        snprintf(workspace_var, sizeof(workspace_var), "dsco_workspace=%s", w->workspace);
        snprintf(surface_var, sizeof(surface_var), "dsco_surface=%s", draft.id);
        arg(&args, "--type"); arg(&args, kind); arg(&args, "--location"); arg(&args, location);
        arg(&args, match); arg(&args, "--next-to"); arg(&args, next);
        arg(&args, "--dont-take-focus"); arg(&args, "--title"); arg(&args, title ? title : "DSCO pane");
        arg(&args, "--var"); arg(&args, workspace_var); arg(&args, "--var"); arg(&args, surface_var);
        arg(&args, "--"); arg(&args, command);
        char *raw = json_get_raw(json, "args");
        if (raw && *raw != '[') valid = false;
        extra_args_t extra = {.args = &args}; json_array_foreach(json, "args", extra_arg, &extra);
        valid = valid && !extra.bad && draft.id[0]; jbuf_append_char(&args, ']');
        free(raw); free(request); free(command); free(kind); free(location); free(title);
        if (!valid) { jbuf_free(&args); return fail(result, cap, "invalid create executable/type/location/args"); }
        v = &w->views[w->count++]; *v = draft;
        if (!save_registry(w)) { jbuf_free(&args); return fail(result, cap, "pending launch persistence failed"); }
        bool launched = remote(w, "launch", &args, error, sizeof(error)); jbuf_free(&args);
        bool refreshed = observe(w, error, sizeof(error));
        if (refreshed) reconcile(w);
        if (!save_registry(w)) return fail(result, cap, "launch may have succeeded; registry persistence failed");
        return render(w, v, action, refreshed && observed(w, v), launched ? NULL : "launch outcome uncertain; reconcile this request_id", result, cap);
    }
    if (!v) return fail(result, cap, "unknown surface_id; discover owned IDs with list");
    observation_t *o = observed(w, v);
    if (same(action, "inspect")) return render(w, v, action, true, NULL, result, cap);
    if (v->closed || !o) return fail(result, cap, "surface closed or ownership changed; explicit create is required");
    observation_t before = *o;
    int prior_observation_count = w->obs_count;
    char match[48], tab_match[48]; snprintf(match, sizeof(match), "--match=id:%d", o->id);
    snprintf(tab_match, sizeof(tab_match), "--match=id:%d", o->tab);
    const char *command = NULL; bool valid = true;
    jbuf_t args; jbuf_init(&args, 512); jbuf_append_char(&args, '[');
    if (same(action, "layout")) {
        char *layout = json_get_str(json, "layout");
        valid = same(layout, "splits") || same(layout, "tall") || same(layout, "grid") || same(layout, "stack");
        /* A user may move an unrelated pane into this tab. Do not rearrange it. */
        if (!tab_owned(w, o->tab)) valid = false;
        command = "goto-layout"; arg(&args, tab_match); arg(&args, layout ? layout : ""); free(layout);
    } else {
        arg(&args, match);
        if (same(action, "focus")) command = "focus-window";
        else if (same(action, "close")) command = "close-window";
        else if (same(action, "detach")) command = "detach-window";
        else if (same(action, "resize")) {
            char *axis = json_get_str(json, "axis"); if (!axis) axis = strdup("horizontal");
            int inc = json_get_int(json, "increment", 1);
            valid = (same(axis, "horizontal") || same(axis, "vertical") || same(axis, "reset")) && inc >= -1000 && inc <= 1000;
            if (!tab_owned(w, o->tab)) valid = false;
            char increment[24]; snprintf(increment, sizeof(increment), "%d", inc);
            command = "resize-window"; arg(&args, "--axis"); arg(&args, axis);
            arg(&args, "--increment"); arg(&args, increment); free(axis);
        } else if (same(action, "read")) {
            char *extent = json_get_str(json, "extent"); if (!extent) extent = strdup("screen");
            valid = same(extent, "screen") || same(extent, "all") || same(extent, "last_cmd_output");
            command = "get-text"; arg(&args, "--extent"); arg(&args, extent); free(extent);
        } else if (same(action, "send_text") || same(action, "send_key")) {
            bool text = same(action, "send_text");
            char *value = json_get_str(json, text ? "text" : "key");
            valid = value && strlen(value) <= 16384;
            command = text ? "send-text" : "send-key";
            arg(&args, "--");
            jbuf_t literal; jbuf_init(&literal, 64);
            for (const char *p = value ? value : ""; *p; p++) {
                if (text && *p == '\\') jbuf_append_char(&literal, '\\');
                jbuf_append_char(&literal, *p);
            }
            arg(&args, literal.data); jbuf_free(&literal); free(value);
        } else valid = false;
    }
    jbuf_append_char(&args, ']');
    if (!valid || !command) { jbuf_free(&args); return fail(result, cap, "invalid or unsupported surface action/arguments"); }
    char *reply = malloc(cap > 1024 ? cap : 1024);
    if (!reply) { jbuf_free(&args); return false; }
    bool ok;
    if (same(action, "read")) {
        /* Only exact, currently observed owned panes get the narrower scope.
         * This is thread-local authority, never accepted from tool JSON. */
        bool previous_scope = surface_policy_owned_read_scope(true);
        ok = remote(w, command, &args, reply, cap > 1024 ? cap : 1024);
        surface_policy_owned_read_scope(previous_scope);
    } else ok = remote(w, command, &args, reply, cap > 1024 ? cap : 1024);
    jbuf_free(&args);
    if (!ok) { bool ret = fail(result, cap, reply); free(reply); return ret; }
    if (same(action, "read")) {
        /* Preserve the bounded raw text envelope and its explicit truncation flag. */
        if (strlen(reply) < cap) strcpy(result, reply); else ok = fail(result, cap, "result too large");
        free(reply); return ok;
    }
    free(reply);
    bool refreshed = observe(w, error, sizeof(error));
    if (refreshed) reconcile(w);
    observation_t *after = refreshed ? observed(w, v) : NULL;
    bool verified = false;
    if (same(action, "close")) verified = refreshed && !after;
    else if (same(action, "focus")) verified = after && after->focused;
    else if (same(action, "detach")) verified = after && after->tab != before.tab;
    else if (same(action, "resize")) verified = after && (after->cols != before.cols || after->rows != before.rows);
    else if (same(action, "layout")) {
        char *layout = json_get_str(json, "layout"); verified = after && same(after->layout, layout); free(layout);
    }
    if (same(action, "close") && !refreshed && prior_observation_count == 1) {
        struct stat socket_state;
        if (lstat(w->socket + 5, &socket_state) && errno == ENOENT) {
            v->closed = true; v->pending = false; verified = true;
        }
    }
    if (!save_registry(w)) return fail(result, cap, "action executed; registry persistence failed");
    return render(w, v, action, verified, verified ? NULL : "command accepted; postcondition unverified (input requires subsequent observation)", result, cap);
}

bool tool_surface(const char *input, char *result, size_t cap) {
    if (!result || !cap) return false;
    yyjson_doc *doc = input ? yyjson_read(input, strlen(input), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool valid_json = yyjson_is_obj(root);
    size_t index, count; yyjson_val *key, *value;
    if (valid_json) yyjson_obj_foreach(root, index, count, key, value) {
        if (strlen(yyjson_get_str(key)) != yyjson_get_len(key) ||
            (yyjson_is_str(value) && strlen(yyjson_get_str(value)) != yyjson_get_len(value))) valid_json = false;
    }
    if (valid_json) {
        yyjson_val *name = yyjson_obj_get(root, "workspace");
        if (name && !yyjson_is_str(name)) valid_json = false;
    }
    if (doc) yyjson_doc_free(doc);
    if (!valid_json) return fail(result, cap, "expected a JSON object with representable string fields");
    char *name = json_get_str(input, "workspace"), *action = json_get_str(input, "action");
    if (!name) name = strdup("main");
    if (!safe_name(name, 64) || !action) { free(name); free(action); return fail(result, cap, "valid workspace and action required"); }
    static const char *const actions[] = {"status", "start", "list", "inspect", "create", "focus",
        "resize", "layout", "detach", "read", "send_text", "send_key", "close", NULL};
    bool known = false;
    for (int i = 0; actions[i]; i++) if (same(action, actions[i])) known = true;
    if (!known) { free(name); free(action); return fail(result, cap, "unknown surface action"); }
    bool read_only = same(action, "status") || same(action, "list") || same(action, "inspect") || same(action, "read");
    pthread_mutex_lock(&surface_mutex);
    workspace_t *w = calloc(1, sizeof(*w));
    bool ok = false; int lock = -1;
    if (!w) { fail(result, cap, "workspace allocation failed"); goto done; }
    if (!paths(w, name, same(action, "start"))) {
        if (same(action, "status") && errno == ENOENT)
            ok = render(w, NULL, action, false, "workspace has not been started", result, cap);
        else fail(result, cap, "private workspace directory unavailable or socket path too long");
        goto done;
    }
    char lockpath[PATH_MAX]; snprintf(lockpath, sizeof(lockpath), "%s/lock", w->dir);
    lock = open(lockpath, (same(action, "start") ? O_CREAT : 0) |
                (read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) { fail(result, cap, "workspace lock unavailable"); goto done; }
    struct stat lock_state;
    if (fstat(lock, &lock_state) || !S_ISREG(lock_state.st_mode) || lock_state.st_uid != getuid() || (lock_state.st_mode & 0077)) {
        fail(result, cap, "workspace lock is not a private owned regular file"); goto done;
    }
    double deadline = now_ms() + 2000;
    while (flock(lock, (read_only ? LOCK_SH : LOCK_EX) | LOCK_NB)) {
        if (errno != EWOULDBLOCK || now_ms() >= deadline) { fail(result, cap, "workspace busy"); goto done; }
        struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
    }
    if (!load_registry(w)) { fail(result, cap, "invalid ownership registry; refusing to overwrite it"); goto done; }
    ok = surface_call(w, input, action, result, cap);
done:
    if (lock >= 0) { flock(lock, LOCK_UN); close(lock); }
    free(w); free(name); free(action); pthread_mutex_unlock(&surface_mutex); return ok;
}
