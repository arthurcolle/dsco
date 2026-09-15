#ifndef DSCO_TASK_CLOSEOUT_H
#define DSCO_TASK_CLOSEOUT_H

#include "llm.h"
#include <stdbool.h>
#include <stddef.h>

/* Prompt-scoped schema leases, not proof that the user's objective succeeded.
 * Existing schemas are preserved. If the bounded snapshot cannot represent the
 * register, cleanup fails closed. No workers are cancelled or raw text recorded. */
#define TASK_CLOSEOUT_SCHEMA_MAX 128

typedef struct {
    char names[TASK_CLOSEOUT_SCHEMA_MAX][256];
    int count;
    bool enabled;
    bool valid;
} task_closeout_t;

void task_closeout_begin(task_closeout_t *state);
/* Only a normal terminal response with no active workers releases new schemas.
 * All evictions pass through tools_execute_for_tier. Returns false on a snapshot,
 * dispatch or journal error. A deferred/disabled closeout is a successful no-op. */
bool task_closeout_finish(task_closeout_t *state, const char *tier,
                         bool normal_terminal, int active_workers,
                         char *report, size_t report_len);

/* The existing context_compact handler, with typed, bounded JSON options and
 * honest change telemetry. Schema eviction and semantic summarization are
 * separate operations. This function performs no extra model call. */
bool task_closeout_compact(conversation_t *conv, const char *input,
                          char *result, size_t result_len);
#endif
