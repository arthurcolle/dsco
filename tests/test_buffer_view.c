/* Standalone: cc -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude
 * tests/test_buffer_view.c src/buffer_view.c src/json_fast.c -o build/test_buffer_view
 * Nested buffer/surface calls are fixtures; no process, file, or UI mutation. */
#include "buffer_view.h"
#include "crypto.h"
#include "../vendor/yyjson.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_A "11111111-1111-4111-8111-111111111111"
#define ID_B "22222222-2222-4222-8222-222222222222"
typedef struct {
    char id[80], request[80], run[96];
    bool exists, closed, pending;
} fixture_view_t;
static fixture_view_t views[8];
static size_t view_count;
static int calls, launches, mutations, failures, checks;
static bool online, launch_pending, deny_nested;
static bool random_fail;
static int random_calls;
static const char *buffer_id, *kind, *path, *case_name;
static bool buffer_closed;
static char last_launch[8192], last_mutation[80], last_action[24], last_mutation_input[8192];
static const char *current_tier = "untrusted";

#define CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", case_name, __LINE__, #condition); } } while (0)

static const char *str(yyjson_val *o, const char *k) {
    const char *value = yyjson_get_str(yyjson_obj_get(o, k));
    return value ? value : "";
}
const char *tools_execution_tier(void) { return current_tier; }
bool crypto_random_bytes(uint8_t *buf, size_t len) {
    random_calls++;
    CHECK(buf != NULL && len == 16);
    if (random_fail) return false;
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(i + random_calls);
    return true;
}

static void response_write(yyjson_mut_doc *doc, char *result, size_t cap) {
    char *json = yyjson_mut_write(doc, 0, NULL);
    CHECK(json != NULL && strlen(json) < cap);
    if (json) snprintf(result, cap, "%s", json);
    free(json); yyjson_mut_doc_free(doc);
}

static void inventory(char *result, size_t cap, const char *only) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(d), *array = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_bool(d, o, "ok", true);
    yyjson_mut_obj_add_bool(d, o, "verified", online);
    yyjson_mut_obj_add_val(d, o, "surfaces", array);
    for (size_t i = 0; i < view_count; i++) {
        fixture_view_t *v = &views[i];
        if (only && strcmp(only, v->id)) continue;
        yyjson_mut_val *item = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, item, "surface_id", v->id);
        yyjson_mut_obj_add_str(d, item, "request_id", v->request);
        yyjson_mut_obj_add_str(d, item, "run_id", v->run);
        yyjson_mut_obj_add_bool(d, item, "exists", v->exists);
        yyjson_mut_obj_add_bool(d, item, "closed", v->closed);
        yyjson_mut_obj_add_bool(d, item, "pending", v->pending);
        yyjson_mut_obj_add_uint(d, item, "columns", 80);
        yyjson_mut_obj_add_uint(d, item, "rows", 24);
        yyjson_mut_arr_add_val(array, item);
    }
    response_write(d, result, cap);
}

static fixture_view_t *add_view(const char *id, const char *owner, const char *mode, const char *request_id) {
    CHECK(view_count < 8);
    fixture_view_t *v = &views[view_count++];
    memset(v, 0, sizeof(*v));
    snprintf(v->id, sizeof(v->id), "%s", id);
    snprintf(v->run, sizeof(v->run), "buffer:%s:%s", owner, mode);
    snprintf(v->request, sizeof(v->request), "%s", request_id);
    v->exists = true;
    return v;
}

bool tools_execute_for_tier(const char *name, const char *input, const char *tier,
                            char *result, size_t cap) {
    calls++;
    CHECK(tier && !strcmp(tier, current_tier));
    if (deny_nested) {
        snprintf(result, cap, "capability exec denied: DSCO_ALLOW_RUN=0");
        return false;
    }
    yyjson_doc *d = yyjson_read(input, strlen(input), 0);
    CHECK(d != NULL);
    yyjson_val *o = yyjson_doc_get_root(d);
    CHECK(!strcmp(str(o, "workspace"), "main"));
    const char *action = str(o, "action");
    if (!strcmp(name, "buffer")) {
        CHECK(!strcmp(action, "inspect"));
        yyjson_mut_doc *r = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(r), *b = yyjson_mut_obj(r);
        yyjson_mut_doc_set_root(r, root);
        yyjson_mut_obj_add_bool(r, root, "ok", true);
        yyjson_mut_obj_add_val(r, root, "buffer", b);
        yyjson_mut_obj_add_str(r, b, "buffer_id", buffer_id);
        yyjson_mut_obj_add_str(r, b, "name", "notes");
        yyjson_mut_obj_add_str(r, b, "kind", kind);
        yyjson_mut_obj_add_str(r, b, "content_path", path);
        yyjson_mut_obj_add_bool(r, b, "closed", buffer_closed);
        response_write(r, result, cap);
    } else {
        CHECK(!strcmp(name, "surface"));
        if (!strcmp(action, "status")) inventory(result, cap, NULL);
        else if (!strcmp(action, "create") || !strcmp(action, "start")) {
            launches++;
            snprintf(last_launch, sizeof(last_launch), "%s", input);
            snprintf(last_action, sizeof(last_action), "%s", action);
            fixture_view_t *v = add_view("view-created", buffer_id, "edit", str(o, "request_id"));
            snprintf(v->run, sizeof(v->run), "%s", str(o, "run_id"));
            v->pending = launch_pending; v->exists = !launch_pending;
            online = true;
            inventory(result, cap, v->id);
        } else {
            mutations++;
            snprintf(last_mutation_input, sizeof(last_mutation_input), "%s", input);
            snprintf(last_action, sizeof(last_action), "%s", action);
            snprintf(last_mutation, sizeof(last_mutation), "%s", str(o, "surface_id"));
            fixture_view_t *v = NULL;
            for (size_t i = 0; i < view_count; i++)
                if (!strcmp(views[i].id, last_mutation)) v = &views[i];
            CHECK(v != NULL);
            if (!v || !v->exists || v->closed || v->pending) {
                snprintf(result, cap, "{\"ok\":false,\"error\":\"surface is not live\"}");
                yyjson_doc_free(d); return false;
            }
            if (!strcmp(action, "close")) { v->closed = true; v->exists = false; }
            inventory(result, cap, v->id);
        }
    }
    yyjson_doc_free(d);
    return true;
}

static void reset(const char *name) {
    case_name = name;
    view_count = 0; calls = launches = mutations = 0;
    online = true; launch_pending = deny_nested = buffer_closed = false;
    random_fail = false; random_calls = 0;
    buffer_id = ID_A; kind = "scratch"; path = "/tmp/owned 雪 buffer;$(id).txt";
    last_launch[0] = last_mutation[0] = last_action[0] = last_mutation_input[0] = '\0';
}

static yyjson_doc *run(const char *input, bool expected_ok, const char *expected_error) {
    char result[32768];
    bool ok = tool_buffer_view(input, result, sizeof(result));
    CHECK(ok == expected_ok);
    yyjson_doc *doc = yyjson_read(result, strlen(result), 0);
    CHECK(doc && yyjson_is_obj(yyjson_doc_get_root(doc)));
    if (expected_error)
        CHECK(!strcmp(str(yyjson_doc_get_root(doc), "error"), expected_error));
    return doc;
}

static void verify_launch(const char *mode) {
    yyjson_doc *d = yyjson_read(last_launch, strlen(last_launch), 0);
    CHECK(d != NULL);
    yyjson_val *o = yyjson_doc_get_root(d), *argv = yyjson_obj_get(o, "args");
    CHECK(!strcmp(str(o, "command"), "/usr/bin/env"));
    size_t count = yyjson_arr_size(argv);
    CHECK(count > 2 && yyjson_equals_str(yyjson_arr_get(argv, count - 1), path));
    CHECK(yyjson_equals_str(yyjson_arr_get(argv, count - 2), "--"));
    bool vi = false, less = false, follow = false, modeline = false, secure = false;
    size_t i, n; yyjson_val *a;
    yyjson_arr_foreach(argv, i, n, a) {
        const char *s = yyjson_get_str(a);
        CHECK(s && strcmp(s, "/bin/sh") && strcmp(s, "-c"));
        vi |= yyjson_equals_str(a, "/usr/bin/vi");
        less |= yyjson_equals_str(a, "/usr/bin/less");
        follow |= yyjson_equals_str(a, "+F");
        modeline |= yyjson_equals_str(a, "set nomodeline");
        secure |= yyjson_equals_str(a, "LESSSECURE=1");
    }
    CHECK(!strcmp(mode, "edit") ? vi && modeline && !less : less && secure && !vi);
    CHECK(follow == !strcmp(mode, "follow"));
    yyjson_doc_free(d);
}

int main(void) {
    const char *invalid[] = {NULL, "{", "[]", "{}", "{\"action\":\"bogus\"}",
        "{\"action\":\"list\",\"command\":\"/bin/sh\"}",
        "{\"action\":\"list\",\"action\":\"list\"}",
        "{\"action\":\"list\",\"name\":\"a\",\"name\":\"b\"}",
        "{\"action\":\"list\\u0000\"}", "{\"action\\u0000ignored\":\"list\"}",
        "{\"action\":\"open\",\"name\":\"a\\u0000b\"}",
        "{\"action\":\"open\",\"mode\":\"shell\"}",
        "{\"action\":\"open\",\"new_view\":1}",
        "{\"action\":\"resize\",\"increment\":1.5}",
        "{\"action\":\"resize\",\"increment\":1001}",
        "{\"action\":\"resize\",\"increment\":-1001}",
        "{\"action\":\"resize\",\"increment\":18446744073709551615}",
        "{\"action\":\"list\",\"request_id\":\"x\"}",
        "{\"action\":\"open\",\"surface_id\":\"view-a\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        reset("invalid arguments");
        yyjson_doc *d = run(invalid[i], false, "invalid_arguments");
        CHECK(calls == 0); yyjson_doc_free(d);
    }
    reset("empty list");
    yyjson_doc *d = run("{\"action\":\"list\"}", true, NULL);
    CHECK(calls == 1 && launches == 0 && yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(d), "views")) == 0);
    yyjson_doc_free(d);
    reset("nested denial preserves reason"); deny_nested = true;
    d = run("{\"action\":\"list\"}", false, "adapter_failed");
    CHECK(strstr(str(yyjson_doc_get_root(d), "detail"), "DSCO_ALLOW_RUN=0") != NULL); yyjson_doc_free(d);

    const char *modes[] = {"edit", "view", "follow"};
    for (int i = 0; i < 3; i++) {
        reset("literal launch argv");
        char input[128]; snprintf(input, sizeof(input), "{\"action\":\"open\",\"name\":\"notes\",\"mode\":\"%s\"}", modes[i]);
        d = run(input, true, NULL); yyjson_doc_free(d);
        CHECK(launches == 1 && !strcmp(last_action, "create")); verify_launch(modes[i]);
    }
    reset("default scratch edit offline startup"); online = false;
    d = run("{\"action\":\"open\",\"name\":\"notes\"}", true, NULL); yyjson_doc_free(d);
    CHECK(!strcmp(last_action, "start")); verify_launch("edit");
    reset("default log follow"); kind = "log";
    d = run("{\"action\":\"open\",\"name\":\"notes\"}", true, NULL); yyjson_doc_free(d); verify_launch("follow");
    reset("owned path validation"); path = "relative/path";
    d = run("{\"action\":\"open\",\"name\":\"notes\"}", false, "invalid_content_path");
    CHECK(!launches); yyjson_doc_free(d);
    reset("closed buffer retains content"); buffer_closed = true;
    d = run("{\"action\":\"open\",\"name\":\"notes\"}", false, "buffer_closed");
    CHECK(!launches); yyjson_doc_free(d);
    reset("selected view identity mismatch"); add_view("view-b", ID_B, "edit", "req-b");
    d = run("{\"action\":\"focus\",\"surface_id\":\"view-b\",\"name\":\"notes\"}", false, "identity_mismatch");
    CHECK(!mutations); yyjson_doc_free(d);
    reset("unregistered view rejected");
    d = run("{\"action\":\"focus\",\"surface_id\":\"unknown\"}", false, "unknown_view");
    CHECK(!mutations && calls == 1); yyjson_doc_free(d);
    reset("list filters buffer identity"); add_view("view-a", ID_A, "edit", "a"); add_view("view-b", ID_B, "view", "b");
    d = run("{\"action\":\"list\",\"name\":\"notes\"}", true, NULL);
    CHECK(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(d), "views")) == 1);
    CHECK(!strcmp(str(yyjson_arr_get_first(yyjson_obj_get(yyjson_doc_get_root(d), "views")), "surface_id"), "view-a"));
    yyjson_doc_free(d);
    reset("request replay"); add_view("view-a", ID_A, "edit", "req-a");
    d = run("{\"action\":\"open\",\"name\":\"notes\",\"request_id\":\"req-a\"}", true, NULL);
    CHECK(!launches && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(d), "reused"))); yyjson_doc_free(d);
    reset("request conflict"); add_view("view-a", ID_A, "view", "req-a");
    d = run("{\"action\":\"open\",\"name\":\"notes\",\"request_id\":\"req-a\"}", false, "request_conflict");
    CHECK(!launches); yyjson_doc_free(d);
    reset("request conflicts across buffers"); add_view("view-b", ID_B, "edit", "req-a");
    d = run("{\"action\":\"open\",\"name\":\"notes\",\"request_id\":\"req-a\"}", false, "request_conflict");
    CHECK(!launches); yyjson_doc_free(d);
    reset("default reuse"); add_view("view-a", ID_A, "edit", "req-a");
    d = run("{\"action\":\"open\",\"name\":\"notes\"}", true, NULL);
    CHECK(!launches && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(d), "reused"))); yyjson_doc_free(d);
    reset("explicit new view"); add_view("view-a", ID_A, "edit", "req-a");
    d = run("{\"action\":\"open\",\"name\":\"notes\",\"new_view\":true}", true, NULL);
    CHECK(launches == 1 && yyjson_is_false(yyjson_obj_get(yyjson_doc_get_root(d), "reused"))); yyjson_doc_free(d);
    reset("new views use unique requests despite identical inventory snapshots");
    char generated_requests[2][80];
    for (int i = 0; i < 2; i++) {
        /* Model concurrent callers that both observed the same empty inventory. */
        view_count = 0;
        d = run("{\"action\":\"open\",\"name\":\"notes\",\"new_view\":true}", true, NULL);
        yyjson_doc_free(d);
        d = yyjson_read(last_launch, strlen(last_launch), 0);
        snprintf(generated_requests[i], sizeof(generated_requests[i]), "%s", str(yyjson_doc_get_root(d), "request_id"));
        CHECK(strlen(generated_requests[i]) == 36 && !strncmp(generated_requests[i], "new-", 4));
        yyjson_doc_free(d);
    }
    CHECK(launches == 2 && random_calls == 2 && strcmp(generated_requests[0], generated_requests[1]));
    reset("new view refuses unavailable entropy"); random_fail = true;
    d = run("{\"action\":\"open\",\"name\":\"notes\",\"new_view\":true}", false, "random_unavailable");
    CHECK(random_calls == 1 && !launches && !mutations); yyjson_doc_free(d);
    reset("caller supplied request identity needs no random replacement"); random_fail = true;
    d = run("{\"action\":\"open\",\"name\":\"notes\",\"new_view\":true,\"request_id\":\"explicit-request\"}", true, NULL);
    CHECK(launches == 1 && !random_calls); yyjson_doc_free(d);
    d = yyjson_read(last_launch, strlen(last_launch), 0);
    CHECK(!strcmp(str(yyjson_doc_get_root(d), "request_id"), "explicit-request")); yyjson_doc_free(d);
    reset("multiple live views require exact target"); add_view("view-a", ID_A, "edit", "a"); add_view("view-b", ID_A, "view", "b");
    d = run("{\"action\":\"focus\",\"name\":\"notes\"}", false, "ambiguous_view");
    CHECK(!mutations); yyjson_doc_free(d);
    d = run("{\"action\":\"focus\",\"name\":\"notes\",\"surface_id\":\"view-b\"}", true, NULL);
    CHECK(mutations == 1 && !strcmp(last_mutation, "view-b")); yyjson_doc_free(d);
    d = run("{\"action\":\"close\",\"surface_id\":\"view-a\"}", true, NULL);
    CHECK(!strcmp(last_mutation, "view-a") && !buffer_closed); yyjson_doc_free(d);
    const char *actions[] = {"resize", "layout", "detach"};
    for (int i = 0; i < 3; i++) {
        reset("delegated view mutation arguments"); add_view("view-a", ID_A, "edit", "a");
        char input[256];
        snprintf(input, sizeof(input), "{\"action\":\"%s\",\"surface_id\":\"view-a\"%s}", actions[i],
            i == 0 ? ",\"axis\":\"horizontal\",\"increment\":5" : i == 1 ? ",\"layout\":\"grid\"" : "");
        d = run(input, true, NULL); yyjson_doc_free(d);
        CHECK(mutations == 1 && !strcmp(last_action, actions[i]) && !strcmp(last_mutation, "view-a"));
        d = yyjson_read(last_mutation_input, strlen(last_mutation_input), 0);
        if (i == 0) {
            CHECK(!strcmp(str(yyjson_doc_get_root(d), "axis"), "horizontal"));
            CHECK(yyjson_get_int(yyjson_obj_get(yyjson_doc_get_root(d), "increment")) == 5);
        }
        if (i == 1) CHECK(!strcmp(str(yyjson_doc_get_root(d), "layout"), "grid"));
        yyjson_doc_free(d);
    }
    reset("focus after create exact surface");
    d = run("{\"action\":\"open\",\"name\":\"notes\",\"focus\":true}", true, NULL);
    CHECK(launches == 1 && mutations == 1 && !strcmp(last_mutation, "view-created")); yyjson_doc_free(d);

    for (int state = 0; state < 3; state++) {
        reset(state == 0 ? "closed request replay not verified" : state == 1 ? "pending reuse not verified" : "missing request replay not verified");
        fixture_view_t *v = add_view("view-a", ID_A, "edit", "req-a");
        v->exists = false; v->closed = state == 0; v->pending = state == 1;
        d = run("{\"action\":\"open\",\"name\":\"notes\",\"request_id\":\"req-a\"}", true, NULL);
        CHECK(!launches);
        CHECK(yyjson_is_false(yyjson_obj_get(yyjson_doc_get_root(d), "verified")));
        yyjson_doc_free(d);
    }
    reset("new pending launch not verified"); launch_pending = true;
    d = run("{\"action\":\"open\",\"name\":\"notes\"}", true, NULL);
    CHECK(launches == 1 && yyjson_is_false(yyjson_obj_get(yyjson_doc_get_root(d), "verified")));
    yyjson_doc_free(d);
    printf("buffer view: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
