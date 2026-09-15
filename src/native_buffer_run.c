#define _DARWIN_C_SOURCE 1

#include "native_buffer_run.h"
#include "crypto.h"
#include "tools.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NBR_RESULT_CAP (128u * 1024u)
#define NBR_JSON_CAP (256u * 1024u)
#define NBR_BUFFER_CONTENT_CAP (1024u * 1024u)
#define NBR_TIER_CAP 32u
#define NBR_ID_CAP 80u

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool ok;
} nbr_string_t;

typedef struct {
    char source[NATIVE_WINDOW_TEXT_CAP];
    size_t source_len;
    char source_hash[65];
    char base_revision[65];
    size_t selection_start;
    size_t selection_end;
    char cwd[PATH_MAX];
    char workspace[64];
    char tier[NBR_TIER_CAP];
} nbr_request_t;

typedef struct {
    bool launching;
    bool operation_in_flight;
    bool has_handle;
    bool running;
    unsigned long generation;
    char bash_id[NBR_ID_CAP];
    char buffer_id[37];
    char buffer_revision[65];
    char workspace[64];
    char source_hash[65];
    char tier[NBR_TIER_CAP];
} nbr_state_t;

typedef struct {
    pthread_mutex_t mu;
    native_buffer_run_hooks_t hooks;
    nbr_state_t run;
} nbr_runtime_t;

static nbr_runtime_t g_nbr = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
};

static void nbr_string_init(nbr_string_t *s, size_t initial) {
    memset(s, 0, sizeof(*s));
    s->cap = initial ? initial : 128;
    s->data = malloc(s->cap);
    s->ok = s->data != NULL;
    if (s->ok) s->data[0] = '\0';
}

static bool nbr_string_reserve(nbr_string_t *s, size_t extra) {
    if (!s || !s->ok || extra > NBR_BUFFER_CONTENT_CAP || s->len > NBR_BUFFER_CONTENT_CAP - extra)
        return false;
    size_t need = s->len + extra + 1;
    if (need <= s->cap) return true;
    size_t next = s->cap;
    while (next < need) {
        if (next > NBR_BUFFER_CONTENT_CAP / 2) { next = NBR_BUFFER_CONTENT_CAP; break; }
        next *= 2;
    }
    char *p = realloc(s->data, next);
    if (!p) { s->ok = false; return false; }
    s->data = p;
    s->cap = next;
    return true;
}

static bool nbr_string_put_len(nbr_string_t *s, const char *p, size_t n) {
    if (!n) return s && s->ok;
    if (!p || !nbr_string_reserve(s, n)) return false;
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = '\0';
    return true;
}

static bool nbr_string_put(nbr_string_t *s, const char *p) {
    return p && nbr_string_put_len(s, p, strlen(p));
}

static bool nbr_string_char(nbr_string_t *s, char c) {
    return nbr_string_put_len(s, &c, 1);
}

static bool nbr_string_json(nbr_string_t *s, const char *p) {
    if (!p || !nbr_string_char(s, '"')) return false;
    for (const unsigned char *at = (const unsigned char *)p; *at; at++) {
        switch (*at) {
        case '"': if (!nbr_string_put(s, "\\\"")) return false; break;
        case '\\': if (!nbr_string_put(s, "\\\\")) return false; break;
        case '\b': if (!nbr_string_put(s, "\\b")) return false; break;
        case '\f': if (!nbr_string_put(s, "\\f")) return false; break;
        case '\n': if (!nbr_string_put(s, "\\n")) return false; break;
        case '\r': if (!nbr_string_put(s, "\\r")) return false; break;
        case '\t': if (!nbr_string_put(s, "\\t")) return false; break;
        default:
            if (*at < 0x20) {
                char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", *at);
                if (!nbr_string_put(s, escaped)) return false;
            } else if (!nbr_string_char(s, (char)*at)) return false;
            break;
        }
    }
    return nbr_string_char(s, '"');
}

static void nbr_string_free(nbr_string_t *s) {
    if (!s) return;
    free(s->data);
    memset(s, 0, sizeof(*s));
}

static native_buffer_run_hooks_t nbr_hooks(void) {
    native_buffer_run_hooks_t hooks;
    pthread_mutex_lock(&g_nbr.mu);
    hooks = g_nbr.hooks;
    pthread_mutex_unlock(&g_nbr.mu);
    return hooks;
}

static bool nbr_execute_default(const char *name, const char *input, const char *tier,
                                char *result, size_t result_len) {
    return tools_execute_for_tier(name, input, tier, result, result_len);
}

static void nbr_snapshot_default(native_windows_snapshot_t *out) {
    native_windows_snapshot(out);
}

static bool nbr_bind_default(uint64_t id, const char *title, const char *text,
                             const char *buffer_id, const char *revision,
                             const char *workspace, bool sensitive,
                             char *result, size_t result_len) {
    return native_windows_bind_buffer(id, title, text, buffer_id, revision,
                                      workspace, sensitive, result, result_len);
}

static bool nbr_cwd_default(char *out, size_t out_len) {
    return out && out_len && getcwd(out, out_len) != NULL;
}

static bool nbr_call_execute(const native_buffer_run_hooks_t *hooks, const char *name,
                             const char *input, const char *tier, char *result, size_t cap) {
    native_buffer_run_execute_fn execute = hooks && hooks->execute ? hooks->execute : nbr_execute_default;
    return execute(name, input, tier, result, cap);
}

static bool nbr_json_field_string(const char *json, const char *key, char *out, size_t cap) {
    if (!json || !key || !out || cap < 1) return false;
    char needle[96];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return false;
    const char *at = json;
    while ((at = strstr(at, needle)) != NULL) {
        const char *p = at + n;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p++ != ':') { at++; continue; }
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p++ != '"') { at++; continue; }
        size_t used = 0;
        while (*p && *p != '"') {
            unsigned char c = (unsigned char)*p++;
            if (c == '\\') {
                c = (unsigned char)*p++;
                if (!c) return false;
                switch (c) {
                case '"': case '\\': case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u':
                    /* IDs/statuses used by this adapter are ASCII. Preserve a
                     * non-ASCII escape as a harmless placeholder if encountered. */
                    for (int i = 0; i < 4 && *p; i++) p++;
                    c = '?';
                    break;
                default: return false;
                }
            }
            if (used + 1 >= cap) return false;
            out[used++] = (char)c;
        }
        if (*p != '"') return false;
        out[used] = '\0';
        return true;
    }
    return false;
}

static bool nbr_json_status(const char *json, const char *status) {
    char value[32];
    return nbr_json_field_string(json, "status", value, sizeof(value)) && !strcmp(value, status);
}

static bool nbr_copy_cstr(char *dst, size_t cap, const char *src) {
    if (!dst || !cap || !src || strlen(src) >= cap) return false;
    strcpy(dst, src);
    return true;
}

static bool nbr_capture(nbr_request_t *request) {
    if (!request) return false;
    memset(request, 0, sizeof(*request));
    native_buffer_run_hooks_t hooks = nbr_hooks();
    native_buffer_run_snapshot_fn snapshot = hooks.snapshot ? hooks.snapshot : nbr_snapshot_default;
    native_windows_snapshot_t model;
    memset(&model, 0, sizeof(model));
    snapshot(&model);

    const native_window_t *focused = NULL;
    for (int i = 0; i < model.count && i < NATIVE_WINDOWS_MAX; i++) {
        if (model.windows[i].id == model.focused_id) { focused = &model.windows[i]; break; }
    }
    if (!focused || focused->kind != NATIVE_WINDOW_BUFFER) return false;

    size_t start = 0, end = 0;
    if (!native_buffer_editor_get_selected_range(&focused->editor, focused->text, &start, &end))
        return false;
    size_t text_len = strnlen(focused->text, sizeof(focused->text));
    if (start >= end || end > text_len || end - start >= sizeof(request->source)) return false;
    memcpy(request->source, focused->text + start, end - start);
    request->source[end - start] = '\0';
    request->source_len = end - start;
    request->selection_start = start;
    request->selection_end = end;
    sha256_hex((const uint8_t *)request->source, request->source_len, request->source_hash);
    if (!nbr_copy_cstr(request->base_revision, sizeof(request->base_revision), focused->buffer_revision)) return false;
    if (!focused->workspace[0]) strcpy(request->workspace, "main");
    else if (!nbr_copy_cstr(request->workspace, sizeof(request->workspace), focused->workspace)) return false;
    native_buffer_run_cwd_fn getcwd_fn = hooks.getcwd ? hooks.getcwd : nbr_cwd_default;
    if (!getcwd_fn(request->cwd, sizeof(request->cwd)) || !request->cwd[0]) return false;
    const char *tier = tools_execution_tier();
    if (!tier || !*tier) tier = "standard";
    return nbr_copy_cstr(request->tier, sizeof(request->tier), tier);
}

static bool nbr_reserve_start(unsigned long *generation) {
    bool ok = false;
    pthread_mutex_lock(&g_nbr.mu);
    if (!g_nbr.run.launching && !g_nbr.run.operation_in_flight && !g_nbr.run.running) {
        g_nbr.run.launching = true;
        g_nbr.run.generation++;
        if (generation) *generation = g_nbr.run.generation;
        ok = true;
    }
    pthread_mutex_unlock(&g_nbr.mu);
    return ok;
}

static bool nbr_reserve_operation(nbr_state_t *copy) {
    bool ok = false;
    pthread_mutex_lock(&g_nbr.mu);
    if (g_nbr.run.has_handle && !g_nbr.run.launching && !g_nbr.run.operation_in_flight &&
        g_nbr.run.bash_id[0]) {
        g_nbr.run.operation_in_flight = true;
        if (copy) *copy = g_nbr.run;
        ok = true;
    }
    pthread_mutex_unlock(&g_nbr.mu);
    return ok;
}

static bool nbr_make_source_content(const nbr_request_t *request, nbr_string_t *out) {
    char offsets[96];
    snprintf(offsets, sizeof(offsets), "%zu:%zu", request->selection_start, request->selection_end);
    nbr_string_init(out, request->source_len + 512);
    if (!out->ok) return false;
    if (!nbr_string_put(out, "native_buffer_run v1\nsource_sha256: ") ||
        !nbr_string_put(out, request->source_hash) ||
        !nbr_string_put(out, "\nbase_revision: ") ||
        !nbr_string_put(out, request->base_revision) ||
        !nbr_string_put(out, "\nselection_offsets: ") ||
        !nbr_string_put(out, offsets) ||
        !nbr_string_put(out, "\ncwd: ") || !nbr_string_put(out, request->cwd) ||
        !nbr_string_put(out, "\nsource:\n") || !nbr_string_put_len(out, request->source, request->source_len) ||
        !nbr_string_put(out, "\n")) {
        nbr_string_free(out); return false;
    }
    return true;
}

/* Keep formatting in one helper so the adapter never interpolates source text
 * into the shell command. */
static bool nbr_make_create_input(const nbr_request_t *request, unsigned long generation,
                                  const char *name, const char *content, nbr_string_t *out) {
    char request_id[96];
    snprintf(request_id, sizeof(request_id), "native-buffer-run-%lu", generation);
    nbr_string_init(out, strlen(content) + 512);
    if (!out->ok || !nbr_string_put(out, "{\"action\":\"create\",\"workspace\":") ||
        !nbr_string_json(out, request->workspace) || !nbr_string_put(out, ",\"name\":") ||
        !nbr_string_json(out, name) || !nbr_string_put(out, ",\"kind\":\"log\",\"content\":") ||
        !nbr_string_json(out, content) || !nbr_string_put(out, ",\"request_id\":") ||
        !nbr_string_json(out, request_id) || !nbr_string_put(out, "}")) {
        nbr_string_free(out); return false;
    }
    return true;
}

static bool nbr_make_run_input(const nbr_request_t *request, nbr_string_t *out) {
    static const char hex[] = "0123456789abcdef";
    nbr_string_init(out, request->source_len * 2 + 512);
    if (!out->ok || !nbr_string_put(out, "{\"command\":\"python3 -u -c 'exec(bytes.fromhex(\\\"")) {
        nbr_string_free(out); return false;
    }
    for (size_t i = 0; i < request->source_len; i++) {
        char pair[2] = {hex[((unsigned char)request->source[i]) >> 4], hex[((unsigned char)request->source[i]) & 15]};
        if (!nbr_string_put_len(out, pair, sizeof(pair))) { nbr_string_free(out); return false; }
    }
    if (!nbr_string_put(out, "\\\").decode())'\",\"run_in_background\":true,\"timeout\":30,\"cwd\":") ||
        !nbr_string_json(out, request->cwd) || !nbr_string_put(out, "}")) {
        nbr_string_free(out); return false;
    }
    return true;
}

static bool nbr_make_simple_input(const char *action, const char *id, nbr_string_t *out) {
    nbr_string_init(out, 192);
    if (!out->ok || !nbr_string_put(out, "{\"") || !nbr_string_put(out, action) ||
        !nbr_string_put(out, "\":") || !nbr_string_json(out, id) || !nbr_string_put(out, "}")) {
        nbr_string_free(out); return false;
    }
    return true;
}

static bool nbr_append_receipt(const native_buffer_run_hooks_t *hooks, const nbr_state_t *run,
                               const char *event, const char *raw, unsigned long generation,
                               char *new_revision, size_t revision_cap) {
    if (!run || !run->buffer_id[0] || !run->workspace[0] || !run->tier[0] || !event || !raw)
        return false;
    nbr_string_t content, input;
    nbr_string_init(&content, strlen(raw) + 128);
    bool built = content.ok && nbr_string_put(&content, "\n--- ") && nbr_string_put(&content, event) &&
        nbr_string_put(&content, " receipt ---\n") && nbr_string_put(&content, raw) &&
        nbr_string_put(&content, "\n");
    if (!built) { nbr_string_free(&content); return false; }

    nbr_string_init(&input, content.len + 512);
    char request_id[96];
    snprintf(request_id, sizeof(request_id), "native-buffer-%s-%lu", event, generation);
    built = input.ok && nbr_string_put(&input, "{\"action\":\"append\",\"workspace\":") &&
        nbr_string_json(&input, run->workspace) && nbr_string_put(&input, ",\"buffer_id\":") &&
        nbr_string_json(&input, run->buffer_id);
    if (run->buffer_revision[0]) built = built && nbr_string_put(&input, ",\"expected_revision\":") &&
        nbr_string_json(&input, run->buffer_revision);
    built = built && nbr_string_put(&input, ",\"content\":") && nbr_string_json(&input, content.data) &&
        nbr_string_put(&input, ",\"request_id\":") && nbr_string_json(&input, request_id) &&
        nbr_string_put(&input, "}");
    nbr_string_free(&content);
    if (!built) { nbr_string_free(&input); return false; }

    char *result = calloc(1, NBR_RESULT_CAP);
    if (!result) { nbr_string_free(&input); return false; }
    bool ok = nbr_call_execute(hooks, "buffer", input.data, run->tier, result, NBR_RESULT_CAP);
    if (ok && new_revision && revision_cap) {
        char revision[65];
        if (nbr_json_field_string(result, "revision", revision, sizeof(revision)))
            nbr_copy_cstr(new_revision, revision_cap, revision);
    }
    free(result);
    nbr_string_free(&input);
    return ok;
}

static bool nbr_make_view(const nbr_request_t *request, const char *raw, char *out, size_t cap) {
    if (!request || !raw || !out || cap < 2) return false;
    int n = snprintf(out, cap, "Native Python output\nsource_sha256: %s\nselection_offsets: %zu:%zu\n\n",
                     request->source_hash, request->selection_start, request->selection_end);
    if (n < 0 || (size_t)n >= cap) return false;
    size_t used = (size_t)n;
    size_t room = cap - used - 1;
    size_t raw_len = strlen(raw);
    if (raw_len > room) raw_len = room;
    while (raw_len && raw_len < strlen(raw) && ((unsigned char)raw[raw_len] & 0xc0) == 0x80) raw_len--;
    memcpy(out + used, raw, raw_len);
    used += raw_len;
    if (used + 1 < cap && raw_len < strlen(raw)) {
        const char *tail = "\n[truncated]";
        size_t tail_len = strlen(tail);
        if (tail_len < cap - used) { memcpy(out + used, tail, tail_len); used += tail_len; }
    }
    out[used] = '\0';
    return true;
}

static bool nbr_start(void) {
    nbr_request_t request;
    if (!nbr_capture(&request)) return false;
    unsigned long generation = 0;
    if (!nbr_reserve_start(&generation)) return false;

    char name[160];
    snprintf(name, sizeof(name), "native-buffer-run-%.16s-%zu-%zu",
             request.source_hash, request.selection_start, request.selection_end);
    nbr_string_t content;
    bool ok = nbr_make_source_content(&request, &content);
    nbr_string_t create_input = {0};
    if (ok) ok = nbr_make_create_input(&request, generation, name, content.data, &create_input);
    nbr_string_free(&content);
    native_buffer_run_hooks_t hooks = nbr_hooks();
    char *result = calloc(1, NBR_RESULT_CAP);
    char buffer_id[37] = {0}, buffer_revision[65] = {0};
    if (ok) ok = result && nbr_call_execute(&hooks, "buffer", create_input.data, request.tier,
                                             result, NBR_RESULT_CAP);
    if (ok) ok = nbr_json_field_string(result, "buffer_id", buffer_id, sizeof(buffer_id)) &&
                  nbr_json_field_string(result, "revision", buffer_revision, sizeof(buffer_revision));
    nbr_string_free(&create_input);
    if (!ok) {
        free(result);
        pthread_mutex_lock(&g_nbr.mu);
        g_nbr.run.launching = false;
        pthread_mutex_unlock(&g_nbr.mu);
        return false;
    }

    nbr_state_t pending;
    memset(&pending, 0, sizeof(pending));
    pending.has_handle = true;
    pending.running = false;
    pending.generation = generation;
    nbr_copy_cstr(pending.buffer_id, sizeof(pending.buffer_id), buffer_id);
    nbr_copy_cstr(pending.buffer_revision, sizeof(pending.buffer_revision), buffer_revision);
    nbr_copy_cstr(pending.workspace, sizeof(pending.workspace), request.workspace);
    nbr_copy_cstr(pending.source_hash, sizeof(pending.source_hash), request.source_hash);
    nbr_copy_cstr(pending.tier, sizeof(pending.tier), request.tier);
    bool create_receipt_ok = nbr_append_receipt(&hooks, &pending, "create", result, generation,
                                                pending.buffer_revision, sizeof(pending.buffer_revision));
    free(result);

    nbr_string_t run_input;
    ok = create_receipt_ok && nbr_make_run_input(&request, &run_input);
    char *run_result = calloc(1, NBR_RESULT_CAP);
    bool run_ok = false;
    if (ok) run_ok = run_result && nbr_call_execute(&hooks, "Bash", run_input.data, request.tier,
                                                    run_result, NBR_RESULT_CAP);
    if (!ok) {
        free(run_result);
        pthread_mutex_lock(&g_nbr.mu);
        g_nbr.run.launching = false;
        pthread_mutex_unlock(&g_nbr.mu);
        return false;
    }
    char bash_id[NBR_ID_CAP] = {0};
    bool handle_ok = run_ok && nbr_json_field_string(run_result, "bash_id", bash_id, sizeof(bash_id));
    if (!handle_ok && run_ok) handle_ok = nbr_json_field_string(run_result, "shell_id", bash_id, sizeof(bash_id));
    bool receipt_ok = nbr_append_receipt(&hooks, &pending, "run", run_result, generation,
                                         pending.buffer_revision, sizeof(pending.buffer_revision));
    free(run_result);
    nbr_string_free(&run_input);

    pthread_mutex_lock(&g_nbr.mu);
    if (g_nbr.run.generation == generation) {
        g_nbr.run.launching = false;
        if (handle_ok) {
            pending.running = true;
            nbr_copy_cstr(pending.bash_id, sizeof(pending.bash_id), bash_id);
            g_nbr.run = pending;
        }
    }
    pthread_mutex_unlock(&g_nbr.mu);
    return handle_ok && receipt_ok;
}

static bool nbr_collect(bool cancel_after_kill) {
    nbr_state_t run;
    if (!nbr_reserve_operation(&run)) return false;
    native_buffer_run_hooks_t hooks = nbr_hooks();
    char *result = calloc(1, NBR_RESULT_CAP);
    bool first_ok = false, second_ok = true;
    char *partial = NULL;
    nbr_string_t input;
    if (cancel_after_kill) {
        if (nbr_make_simple_input("shell_id", run.bash_id, &input)) {
            first_ok = result && nbr_call_execute(&hooks, "KillShell", input.data, run.tier,
                                                  result, NBR_RESULT_CAP);
            second_ok = nbr_append_receipt(&hooks, &run, "cancel", result ? result : "", run.generation,
                                           run.buffer_revision, sizeof(run.buffer_revision));
            nbr_string_free(&input);
        } else first_ok = false;
        free(result);
        result = NULL;
        partial = calloc(1, NBR_RESULT_CAP);
        if (nbr_make_simple_input("bash_id", run.bash_id, &input)) {
            bool output_ok = partial && nbr_call_execute(&hooks, "BashOutput", input.data, run.tier,
                                                         partial, NBR_RESULT_CAP);
            second_ok = second_ok && nbr_append_receipt(&hooks, &run, "collect", partial ? partial : "",
                                                         run.generation, run.buffer_revision,
                                                         sizeof(run.buffer_revision));
            first_ok = first_ok && output_ok;
            nbr_string_free(&input);
        } else first_ok = false;
        free(partial);
        pthread_mutex_lock(&g_nbr.mu);
        if (g_nbr.run.generation == run.generation) {
            g_nbr.run.operation_in_flight = false;
            if (first_ok) g_nbr.run.running = false;
        }
        pthread_mutex_unlock(&g_nbr.mu);
        return first_ok && second_ok;
    }

    if (!nbr_make_simple_input("bash_id", run.bash_id, &input)) goto collect_done;
    first_ok = result && nbr_call_execute(&hooks, "BashOutput", input.data, run.tier,
                                          result, NBR_RESULT_CAP);
    nbr_string_free(&input);
    if (!result) goto collect_done;
    second_ok = nbr_append_receipt(&hooks, &run, "collect", result, run.generation,
                                   run.buffer_revision, sizeof(run.buffer_revision));

    bool completed = first_ok && nbr_json_status(result, "completed");
    if (first_ok && !completed) run.running = true;
    if (completed) run.running = false;

    {
        native_buffer_run_bind_fn bind = hooks.bind_buffer ? hooks.bind_buffer : nbr_bind_default;
        char view[NATIVE_WINDOW_TEXT_CAP];
        char bind_result[NBR_RESULT_CAP];
        bool view_ok = nbr_make_view(&(nbr_request_t){
            .selection_start = 0, .selection_end = 0
        }, result, view, sizeof(view));
        /* The linkage header is reconstructed with the retained hash. */
        int header = snprintf(view, sizeof(view), "Native Python output\nsource_sha256: %s\n\n",
                              run.source_hash);
        if (header >= 0 && (size_t)header < sizeof(view)) {
            size_t used = (size_t)header, room = sizeof(view) - used - 1, n = strlen(result);
            if (n > room) n = room;
            memcpy(view + used, result, n); view[used + n] = '\0';
            view_ok = true;
        } else view_ok = false;
        bool bound = view_ok && bind(0, "Native Python output", view, run.buffer_id,
                                     run.buffer_revision, run.workspace, false,
                                     bind_result, sizeof(bind_result));
        second_ok = second_ok && bound;
    }
    free(result);
    pthread_mutex_lock(&g_nbr.mu);
    if (g_nbr.run.generation == run.generation) {
        g_nbr.run.operation_in_flight = false;
        if (first_ok) {
            g_nbr.run.running = run.running;
            nbr_copy_cstr(g_nbr.run.buffer_revision, sizeof(g_nbr.run.buffer_revision), run.buffer_revision);
        }
    }
    pthread_mutex_unlock(&g_nbr.mu);
    return first_ok && second_ok;

collect_done:
    free(result);
    pthread_mutex_lock(&g_nbr.mu);
    if (g_nbr.run.generation == run.generation) g_nbr.run.operation_in_flight = false;
    pthread_mutex_unlock(&g_nbr.mu);
    return false;
}

void native_buffer_run_set_hooks(const native_buffer_run_hooks_t *hooks) {
    pthread_mutex_lock(&g_nbr.mu);
    if (hooks) g_nbr.hooks = *hooks;
    else memset(&g_nbr.hooks, 0, sizeof(g_nbr.hooks));
    pthread_mutex_unlock(&g_nbr.mu);
}

bool native_buffer_run_reset(void) {
    bool ok = false;
    pthread_mutex_lock(&g_nbr.mu);
    if (!g_nbr.run.launching && !g_nbr.run.operation_in_flight && !g_nbr.run.running) {
        native_buffer_run_hooks_t hooks = g_nbr.hooks;
        memset(&g_nbr.run, 0, sizeof(g_nbr.run));
        g_nbr.hooks = hooks;
        ok = true;
    }
    pthread_mutex_unlock(&g_nbr.mu);
    return ok;
}

bool native_buffer_run_key(int key) {
    if (key == 0x12) return nbr_start();
    if (key == 0x0f) return nbr_collect(false);
    if (key == 0x0b) return nbr_collect(true);
    return false;
}
