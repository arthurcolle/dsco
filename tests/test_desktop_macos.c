/* This executable's default mode never changes focus, window bounds, or OS
 * permissions. --call JSON is an explicit operator probe for an owned fixture. */
#include "desktop_macos.h"
#include "../vendor/yyjson.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

static void check_invalid(const char *input) {
    char out[4096];
    assert(!tool_desktop(input, out, sizeof(out)));
    yyjson_doc *doc = yyjson_read(out, strlen(out), 0);
    assert(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_equals_str(yyjson_obj_get(root, "error"), "invalid_request"));
    assert(yyjson_is_false(yyjson_obj_get(root, "ok")));
    yyjson_doc_free(doc);
    ++checks;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--call")) {
        char *out = calloc(1, 1024 * 1024);
        assert(out);
        bool ok = tool_desktop(argv[2], out, 1024 * 1024);
        puts(out);
        free(out);
        return ok ? 0 : 1;
    }
    assert(argc == 1);
    yyjson_doc *schema = yyjson_read(DESKTOP_SCHEMA, strlen(DESKTOP_SCHEMA), 0);
    assert(schema && yyjson_is_obj(yyjson_doc_get_root(schema)));
    yyjson_doc_free(schema);
    ++checks;

    /* These mutation-shaped inputs must fail validation before entering any
     * OS control path. Reject fractional, coerced, missing, or ambiguous IDs. */
    const char *invalid[] = {
        NULL, "", "{", "[]", "null", "{}", "{\"action\":42}",
        "{\"action\":\"focus\"}",
        "{\"action\":\"focus\",\"window_id\":1}",
        "{\"action\":\"focus\",\"pid\":1}",
        "{\"action\":\"focus\",\"window_id\":0,\"pid\":1}",
        "{\"action\":\"focus\",\"window_id\":4294967296,\"pid\":1}",
        "{\"action\":\"focus\",\"window_id\":1,\"pid\":2147483648}",
        "{\"action\":\"focus\",\"window_id\":1.5,\"pid\":1}",
        "{\"action\":\"focus\",\"window_id\":\"1\",\"pid\":1}",
        "{\"action\":\"focus\",\"window_id\":1,\"pid\":true}",
        "{\"action\":\"focus\\u0000list\",\"window_id\":1,\"pid\":1}",
        "{\"action\":\"focus\",\"window_id\":1,\"window_id\":2,\"pid\":1}",
        "{\"action\":\"status\",\"action\":\"focus\"}",
        "{\"action\":\"status\",\"app_name\":\"ignored typo\"}",
        "{\"action\":\"list\",\"include_titles\":\"true\"}",
        "{\"action\":\"list\",\"limit\":0}",
        "{\"action\":\"list\",\"limit\":257}",
        "{\"action\":\"move\",\"window_id\":1,\"pid\":1,\"x\":0}",
        "{\"action\":\"move\",\"window_id\":1,\"pid\":1,\"x\":100001,\"y\":0}",
        "{\"action\":\"resize\",\"window_id\":1,\"pid\":1,\"width\":0,\"height\":100}",
        "{\"action\":\"resize\",\"window_id\":1,\"pid\":1,\"width\":100,\"height\":-1}",
        "{\"action\":\"set_bounds\",\"window_id\":1,\"pid\":1,\"x\":0,\"y\":0}",
        "{\"action\":\"snapshot\",\"window_id\":1,\"pid\":1,\"max_depth\":9}",
        "{\"action\":\"snapshot\",\"window_id\":1,\"pid\":1,\"max_nodes\":257}",
        "{\"action\":\"request_permission\"}",
        "{\"action\":\"request_permission\",\"permission\":\"all\"}",
        "{\"action\":\"request_permission\",\"permission\":\"accessibility\\u0000\"}",
        "{\"action\":\"status\"} trailing"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) check_invalid(invalid[i]);
    char huge[17000];
    memset(huge, ' ', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    check_invalid(huge);

    char out[4096];
    assert(tool_desktop("{\"action\":\"status\"}", out, sizeof(out)));
    yyjson_doc *doc = yyjson_read(out, strlen(out), 0);
    assert(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_is_true(yyjson_obj_get(root, "ok")));
    assert(yyjson_is_false(yyjson_obj_get(root, "permission_prompted")));
#if defined(__APPLE__) && !defined(DSCO_DESKTOP_NO_MACOS)
    assert(yyjson_is_true(yyjson_obj_get(root, "supported")));
    assert(yyjson_is_bool(yyjson_obj_get(root, "accessibility_trusted")));
    assert(yyjson_is_bool(yyjson_obj_get(root, "screen_recording_allowed")));
    assert(yyjson_is_bool(yyjson_obj_get(root, "display_inventory_available")));
#else
    assert(yyjson_is_false(yyjson_obj_get(root, "supported")));
#endif
    yyjson_doc_free(doc);
    ++checks;

    /* Never emit truncated JSON if a caller cannot hold the result. */
    char tiny[2] = {'X', 'X'};
    assert(!tool_desktop("{\"action\":\"status\"}", tiny, sizeof(tiny)));
    assert(tiny[0] == '\0');
    assert(!tool_desktop("{\"action\":\"status\"}", NULL, 0));
    ++checks;
#if !defined(__APPLE__) || defined(DSCO_DESKTOP_NO_MACOS)
    assert(!tool_desktop("{\"action\":\"focus\",\"window_id\":1,\"pid\":1}", out, sizeof(out)));
    assert(strstr(out, "unsupported_platform"));
    ++checks;
#endif
    printf("desktop adapter: %u checks passed (validation and readiness; no UI mutation)\n", checks);
    return 0;
}
