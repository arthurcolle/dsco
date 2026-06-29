#include "config.h"
#include "wasm_core.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect_contains(const char *label, const char *s, const char *needle) {
    if (!s || !strstr(s, needle)) {
        fprintf(stderr, "FAIL %s: missing %s in %s\n", label, needle, s ? s : "(null)");
        failures++;
    }
}

static void expect_not_contains(const char *label, const char *s, const char *needle) {
    if (s && strstr(s, needle)) {
        fprintf(stderr, "FAIL %s: unexpected %s in %s\n", label, needle, s);
        failures++;
    }
}

int main(void) {
    const char *version = dsco_wasm_version();
    expect_contains("version", version, "\"name\":\"dsco-wasm\"");
    expect_contains("version default", version, DEFAULT_MODEL);

    const char *exports = dsco_wasm_exports_json();
    expect_contains("exports", exports, "dsco_wasm_tool_exec");
    expect_contains("exports", exports, "dsco_wasm_session_state");

    const char *models = dsco_wasm_models_json();
    expect_contains("models", models, DEFAULT_MODEL);
    expect_contains("models", models, "\"alias\":\"kimi\"");

    const char *tools = dsco_wasm_tools_json();
    expect_contains("tools", tools, "\"name\":\"route_explain\"");
    expect_contains("tools", tools, "\"name\":\"echo\"");

    const char *route = dsco_wasm_route_explain("kimi");
    expect_contains("route", route, "\"provider\":\"moonshot\"");
    expect_contains("route", route, "\"runtime\":\"browser-wasm\"");

    const char *echo = dsco_wasm_tool_exec("echo", "{\"text\":\"hello\\nworld\"}");
    expect_contains("echo", echo, "hello\\nworld");

    const char *missing = dsco_wasm_tool_exec("echo", "{}");
    expect_contains("missing field", missing, "\"ok\":true");
    expect_not_contains("missing field", missing, "(null)");

    const char *reset = dsco_wasm_session_reset();
    expect_contains("reset", reset, "\"turns\":0");
    const char *add = dsco_wasm_tool_exec("session_add", "{\"content\":\"first\"}");
    expect_contains("session add", add, "\"turns\":1");
    const char *state = dsco_wasm_session_state();
    expect_contains("session state", state, "user: first");

    const char *unknown = dsco_wasm_tool_exec("native_shell", "{}");
    expect_contains("unknown", unknown, "\"ok\":false");
    expect_contains("unknown", unknown, "browser wasm core");

    if (failures) {
        fprintf(stderr, "test_wasm_core: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_wasm_core: ok");
    return 0;
}
