#include "task_profile_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_complete_or_empty(char *out, size_t out_len, const char *text,
                                    size_t text_len) {
    if (!out || out_len == 0)
        return;
    if (text_len >= out_len) {
        out[0] = '\0';
        return;
    }
    memcpy(out, text, text_len);
    out[text_len] = '\0';
}

static void write_error(char *out, size_t out_len, const char *message) {
    char error[128];
    int n = snprintf(error, sizeof(error), "{\"error\":\"%s\"}", message);
    if (n < 0 || (size_t)n >= sizeof(error)) {
        if (out && out_len)
            out[0] = '\0';
        return;
    }
    write_complete_or_empty(out, out_len, error, (size_t)n);
}

static char *copy_profile_text(const task_profile_t *profile,
                               int (*writer)(const task_profile_t *, char *, size_t)) {
    size_t cap = 2048;
    const size_t max_cap = 1024 * 1024;

    for (;;) {
        char *text = malloc(cap);
        if (!text)
            return NULL;
        int n = writer(profile, text, cap);
        if (n >= 0 && (size_t)n < cap - 1)
            return text;
        free(text);
        if (cap >= max_cap)
            return NULL;
        cap *= 2;
    }
}

bool task_profile_tool_request_decode(json_view_t *view,
                                      task_profile_tool_request_t *out) {
    if (!view || !out)
        return false;
    out->task = json_view_str(view, "task");
    out->explain = json_view_bool(view, "explain", false);
    return true;
}

void task_profile_tool_result_init(task_profile_tool_result_t *result) {
    if (result)
        memset(result, 0, sizeof(*result));
}

void task_profile_tool_result_free(task_profile_tool_result_t *result) {
    if (!result)
        return;
    task_profile_free(result->profile);
    free(result->explanation);
    result->profile = NULL;
    result->explanation = NULL;
}

/* Internal typed handler.  It copies all data needed after the request view
 * is closed into the owned result; it never retains request->task. */
static bool task_profile_tool_handle(const task_profile_tool_request_t *request,
                                     task_profile_tool_result_t *result) {
    if (!request || !result || !request->task || !request->task[0])
        return false;

    result->profile = task_profile(request->task, NULL);
    if (!result->profile)
        return false;

    if (request->explain) {
        result->explanation = copy_profile_text(result->profile, task_profile_explain);
        if (!result->explanation) {
            task_profile_tool_result_free(result);
            return false;
        }
    }
    return true;
}

bool task_profile_tool_execute(const char *input, char *result, size_t rlen) {
    if (!result || rlen == 0)
        return false;

    json_view_t *view = json_view_open(input);
    task_profile_tool_request_t request = {0};
    task_profile_tool_result_t typed_result;
    task_profile_tool_result_init(&typed_result);

    /* json_view_open is stricter than the legacy scanners.  A malformed or
     * non-object input therefore intentionally follows the legacy observable
     * missing-task error rather than being accepted as a partial document. */
    if (!view || !task_profile_tool_request_decode(view, &request) ||
        !request.task || !request.task[0]) {
        write_error(result, rlen, "task string is required");
        json_view_close(view);
        return false;
    }

    if (!task_profile_tool_handle(&request, &typed_result)) {
        write_error(result, rlen, "profiling failed");
        json_view_close(view);
        task_profile_tool_result_free(&typed_result);
        return false;
    }
    json_view_close(view);

    char *profile_json = copy_profile_text(typed_result.profile, task_profile_json);
    if (!profile_json) {
        write_error(result, rlen, "profiling failed");
        task_profile_tool_result_free(&typed_result);
        return false;
    }

    char *wire = profile_json;
    size_t wire_len = strlen(profile_json);
    jbuf_t wrapper = {0};
    if (request.explain) {
        jbuf_init(&wrapper, wire_len + strlen(typed_result.explanation) + 32);
        jbuf_append(&wrapper, "{\"explanation\":");
        jbuf_append_json_str(&wrapper, typed_result.explanation);
        jbuf_append(&wrapper, ",\"profile\":");
        jbuf_append(&wrapper, profile_json);
        jbuf_append_char(&wrapper, '}');
        wire = wrapper.data;
        wire_len = wrapper.len;
    }

    if (wire_len >= rlen) {
        write_error(result, rlen, "output buffer too small");
        if (request.explain)
            jbuf_free(&wrapper);
        free(profile_json);
        task_profile_tool_result_free(&typed_result);
        return false;
    }
    memcpy(result, wire, wire_len);
    result[wire_len] = '\0';

    if (request.explain)
        jbuf_free(&wrapper);
    free(profile_json);
    task_profile_tool_result_free(&typed_result);
    return true;
}
