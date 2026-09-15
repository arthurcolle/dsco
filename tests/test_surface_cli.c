/* Compile this test with src/surface_cli.c and src/json_fast.c. Stubs exercise
 * parser/dispatch/output contracts without starting a workspace or any UI. */
#include "surface_cli.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int initializations, calls, checks;
static char last_input[65536];
static bool tool_ok = true;
static const char *tool_result = "{\"ok\":true,\n\"output\":\"line one\\nline two\"}";
static const char *expected_detail;

void tools_init_local_only(void) { ++initializations; }
bool tools_execute_for_tier(const char *name, const char *input, const char *tier,
                            char *result, size_t cap) {
    assert(!strcmp(name, "surface") && !strcmp(tier, "trusted"));
    ++calls;
    snprintf(last_input, sizeof(last_input), "%s", input);
    snprintf(result, cap, "%s", tool_result);
    return tool_ok;
}

static void run_case(int argc, char **argv, int expected_status, const char *action) {
    FILE *capture = tmpfile();
    assert(capture);
    assert(fflush(stdout) == 0);
    int original_stdout = dup(STDOUT_FILENO);
    assert(original_stdout >= 0);
    assert(dup2(fileno(capture), STDOUT_FILENO) == STDOUT_FILENO);
    int before = calls;
    int before_init = initializations;
    int status = surface_cli(argc, argv);
    assert(fflush(stdout) == 0);
    assert(dup2(original_stdout, STDOUT_FILENO) == STDOUT_FILENO);
    close(original_stdout);
    assert(status == expected_status);
    assert(calls - before == (action ? 1 : 0));
    assert(initializations - before_init == (action ? 1 : 0));
    rewind(capture);
    char output[8192];
    size_t n = fread(output, 1, sizeof(output) - 1, capture);
    output[n] = '\0';
    fclose(capture);
    assert(n > 1 && output[n - 1] == '\n');
    assert(memchr(output, '\n', n - 1) == NULL); /* Exactly one physical JSON line. */
    yyjson_doc *doc = yyjson_read(output, n, 0);
    assert(doc && yyjson_is_obj(yyjson_doc_get_root(doc)));
    if (expected_detail) {
        assert(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "error"), "surface_failed"));
        assert(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "detail"), expected_detail));
    }
    yyjson_doc_free(doc);
    if (action) {
        doc = yyjson_read(last_input, strlen(last_input), 0);
        assert(doc);
        yyjson_val *root = yyjson_doc_get_root(doc);
        assert(yyjson_equals_str(yyjson_obj_get(root, "action"), action));
        size_t i, max;
        yyjson_val *key, *value;
        int action_count = 0;
        yyjson_obj_foreach(root, i, max, key, value) {
            (void)value;
            if (yyjson_equals_str(key, "action")) ++action_count;
        }
        assert(action_count == 1);
        yyjson_doc_free(doc);
    }
    ++checks;
}

int main(void) {
    char *help[] = {"dsco", "surface", "--help"};
    run_case(3, help, 0, NULL);
    run_case(2, help, 0, NULL);
    char *list[] = {"dsco", "surface", "list"};
    run_case(3, list, 0, "list");
    char *start[] = {"dsco", "surface", "start", "{\"visible\":true}"};
    run_case(4, start, 0, "start");
    const char *invalid[] = {"{", "[]", "null", "{\"action\":\"close\"}",
        "{\"action\":\"start\",\"action\":\"start\"}", "{\"visible\":true,\"visible\":false}",
        "{\"action\":\"start\\u0000\"}", "{\"action\\u0000ignored\":\"close\"}",
        "{\"action\":null}", "{\"visible\":true} trailing"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        start[3] = (char *)invalid[i];
        run_case(4, start, 2, NULL);
    }
    start[3] = "{\"action\":\"start\",\"title\":\"雪 \\n\",\"visible\":true}";
    run_case(4, start, 0, "start");
    char *unknown[] = {"dsco", "surface", "not-an-action"};
    run_case(3, unknown, 2, NULL);
    char *extra[] = {"dsco", "surface", "list", "{}", "extra"};
    run_case(5, extra, 2, NULL);
    tool_ok = false;
    tool_result = "{\"ok\":false,\"error\":\"denied\"}";
    run_case(3, list, 1, "list");
    tool_result = "capability exec denied: DSCO_ALLOW_RUN=0\noperator grant remains disabled";
    expected_detail = tool_result;
    run_case(3, list, 1, "list");
    tool_ok = true;
    tool_result = "not JSON";
    expected_detail = tool_result;
    run_case(3, list, 1, "list");
    printf("surface CLI: %d parser/dispatch/output checks passed\n", checks);
    return 0;
}
