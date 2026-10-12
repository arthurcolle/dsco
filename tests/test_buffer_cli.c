/* cc -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude tests/test_buffer_cli.c
 * src/buffer_cli.c src/json_fast.c -o build/test_buffer_cli */
#include "buffer_cli.h"
#include "../vendor/yyjson.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int initializations, calls, checks;
static char last_input[65537], last_tool[32], last_tier[32];
static bool tool_ok = true;
static const char *tool_result = "{\"ok\":true,\n\"content\":\"line one\\nline two\"}";
static const char *expected_detail;

void tools_init_local_only(void) { ++initializations; }
bool tools_execute_for_tier(const char *name, const char *input, const char *tier,
                            char *result, size_t cap) {
    ++calls;
    snprintf(last_tool, sizeof(last_tool), "%s", name);
    snprintf(last_tier, sizeof(last_tier), "%s", tier);
    snprintf(last_input, sizeof(last_input), "%s", input);
    snprintf(result, cap, "%s", tool_result);
    return tool_ok;
}

static void verify_request(const char *tool, const char *action, const char *tier) {
    assert(!strcmp(last_tool, tool) && !strcmp(last_tier, tier));
    yyjson_doc *doc = yyjson_read(last_input, strlen(last_input), 0);
    assert(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_equals_str(yyjson_obj_get(root, "action"), action));
    size_t i, max; yyjson_val *key, *value;
    int count = 0;
    yyjson_obj_foreach(root, i, max, key, value) {
        (void)value;
        if (yyjson_equals_str(key, "action")) count++;
    }
    assert(count == 1);
    yyjson_doc_free(doc);
}

static void verify_field(const char *key, const char *value) {
    yyjson_doc *doc = yyjson_read(last_input, strlen(last_input), 0);
    assert(doc && yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), key), value));
    yyjson_doc_free(doc);
}

static void verify_output(const char *output, size_t n) {
    yyjson_doc *doc = yyjson_read(output, n, 0);
    assert(doc && yyjson_is_obj(yyjson_doc_get_root(doc)));
    if (expected_detail) {
        assert(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "error"), "buffer_failed"));
        assert(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "detail"), expected_detail));
    }
    yyjson_doc_free(doc);
}

static void run_cli(int argc, char **argv, int expected_status, const char *tool, const char *action) {
    FILE *capture = tmpfile(); assert(capture);
    assert(fflush(stdout) == 0);
    int saved_stdout = dup(STDOUT_FILENO); assert(saved_stdout >= 0);
    assert(dup2(fileno(capture), STDOUT_FILENO) == STDOUT_FILENO);
    int before = calls, before_init = initializations;
    int status = buffer_cli(argc, argv);
    assert(fflush(stdout) == 0);
    assert(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
    close(saved_stdout);
    assert(status == expected_status);
    assert(calls - before == (tool ? 1 : 0));
    assert(initializations - before_init == (tool ? 1 : 0));
    rewind(capture);
    char output[16384];
    size_t n = fread(output, 1, sizeof(output) - 1, capture);
    output[n] = '\0'; fclose(capture);
    assert(n > 1 && output[n - 1] == '\n' && !memchr(output, '\n', n - 1));
    verify_output(output, n);
    if (tool) verify_request(tool, action, "trusted");
    checks++;
}

static void run_slash(const char *tail, bool success, const char *tool, const char *action) {
    FILE *capture = tmpfile(); assert(capture);
    assert(fflush(stdout) == 0);
    int saved_stdout = dup(STDOUT_FILENO); assert(saved_stdout >= 0);
    assert(dup2(fileno(capture), STDOUT_FILENO) == STDOUT_FILENO);
    int before = calls, before_init = initializations;
    char output[16384];
    assert(buffer_command_execute(tail, "untrusted", output, sizeof(output)) == success);
    assert(fflush(stdout) == 0);
    assert(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
    close(saved_stdout);
    assert(ftell(capture) == 0); fclose(capture);
    assert(initializations == before_init);
    assert(calls - before == (tool ? 1 : 0));
    assert(strchr(output, '\n') == NULL);
    verify_output(output, strlen(output));
    if (tool) verify_request(tool, action, "untrusted");
    checks++;
}

int main(void) {
    char *help[] = {"dsco", "buffer", "--help"};
    run_cli(2, help, 0, NULL, NULL);
    run_cli(3, help, 0, NULL, NULL);
    char *args[] = {"dsco", "buffer", "list", "{}", "extra", "extra"};
    const char *storage[] = {"create", "list", "inspect", "read", "write", "append", "rename", "fork", "close", "reopen", "save"};
    for (size_t i = 0; i < sizeof(storage) / sizeof(storage[0]); i++) {
        args[2] = (char *)storage[i];
        run_cli(4, args, 0, "buffer", storage[i]);
    }
    const char *views[] = {"open", "view", "views", "focus", "resize", "layout", "detach", "close-view"};
    const char *canonical[] = {"open", "open", "list", "focus", "resize", "layout", "detach", "close"};
    for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
        args[2] = (char *)views[i];
        run_cli(4, args, 0, "buffer_view", canonical[i]);
        if (!strcmp(views[i], "view")) verify_field("mode", "view");
    }
    args[2] = "new"; args[3] = "雪 notes";
    run_cli(4, args, 0, "buffer", "create");
    verify_field("name", "雪 notes"); verify_field("kind", "scratch");
    args[2] = "open";
    run_cli(4, args, 0, "buffer_view", "open"); verify_field("name", "雪 notes");
    args[2] = "edit";
    run_cli(4, args, 0, "native_window", "buffer"); verify_field("name", "雪 notes");
    args[2] = "read";
    run_cli(4, args, 0, "buffer", "read"); verify_field("name", "雪 notes");
    args[2] = "append"; args[4] = "literal $(echo secret) `id` $HOME \"雪\"\nnext";
    run_cli(5, args, 0, "buffer", "append");
    verify_field("name", args[3]); verify_field("content", args[4]);
    args[4] = "";
    run_cli(5, args, 0, "buffer", "append"); verify_field("content", "");

    args[2] = "create";
    const char *invalid[] = {"{", "[]", "{\"action\":\"close\"}",
        "{\"action\":\"create\",\"action\":\"create\"}", "{\"name\":\"a\",\"name\":\"b\"}",
        "{\"action\":\"create\\u0000\"}", "{\"action\\u0000ignored\":\"close\"}",
        "{\"action\":null}", "{\"name\":\"a\"} trailing"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        args[3] = (char *)invalid[i]; run_cli(4, args, 2, NULL, NULL);
    }
    args[3] = "{\"action\":\"create\",\"name\":\"雪\\nnotes\"}";
    run_cli(4, args, 0, "buffer", "create"); verify_field("name", "雪\nnotes");
    args[2] = "views"; args[3] = "{\"action\":\"list\"}";
    run_cli(4, args, 0, "buffer_view", "list");
    args[3] = "{\"action\":\"views\"}";
    run_cli(4, args, 2, NULL, NULL);
    args[2] = "view"; args[3] = "{\"mode\":\"edit\"}";
    run_cli(4, args, 2, NULL, NULL);
    args[2] = "unknown"; run_cli(3, args, 2, NULL, NULL);
    args[2] = "read"; args[3] = ""; run_cli(4, args, 2, NULL, NULL);
    args[3] = "name"; run_cli(5, args, 2, NULL, NULL);
    args[2] = "append"; run_cli(4, args, 2, NULL, NULL);
    run_cli(6, args, 2, NULL, NULL);
    char long_name[162]; memset(long_name, 'x', 161); long_name[161] = '\0';
    args[2] = "new"; args[3] = long_name; run_cli(4, args, 2, NULL, NULL);
    char *huge = malloc(65538); assert(huge); memset(huge, 'x', 65537); huge[65537] = '\0';
    args[2] = "append"; args[3] = "notes"; args[4] = huge; run_cli(5, args, 2, NULL, NULL);
    free(huge);
    args[2] = "list";
    tool_ok = false; tool_result = "{\"ok\":false,\"error\":\"denied\"}";
    run_cli(3, args, 1, "buffer", "list");
    tool_result = "capability exec denied: DSCO_ALLOW_RUN=0\noperator grant remains disabled";
    expected_detail = tool_result;
    run_cli(3, args, 1, "buffer", "list");
    run_slash("open notes", false, "buffer_view", "open");
    tool_ok = true; tool_result = "not JSON"; expected_detail = tool_result;
    run_cli(3, args, 1, "buffer", "list");
    expected_detail = NULL; tool_result = "{\"ok\":true}";

    run_slash(NULL, true, NULL, NULL);
    run_slash("  ", true, NULL, NULL);
    run_slash("help", true, NULL, NULL);
    run_slash("list", true, "buffer", "list");
    run_slash("edit 'snow notes'", true, "native_window", "buffer");
    verify_field("name", "snow notes");
    run_slash("edit {\"workspace\":\"writing\",\"buffer_id\":\"owned-id\"}", true, "native_window", "buffer");
    verify_field("workspace", "writing");verify_field("buffer_id", "owned-id");
    run_slash("new 'snow notes'", true, "buffer", "create"); verify_field("name", "snow notes");
    run_slash("read snow\\ notes", true, "buffer", "read"); verify_field("name", "snow notes");
    run_slash("append \"snow notes\" 'literal $(id) `id` $HOME'", true, "buffer", "append");
    verify_field("content", "literal $(id) `id` $HOME");
    run_slash("append notes \"\"", true, "buffer", "append"); verify_field("content", "");
    run_slash("create { \"name\": \"snow notes\", \"content\": \"a \\\"quote\\\"\" }", true, "buffer", "create");
    verify_field("content", "a \"quote\"");
    run_slash("view '{\"name\":\"snow notes\"}'", true, "buffer_view", "open");
    verify_field("mode", "view"); verify_field("name", "snow notes");
    run_slash("new 'unterminated", false, NULL, NULL);
    run_slash("new dangling\\", false, NULL, NULL);
    run_slash("append notes too many words", false, NULL, NULL);
    run_slash("create {\"name\":\"a\",\"name\":\"b\"}", false, NULL, NULL);
    char small[64]; int before = calls;
    assert(!buffer_command_execute("list", NULL, small, sizeof(small)) && calls == before);
    assert(!buffer_command_execute("help", "trusted", small, sizeof(small)));
    verify_output(small, strlen(small));
    assert(!buffer_command_execute("list", "trusted", NULL, 0));
    checks += 3;
    printf("buffer CLI: %d parser/dispatch/output checks passed\n", checks);
    return 0;
}
