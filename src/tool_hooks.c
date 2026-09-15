#define _POSIX_C_SOURCE 200809L

#include "tool_hooks.h"

#include "env_config.h"
#include "json_util.h"
#include "process_capture.h"

#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define TOOL_HOOK_DEFAULT_TIMEOUT_MS 2000
#define TOOL_HOOK_MAX_TIMEOUT_MS 30000
#define TOOL_HOOK_DEFAULT_MAX_OUTPUT 8192
#define TOOL_HOOK_MAX_OUTPUT 65536
#define TOOL_HOOK_DEFAULT_PAYLOAD_MAX 32768
#define TOOL_HOOK_MAX_PAYLOAD 65536

static const char *nonempty_env(const char *name) {
    const char *value = name ? getenv(name) : NULL;
    if (!value || !value[0] || !strcasecmp(value, "off") || !strcmp(value, "0"))
        return NULL;
    return value;
}

const char *dsco_tool_hook_event_name(dsco_tool_hook_event_t event) {
    switch (event) {
    case DSCO_TOOL_HOOK_BEFORE:
        return "tool.call";
    case DSCO_TOOL_HOOK_RESULT:
        return "tool.result";
    case DSCO_TOOL_HOOK_FAILED:
        return "tool.failed";
    case DSCO_TOOL_HOOK_DENIED:
        return "tool.policy.blocked";
    default:
        return "tool.unknown";
    }
}

static const char *hook_path_for(dsco_tool_hook_event_t event) {
    const char *specific = NULL;
    switch (event) {
    case DSCO_TOOL_HOOK_BEFORE:
        specific = nonempty_env("DSCO_TOOL_HOOK_BEFORE");
        break;
    case DSCO_TOOL_HOOK_RESULT:
        specific = nonempty_env("DSCO_TOOL_HOOK_AFTER");
        break;
    case DSCO_TOOL_HOOK_FAILED:
        specific = nonempty_env("DSCO_TOOL_HOOK_FAILED");
        break;
    case DSCO_TOOL_HOOK_DENIED:
        specific = nonempty_env("DSCO_TOOL_HOOK_DENIED");
        break;
    }
    return specific ? specific : nonempty_env("DSCO_TOOL_HOOK");
}

static bool hook_tool_selected(const char *tool_name) {
    const char *filter = nonempty_env("DSCO_TOOL_HOOK_TOOLS");
    if (!filter || !filter[0])
        return true;

    const char *p = filter;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n')
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n'))
            end--;
        size_t len = (size_t)(end - start);
        if (len == 1 && start[0] == '*')
            return true;
        if (tool_name && len == strlen(tool_name) && !strncmp(start, tool_name, len))
            return true;
    }
    return false;
}

static bool hook_payload_mode(const char *mode, bool *include_input, bool *include_result) {
    *include_input = false;
    *include_result = false;
    if (!mode || !mode[0] || !strcasecmp(mode, "metadata"))
        return true;
    if (!strcasecmp(mode, "input")) {
        *include_input = true;
        return true;
    }
    if (!strcasecmp(mode, "full")) {
        *include_input = true;
        *include_result = true;
        return true;
    }
    return false;
}

static void copy_safe_reason(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0)
        return;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : "");
         *p && n + 1 < dst_len; p++) {
        unsigned char c = *p;
        dst[n++] = (c == '"' || c == '\\' || c < 0x20) ? ' ' : (char)c;
    }
    dst[n] = '\0';
}

static void append_bounded_value(jbuf_t *b, const char *key, const char *value,
                                 size_t value_limit, bool object_if_valid) {
    if (!b || !key)
        return;
    jbuf_append(b, ",");
    jbuf_append_json_str(b, key);
    jbuf_append(b, ":");
    if (!value || strlen(value) > value_limit) {
        jbuf_append(b, "null");
        return;
    }
    if (object_if_valid && json_is_valid_container(value))
        jbuf_append(b, value);
    else
        jbuf_append_json_str(b, value);
}

static bool build_payload(dsco_tool_hook_event_t event, const execution_attempt_t *a,
                          const char *input_json, const char *result, char *out,
                          size_t out_len) {
    if (!a || !out || out_len == 0)
        return false;

    const char *mode = nonempty_env("DSCO_TOOL_HOOK_PAYLOAD");
    bool include_input = false, include_result = false;
    if (!hook_payload_mode(mode, &include_input, &include_result))
        return false;

    size_t max_payload = dsco_env_size("DSCO_TOOL_HOOK_PAYLOAD_MAX", TOOL_HOOK_DEFAULT_PAYLOAD_MAX,
                                      1024, TOOL_HOOK_MAX_PAYLOAD);
    size_t value_limit = max_payload > 1024 ? (max_payload - 1024) / 2 : 1;

    jbuf_t b;
    jbuf_init(&b, 2048);
    jbuf_append(&b, "{\"schema\":\"dsco.tool_hook.v1\",\"event\":");
    jbuf_append_json_str(&b, dsco_tool_hook_event_name(event));
    jbuf_append(&b, ",\"execution_id\":");
    jbuf_append_json_str(&b, a->execution_id);
    jbuf_appendf(&b, ",\"sequence\":%llu,\"tool\":",
                 (unsigned long long)a->sequence);
    jbuf_append_json_str(&b, a->tool_name);
    jbuf_append(&b, ",\"dispatch_tool\":");
    if (a->dispatch_tool_name[0])
        jbuf_append_json_str(&b, a->dispatch_tool_name);
    else
        jbuf_append(&b, "null");
    jbuf_append(&b, ",\"tier\":");
    jbuf_append_json_str(&b, a->tier);
    jbuf_append(&b, ",\"principal\":");
    jbuf_append_json_str(&b, a->principal);
    jbuf_append(&b, ",\"capabilities\":");
    jbuf_append_json_str(&b, a->capabilities);
    jbuf_appendf(&b, ",\"capability_mask\":%u,\"status\":",
                 a->capability_mask);
    jbuf_append_json_str(&b, execution_attempt_status_name(a->status));
    jbuf_appendf(&b, ",\"admitted\":%s,\"started\":%s,\"terminal\":%s,"
                    "\"ok\":%s,\"elapsed_ms\":%.3f,\"result_bytes\":%zu",
                 a->admitted ? "true" : "false", a->started ? "true" : "false",
                 a->terminal ? "true" : "false",
                 a->status == EXECUTION_ATTEMPT_SUCCEEDED ? "true" : "false",
                 a->elapsed_ms, a->result_bytes);
    append_bounded_value(&b, "input_sha256", a->input_sha256, 128, false);
    append_bounded_value(&b, "dispatch_input_sha256", a->dispatch_input_sha256, 128, false);
    append_bounded_value(&b, "result_sha256", a->result_sha256, 128, false);
    if (include_input)
        append_bounded_value(&b, "input", input_json ? input_json : "{}", value_limit, true);
    if (include_result)
        append_bounded_value(&b, "result", result ? result : "", value_limit, false);
    jbuf_append(&b, "}");

    bool fits = b.data && b.len + 1 <= out_len && b.len <= max_payload;
    if (fits)
        memcpy(out, b.data, b.len + 1);
    jbuf_free(&b);
    return fits;
}

static bool hook_fail_open(void) {
    const char *mode = nonempty_env("DSCO_TOOL_HOOK_FAIL_MODE");
    return mode && !strcasecmp(mode, "allow");
}

static bool run_hook(const char *path, dsco_tool_hook_event_t event, const char *payload,
                     process_capture_t *capture) {
    if (!path || !path[0] || !payload || !capture)
        return false;
    if (strlen(path) >= PATH_MAX)
        return false;

    char event_name[64];
    snprintf(event_name, sizeof(event_name), "%s", dsco_tool_hook_event_name(event));
    char *argv[] = {(char *)path, event_name, NULL};
    int timeout_ms = dsco_env_int("DSCO_TOOL_HOOK_TIMEOUT_MS", TOOL_HOOK_DEFAULT_TIMEOUT_MS,
                                  1, TOOL_HOOK_MAX_TIMEOUT_MS);
    size_t max_output = dsco_env_size("DSCO_TOOL_HOOK_MAX_OUTPUT", TOOL_HOOK_DEFAULT_MAX_OUTPUT,
                                      256, TOOL_HOOK_MAX_OUTPUT);
    return process_capture_input(path, argv, payload, strlen(payload), timeout_ms, max_output,
                                 capture);
}

bool dsco_tool_hook_before(const execution_attempt_t *attempt, const char *input_json,
                           char *reason, size_t reason_len) {
    if (reason && reason_len)
        reason[0] = '\0';
    const char *path = hook_path_for(DSCO_TOOL_HOOK_BEFORE);
    if (!path || !hook_tool_selected(attempt ? attempt->tool_name : NULL))
        return true;

    char payload[TOOL_HOOK_MAX_PAYLOAD + 1];
    if (!build_payload(DSCO_TOOL_HOOK_BEFORE, attempt, input_json, NULL, payload, sizeof(payload))) {
        if (reason && reason_len)
            snprintf(reason, reason_len, "before hook payload invalid or too large");
        return hook_fail_open();
    }

    process_capture_t capture;
    memset(&capture, 0, sizeof(capture));
    capture.exit_code = -1;
    bool ran = run_hook(path, DSCO_TOOL_HOOK_BEFORE, payload, &capture);
    if (!ran) {
        if (reason && reason_len) {
            if (capture.timed_out)
                snprintf(reason, reason_len, "before hook timed out");
            else if (capture.spawn_error)
                snprintf(reason, reason_len, "before hook unavailable: %s",
                         strerror(capture.spawn_error));
            else
                snprintf(reason, reason_len, "before hook exited %d",
                         capture.exit_code);
        }
        process_capture_free(&capture);
        return hook_fail_open();
    }

    bool allowed = false;
    bool valid_decision = false;
    char *decision = json_get_str(capture.output ? capture.output : "", "decision");
    if (decision && !strcasecmp(decision, "allow")) {
        allowed = true;
        valid_decision = true;
    } else if (decision && !strcasecmp(decision, "deny")) {
        valid_decision = true;
        char *hook_reason = json_get_str(capture.output, "reason");
        if (reason && reason_len)
            snprintf(reason, reason_len, "before hook denied: ");
        if (reason && reason_len && hook_reason && hook_reason[0]) {
            size_t used = strlen(reason);
            if (used + 1 < reason_len)
                copy_safe_reason(reason + used, reason_len - used, hook_reason);
        }
        free(hook_reason);
    } else if (reason && reason_len) {
        snprintf(reason, reason_len, "before hook returned invalid decision");
    }
    free(decision);
    process_capture_free(&capture);
    /* Fail-open covers a broken/malformed hook contract only. An explicit
     * policy denial is always a denial, even when the operator chose to
     * tolerate hook process failures. */
    return valid_decision ? allowed : hook_fail_open();
}

void dsco_tool_hook_after(const execution_attempt_t *attempt, const char *result) {
    if (!attempt || !hook_tool_selected(attempt->tool_name))
        return;

    dsco_tool_hook_event_t event = DSCO_TOOL_HOOK_RESULT;
    if (attempt->status == EXECUTION_ATTEMPT_DENIED)
        event = DSCO_TOOL_HOOK_DENIED;
    else if (attempt->status == EXECUTION_ATTEMPT_FAILED)
        event = DSCO_TOOL_HOOK_FAILED;
    const char *path = hook_path_for(event);
    if (!path)
        return;

    char payload[TOOL_HOOK_MAX_PAYLOAD + 1];
    if (!build_payload(event, attempt, NULL, result, payload, sizeof(payload)))
        return;
    process_capture_t capture;
    memset(&capture, 0, sizeof(capture));
    capture.exit_code = -1;
    (void)run_hook(path, event, payload, &capture);
    process_capture_free(&capture);
}
