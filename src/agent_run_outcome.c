#include "agent_run_outcome.h"
#include "../vendor/yyjson.h"
#include <string.h>

static bool eq(yyjson_val *object, const char *key, const char *value) {
    const char *s = yyjson_get_str(yyjson_obj_get(object, key));
    return s && !strcmp(s, value);
}

agent_run_outcome_t agent_run_outcome(const char *adapter,
                                     const process_capture_t *capture) {
    bool codex = adapter && !strcmp(adapter, "codex");
    bool claude = adapter && !strcmp(adapter, "claude-code");
    agent_run_outcome_t result = {.state = AGENT_RUN_UNSUPPORTED};
    if (!codex && !claude) return result;
    result.state = AGENT_RUN_UNKNOWN;
    if (!capture || !capture->output) return result;
    bool completed = false, failed = false;
    for (size_t offset = 0; offset < capture->length;) {
        const char *start = capture->output + offset;
        const char *newline = memchr(start, '\n', capture->length - offset);
        size_t length = newline ? (size_t)(newline - start) : capture->length - offset;
        size_t next = offset + length + (newline != NULL);
        /* Parse exact bytes, not substrings; NULs/trailing junk fail parsing.
         * Merged stderr that is not JSON makes positive completion uncertain. */
        if (length && !(length == 1 && *start == '\r')) {
            yyjson_doc *doc = yyjson_read(start, length, 0);
            yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
            if (!yyjson_is_obj(root)) result.stream_invalid = true;
            else {
                bool failure = false;
                if (codex) {
                    yyjson_val *error = yyjson_obj_get(root, "error");
                    failure = (eq(root, "type", "turn.failed") &&
                               yyjson_is_obj(error) &&
                               yyjson_is_str(yyjson_obj_get(error, "message"))) ||
                              (eq(root, "type", "error") &&
                               yyjson_is_str(yyjson_obj_get(root, "message")));
                    if (eq(root, "type", "turn.started")) completed = false;
                    if (eq(root, "type", "turn.completed") &&
                        yyjson_is_obj(yyjson_obj_get(root, "usage"))) completed = true;
                } else if (eq(root, "type", "result")) {
                    yyjson_val *is_error = yyjson_obj_get(root, "is_error");
                    failure = yyjson_is_bool(is_error) && yyjson_get_bool(is_error);
                    completed = yyjson_is_bool(is_error) && !yyjson_get_bool(is_error) &&
                                eq(root, "subtype", "success") &&
                                !eq(root, "terminal_reason", "aborted_streaming") &&
                                !eq(root, "terminal_reason", "aborted_tools");
                }
                if (failure && !failed) {
                    result.failure_offset = offset; result.failure_length = length;
                }
                failed |= failure;
            }
            yyjson_doc_free(doc);
        }
        offset = next;
    }
    /* Positive completion requires an entire stream and clean process lifecycle.
     * Explicit failure stays visible even if later output is missing/truncated. */
    if (failed) result.state = AGENT_RUN_FAILED;
    else if (completed && !result.stream_invalid && !capture->truncated &&
             !capture->cancelled && !capture->timed_out && !capture->spawn_error &&
             capture->cleanup_complete && capture->output_complete &&
             capture->exit_code == 0) result.state = AGENT_RUN_COMPLETED;
    return result;
}

const char *agent_run_state_name(agent_run_state_t state) {
    switch (state) {
    case AGENT_RUN_UNSUPPORTED: return "unknown";
    case AGENT_RUN_COMPLETED: return "completed";
    case AGENT_RUN_FAILED: return "failed";
    default: return "unknown";
    }
}
