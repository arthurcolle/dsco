#include "agent_run_outcome.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int checks;
static void check(const char *adapter, const char *text, agent_run_state_t expected) {
    process_capture_t capture = {.output = (char *)text, .length = strlen(text),
        .exit_code = 0, .root_reaped = true, .group_gone = true,
        .cleanup_complete = true, .output_complete = true};
    agent_run_outcome_t result = agent_run_outcome(adapter, &capture);
    assert(result.state == expected); checks++;
    if (expected == AGENT_RUN_FAILED) { assert(result.failure_length); checks++; }
}
int main(void) {
    const char *ok = "{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":2,\"cached_input_tokens\":0,\"output_tokens\":1}}\n";
    const char *failed = "{\"type\":\"turn.failed\",\"error\":{\"message\":\"fixture quota exhausted\"}}\n";
    check("codex", ok, AGENT_RUN_COMPLETED);
    check("codex", failed, AGENT_RUN_FAILED);
    check("codex", "{\"type\":\"error\",\"message\":\"fixture auth expired\"}\n", AGENT_RUN_FAILED);
    check("claude-code", "{\"type\":\"result\",\"subtype\":\"error_during_execution\",\"is_error\":true,\"errors\":[\"fixture tool denied\"]}\n", AGENT_RUN_FAILED);
    check("claude-code", "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":true}\n", AGENT_RUN_FAILED);
    check("claude-code", "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false}\n", AGENT_RUN_COMPLETED);
    check("claude-code", "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":\"false\"}\n", AGENT_RUN_UNKNOWN);
    check("claude-code", "{\"type\":\"result\",\"subtype\":\"success\"}\n", AGENT_RUN_UNKNOWN);
    check("claude-code", "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"terminal_reason\":\"aborted_tools\"}\n", AGENT_RUN_UNKNOWN);
    check("codex", "{\"type\":\"turn.failed\",\"error\":\"text\"}\n", AGENT_RUN_UNKNOWN);
    check("codex", "{\"type\":\"turn.completed\"}\n", AGENT_RUN_UNKNOWN);
    check("codex", "{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\",\"text\":\"turn.completed success\"}}\n", AGENT_RUN_UNKNOWN);
    check("codex", "{\"type\":\"item.completed\",\"item\":{\"type\":\"error\",\"message\":\"nonfatal\"}}\n", AGENT_RUN_UNKNOWN);
    check("codex", "provider success\n", AGENT_RUN_UNKNOWN);
    check("generic", failed, AGENT_RUN_UNSUPPORTED);
    char combined[512]; snprintf(combined, sizeof(combined), "%s%s", failed, ok);
    check("codex", combined, AGENT_RUN_FAILED);
    snprintf(combined, sizeof(combined), "%s{\"type\":\"turn.started\"}\n", ok);
    check("codex", combined, AGENT_RUN_UNKNOWN);
    snprintf(combined, sizeof(combined), "stderr fixture\n%s", ok);
    check("codex", combined, AGENT_RUN_UNKNOWN);
    process_capture_t capture = {.output = (char *)ok, .length = strlen(ok),
        .exit_code = 0, .cleanup_complete = true, .output_complete = true};
    capture.truncated = true;
    assert(agent_run_outcome("codex", &capture).state == AGENT_RUN_UNKNOWN); checks++;
    capture.truncated = false; capture.cancelled = true;
    assert(agent_run_outcome("codex", &capture).state == AGENT_RUN_UNKNOWN); checks++;
    capture.cancelled = false; capture.cleanup_complete = false;
    assert(agent_run_outcome("codex", &capture).state == AGENT_RUN_UNKNOWN); checks++;
    capture.output = (char *)failed; capture.length = strlen(failed); capture.truncated = true;
    assert(agent_run_outcome("codex", &capture).state == AGENT_RUN_FAILED); checks++;
    assert(!strcmp(agent_run_state_name(AGENT_RUN_UNKNOWN), "unknown")); checks++;
    printf("agent run outcome: %d checks passed\n", checks);
}
