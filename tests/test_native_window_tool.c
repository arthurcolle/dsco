/* Standalone adapter contract: real retained model, stubbed renderer and
 * governed buffer transport. No terminal, files, providers or keychains. */
#include "native_windows.h"
#include "pixel_tui.h"
#include "tools.h"
#include "../vendor/yyjson.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned checks;
static bool active, end_session_during_read;
static uint64_t close_during_read;
static int nested_calls, redraws, bypass_calls;
static const char *caller_tier = "untrusted";
static char last_query[16384], last_tier[32], result[131072];
static const char *const BUFFER_ID = "11111111-1111-4111-8111-111111111111";
static const char *const OTHER_ID = "22222222-2222-4222-8222-222222222222";
static const char *const SOURCE_NAME = "Owned buffer";
static char source_text[8192] = "Version one 雪";
static char source_revision[65];
static bool source_sensitive = true;
enum { REPLY_OK, REPLY_DENIED, REPLY_NOT_JSON, REPLY_FALSE, REPLY_NO_REVISION,
       REPLY_BAD_ID, REPLY_WRONG_ID, REPLY_BAD_SENSITIVE, REPLY_NUL_TEXT,
       REPLY_NUL_ID, REPLY_NUL_REVISION, REPLY_NUL_NAME, REPLY_TRUNCATED };
static int reply_mode;

static void check(bool pass, const char *message) {
    checks++;
    if (!pass) { fprintf(stderr, "FAIL %u: %s\n%s\n", checks, message, result); exit(1); }
}
static const char *field(yyjson_val *obj, const char *key) { return yyjson_get_str(yyjson_obj_get(obj, key)); }
static bool equal(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static native_windows_snapshot_t snapshot(void) { native_windows_snapshot_t s; native_windows_snapshot(&s); return s; }
static void unchanged(native_windows_snapshot_t old) {
    native_windows_snapshot_t now = snapshot();
    check(!memcmp(&old, &now, sizeof(old)), "failed/observational adapter call preserves entire model");
}
bool pixel_tui_session_active(void) { return active; }
void pixel_tui_session_windows_changed(FILE *out) {
    check(out == stderr, "adapter schedules redraw on renderer stream");
    check(active, "inactive compositor is not asked to redraw");
    /* Reentrant observation proves model mutex is released before callbacks. */
    native_windows_snapshot_t snap = snapshot();
    check(snap.count >= 0 && snap.count <= NATIVE_WINDOWS_MAX, "redraw sees completed model mutation");
    redraws++;
}
const char *tools_execution_tier(void) { return caller_tier; }
bool tools_execute(const char *name, const char *input, char *out, size_t cap) {
    (void)name; (void)input; (void)out; (void)cap; bypass_calls++; return false;
}
bool tool_buffer(const char *input, char *out, size_t cap) {
    (void)input; (void)out; (void)cap; bypass_calls++; return false;
}
bool tools_execute_for_tier(const char *name, const char *input, const char *tier, char *out, size_t cap) {
    nested_calls++;
    check(equal(name, "buffer") && equal(tier, caller_tier), "nested buffer read preserves exact caller tier");
    snprintf(last_query, sizeof(last_query), "%s", input);
    snprintf(last_tier, sizeof(last_tier), "%s", tier);
    native_windows_snapshot_t snap = snapshot();
    check(snap.count >= 0, "governed callback is outside model mutex");
    yyjson_doc *query = yyjson_read(input, strlen(input), 0);
    yyjson_val *q = yyjson_doc_get_root(query);
    check(query && yyjson_is_obj(q) && equal(field(q, "action"), "read"), "adapter invokes a read-only buffer action");
    check(yyjson_get_uint(yyjson_obj_get(q, "max_bytes")) == NATIVE_WINDOW_TEXT_CAP - 1,
          "nested read is bounded to native snapshot capacity");
    const char *id = field(q, "buffer_id"), *selected_name = field(q, "name");
    bool valid = equal(field(q, "workspace"), "fixture") && (id || selected_name) &&
        (!id || equal(id, BUFFER_ID)) && (!selected_name || equal(selected_name, SOURCE_NAME));
    yyjson_doc_free(query);
    if (!valid) { snprintf(out, cap, "{\"ok\":false,\"error\":\"selector_conflict\"}"); return false; }
    if (end_session_during_read) active = false;
    if (close_during_read) {
        char close_query[128], close_result[1024];
        snprintf(close_query, sizeof(close_query), "{\"action\":\"close\",\"id\":%" PRIu64 "}", close_during_read);
        check(native_windows_command(close_query, close_result, sizeof(close_result)), "fixture user closes panel during governed read");
    }
    if (reply_mode == REPLY_DENIED) { snprintf(out, cap, "{\"error\":\"governance_block\",\"tool\":\"buffer\"}"); return false; }
    if (reply_mode == REPLY_NOT_JSON) { snprintf(out, cap, "not JSON"); return true; }
    if (reply_mode == REPLY_NUL_TEXT) {
        snprintf(out, cap, "{\"ok\":true,\"text\":\"prefix\\u0000suffix\",\"buffer\":{\"buffer_id\":\"%s\",\"name\":\"Owned buffer\",\"revision\":\"%s\",\"sensitive\":true}}", BUFFER_ID, source_revision);
        return true;
    }
    if (reply_mode == REPLY_NUL_ID || reply_mode == REPLY_NUL_REVISION || reply_mode == REPLY_NUL_NAME) {
        snprintf(out, cap, "{\"ok\":true,\"text\":\"Owned snapshot\",\"buffer\":{\"buffer_id\":\"%s%s\",\"name\":\"Owned buffer%s\",\"revision\":\"%s%s\",\"sensitive\":true}}",
            BUFFER_ID, reply_mode == REPLY_NUL_ID ? "\\u0000suffix" : "", reply_mode == REPLY_NUL_NAME ? "\\u0000suffix" : "",
            source_revision, reply_mode == REPLY_NUL_REVISION ? "\\u0000suffix" : "");
        return true;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d), *meta = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_bool(d, root, "ok", reply_mode != REPLY_FALSE);
    yyjson_mut_obj_add_str(d, root, "text", source_text);
    yyjson_mut_obj_add_bool(d,root,"truncated",reply_mode==REPLY_TRUNCATED);
    yyjson_mut_obj_add_val(d, root, "buffer", meta);
    yyjson_mut_obj_add_str(d, meta, "name", SOURCE_NAME);
    yyjson_mut_obj_add_str(d, meta, "buffer_id", reply_mode == REPLY_BAD_ID ? "invalid-identity" : reply_mode == REPLY_WRONG_ID ? OTHER_ID : BUFFER_ID);
    if (reply_mode != REPLY_NO_REVISION) yyjson_mut_obj_add_str(d, meta, "revision", source_revision);
    if (reply_mode == REPLY_BAD_SENSITIVE) yyjson_mut_obj_add_str(d, meta, "sensitive", "true");
    else yyjson_mut_obj_add_bool(d, meta, "sensitive", source_sensitive);
    char *encoded = yyjson_mut_write(d, 0, NULL);
    check(encoded && strlen(encoded) < cap, "fixture buffer reply fits transport");
    memcpy(out, encoded, strlen(encoded) + 1); free(encoded); yyjson_mut_doc_free(d); return true;
}

static bool call(const char *input) {
    bool ok = tool_native_window(input, result, sizeof(result));
    yyjson_doc *doc = yyjson_read(result, strlen(result), 0);
    check(doc && yyjson_is_obj(yyjson_doc_get_root(doc)), "adapter output is a JSON object");
    check(yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "ok")) == ok, "adapter return agrees with ok field");
    yyjson_doc_free(doc); return ok;
}
static void success(const char *input) { check(call(input), input); }
static void reject(const char *input) { check(!call(input), input); }
static void with_id(const char *action, uint64_t id, bool ok) {
    char input[128]; snprintf(input, sizeof(input), "{\"action\":\"%s\",\"id\":%" PRIu64 "}", action, id);
    if (ok) success(input); else reject(input);
}
static void reset(void) {
    native_windows_reset(); native_windows_set_work_area((native_ui_rect_t){0, 0, 1000, 700});
    active = true; end_session_during_read = false; reply_mode = REPLY_OK;
    close_during_read = 0;
    nested_calls = redraws = bypass_calls = 0; caller_tier = "untrusted";
    strcpy(source_text, "Version one 雪"); memset(source_revision, 'a', 64); source_revision[64] = 0; source_sensitive = true;
}

static void test_active_and_validation_boundary(void) {
    reset(); active = false;
    native_windows_snapshot_t old = snapshot();
    const char *mutations[] = {"{\"action\":\"open\",\"title\":\"No native session\"}",
        "{\"action\":\"show\"}", "{\"action\":\"buffer\",\"name\":\"Owned buffer\",\"workspace\":\"fixture\"}"};
    for (size_t i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++) { reject(mutations[i]); unchanged(old); }
    success("{\"action\":\"list\"}"); unchanged(old);
    success("{\"action\":\"events\"}"); unchanged(old);
    check(!nested_calls && !redraws, "inactive calls never read buffers or redraw; observations remain available");
    active = true;
    const char *invalid[] = {NULL, "[]", "{\"action\":\"bogus\"}",
        "{\"action\":\"buffer\",\"name\":true}", "{\"action\":\"buffer\",\"name\":\"bad\\u0000name\"}",
        "{\"action\":\"buffer\",\"name\":\"Owned buffer\",\"name\":\"duplicate\"}",
        "{\"action\":\"buffer\",\"name\":\"Owned buffer\",\"unexpected\":true}",
        "{\"action\":\"buffer\"}", "{\"action\":\"buffer\",\"name\":\"\"}",
        "{\"action\":\"buffer\",\"id\":1,\"name\":\"Owned buffer\"}", "{\"action\":\"refresh\",\"id\":999}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) { reject(invalid[i]); unchanged(old); }
    check(!nested_calls && !redraws, "malformed/insufficient selectors fail before transport or mutation");
    reject("{\"action\":\"buffer\",\"buffer_id\":\"not-a-uuid\",\"workspace\":\"fixture\"}"); unchanged(old);
    check(!redraws, "backend-rejected selector cannot create a native panel");
}

static void test_governed_snapshot_refresh_and_close(void) {
    reset();
    success("{\"action\":\"buffer\",\"name\":\"Owned buffer\",\"workspace\":\"fixture\",\"title\":\"Pinned snapshot\"}");
    native_windows_snapshot_t snap = snapshot();
    check(snap.count == 1 && nested_calls == 1 && redraws == 1, "buffer opens exactly one panel after one governed read");
    native_window_t pane = snap.windows[0]; uint64_t id = pane.id;
    check(pane.kind == NATIVE_WINDOW_BUFFER && equal(pane.buffer_id, BUFFER_ID) && equal(pane.workspace, "fixture") &&
          equal(pane.title, "Pinned snapshot") && equal(pane.text, source_text) && equal(pane.buffer_revision, source_revision),
          "buffer panel retains exact snapshot identity, text and revision");
    check(pane.sensitive && native_windows_sensitive(), "sensitive buffer metadata is retained by native model");
    check(equal(last_tier, "untrusted"), "adapter never upgrades untrusted caller to ambient/default tier");
    strcpy(source_text, "Version two 🚀"); memset(source_revision, 'b', 64);
    check(equal(snapshot().windows[0].text, "Version one 雪"), "persistent changes do not silently rewrite retained snapshot");
    caller_tier = "standard"; with_id("refresh", id, true);
    snap = snapshot(); pane = snap.windows[0];
    check(snap.count == 1 && pane.id == id && equal(pane.text, source_text) && equal(pane.buffer_revision, source_revision),
          "refresh updates contents and revision without replacing native ID");
    check(equal(pane.title, "Pinned snapshot") && equal(last_tier, "standard"), "refresh preserves panel title and current caller tier");
    yyjson_doc *q = yyjson_read(last_query, strlen(last_query), 0); yyjson_val *r = yyjson_doc_get_root(q);
    check(equal(field(r, "buffer_id"), BUFFER_ID) && !yyjson_obj_get(r, "name") && equal(field(r, "workspace"), "fixture"),
          "refresh reads the originally bound exact buffer ID and workspace");
    yyjson_doc_free(q);
    int reads_before = nested_calls; with_id("close", id, true);
    check(!snapshot().count && nested_calls == reads_before && equal(source_text, "Version two 🚀"),
          "closing native panel does not call or modify persistent buffer backend");
    check(!native_windows_sensitive(), "closed sensitive snapshots are no longer exposed by native model");
    with_id("refresh", id, false); check(nested_calls == reads_before, "stale native ID cannot refresh a closed panel");
    caller_tier = "trusted";
    success("{\"action\":\"buffer\",\"name\":\"Owned buffer\",\"workspace\":\"fixture\"}");
    check(snapshot().windows[0].id > id && equal(snapshot().windows[0].text, "Version two 🚀"),
          "new view can reopen unchanged persistent buffer with a fresh native ID");
    check(equal(last_tier, "trusted") && !bypass_calls, "all caller tiers route exclusively through governed nested execution");
    native_windows_snapshot_t before = snapshot(); int old_redraws = redraws;
    active = false; success("{\"action\":\"list\"}"); success("{\"action\":\"events\"}"); unchanged(before);
    check(redraws == old_redraws && nested_calls == reads_before + 1, "inactive observations neither redraw nor refresh persistent data");
}

static void test_denials_and_malformed_replies(void) {
    reset();
    success("{\"action\":\"buffer\",\"name\":\"Owned buffer\",\"workspace\":\"fixture\"}");
    native_windows_snapshot_t before = snapshot(); uint64_t id = before.windows[0].id;
    int old_redraws = redraws;
    const int modes[] = {REPLY_DENIED, REPLY_NOT_JSON, REPLY_FALSE, REPLY_NO_REVISION,
        REPLY_BAD_ID, REPLY_WRONG_ID, REPLY_BAD_SENSITIVE, REPLY_NUL_TEXT,
        REPLY_NUL_ID, REPLY_NUL_REVISION, REPLY_NUL_NAME, REPLY_TRUNCATED};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        reply_mode = modes[i]; with_id("refresh", id, false); unchanged(before);
        check(redraws == old_redraws, "failed/malformed governed read cannot publish a new snapshot");
    }
    reply_mode = REPLY_OK;
    char input[512]; snprintf(input, sizeof(input), "{\"action\":\"buffer\",\"buffer_id\":\"%s\",\"name\":\"Wrong name\",\"workspace\":\"fixture\"}", BUFFER_ID);
    reject(input); unchanged(before);
    check(redraws == old_redraws, "contradictory dual selectors cannot be silently narrowed to an ID");
    end_session_during_read = true;
    with_id("refresh", id, false); unchanged(before);
    check(!active && redraws == old_redraws, "session ending during governed read prevents publishing to inactive compositor");
    active = true; end_session_during_read = false; close_during_read = id;
    with_id("refresh", id, false);
    check(!snapshot().count && equal(source_text, "Version one 雪"), "close during governed read cannot resurrect panel or alter persistent text");
    check(redraws == old_redraws, "failed stale-panel refresh does not publish a redraw");
}

static void test_small_output_contract(void) {
    reset();
    native_windows_snapshot_t old = snapshot();
    char small[160];
    for (size_t cap = 3; cap < 128; cap++) {
        memset(small, 'X', sizeof(small));
        check(!tool_native_window("{\"action\":\"bogus\"}", small, cap), "invalid request stays rejected at small output capacity");
        check(strlen(small) < cap && small[cap] == 'X', "bounded error output remains terminated and within capacity");
        yyjson_doc *doc = yyjson_read(small, strlen(small), 0);
        check(doc && yyjson_is_obj(yyjson_doc_get_root(doc)), "small output never contains a truncated JSON error");
        yyjson_doc_free(doc);
    }
    unchanged(old);
    bool ok = tool_native_window("{\"action\":\"open\",\"title\":\"Bounded mutation\"}", small, 64);
    yyjson_doc *doc = yyjson_read(small, strlen(small), 0);
    check(doc && yyjson_is_obj(yyjson_doc_get_root(doc)), "small mutation response is valid JSON");
    if (!ok) unchanged(old);
    else check(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "id")) > 0,
               "successful compact mutation response preserves a reviewable native ID");
    yyjson_doc_free(doc);
}

int main(void) {
    alarm(15); /* Bound accidental lock inversions in the reentrant stubs. */
    test_active_and_validation_boundary();
    test_governed_snapshot_refresh_and_close();
    test_denials_and_malformed_replies();
    test_small_output_contract();
    check(!bypass_calls, "no direct store or ambient-tier execution bypass");
    alarm(0); printf("native_window_tool: %u checks passed\n", checks); return 0;
}
