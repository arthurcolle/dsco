#ifndef DSCO_TASK_PROFILE_TOOL_H
#define DSCO_TASK_PROFILE_TOOL_H

#include <stdbool.h>
#include <stddef.h>

#include "json_util.h"
#include "task_profile.h"

/* A decoded request borrows task from the json_view passed to
 * task_profile_tool_request_decode().  The task pointer is invalid after the
 * view is closed; the typed handler does not retain it. */
typedef struct {
    const char *task; /* borrowed from json_view_t */
    bool explain;
} task_profile_tool_request_t;

/* A result owns profile and explanation.  Release both with
 * task_profile_tool_result_free(); callers must not free either field. */
typedef struct {
    task_profile_t *profile; /* owned */
    char *explanation;        /* owned; NULL unless explain was requested */
} task_profile_tool_result_t;

/* Decode one already-parsed JSON object.  Missing task is represented by a
 * NULL borrowed task so the adapter can preserve the legacy error response.
 * Missing or non-boolean explain uses the legacy default false. */
bool task_profile_tool_request_decode(json_view_t *view,
                                      task_profile_tool_request_t *out);

void task_profile_tool_result_init(task_profile_tool_result_t *result);
void task_profile_tool_result_free(task_profile_tool_result_t *result);

/* Legacy swarm-action adapter.  Returns false for legacy input errors and for
 * output-capacity failures.  On success result is one complete JSON document;
 * it is never reported successful with a truncated document. */
bool task_profile_tool_execute(const char *input, char *result, size_t rlen);

#endif /* DSCO_TASK_PROFILE_TOOL_H */
