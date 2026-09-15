#ifndef DSCO_TOOL_HOOKS_H
#define DSCO_TOOL_HOOKS_H

#include "execution_kernel.h"

#include <stdbool.h>
#include <stddef.h>

/* Environment-configured lifecycle hooks for the governed tool boundary.
 *
 * Hook executables are invoked with direct argv (never through a shell) and
 * receive one bounded JSON envelope on stdin. The default payload is
 * metadata-only: raw input/results are represented by the hashes already
 * retained by the execution kernel. Set DSCO_TOOL_HOOK_PAYLOAD=input|full when
 * an operator-owned hook explicitly needs the corresponding raw value.
 *
 * DSCO_TOOL_HOOK_BEFORE (or DSCO_TOOL_HOOK) is the only decision hook. It must
 * print {"decision":"allow"} or {"decision":"deny","reason":"..."} and
 * exit zero. A malformed/failed before hook denies by default; set
 * DSCO_TOOL_HOOK_FAIL_MODE=allow only for an explicitly fail-open policy.
 * After/failed/denied hooks are observational and cannot change execution.
 */

typedef enum {
    DSCO_TOOL_HOOK_BEFORE = 0,
    DSCO_TOOL_HOOK_RESULT,
    DSCO_TOOL_HOOK_FAILED,
    DSCO_TOOL_HOOK_DENIED,
} dsco_tool_hook_event_t;

const char *dsco_tool_hook_event_name(dsco_tool_hook_event_t event);

/* Returns false only when an enabled before hook denies/fails. `reason` is a
 * bounded diagnostic suitable for a tool result; it never contains hook
 * stdout beyond the hook's explicit reason field. */
bool dsco_tool_hook_before(const execution_attempt_t *attempt,
                           const char *input_json, char *reason, size_t reason_len);

/* Called after the execution-kernel terminal state is recorded. The kernel
 * intentionally retains no raw input; result is available only for the
 * explicitly selected full payload mode. This function is observational;
 * hook failures do not change the tool result or terminal receipt. */
void dsco_tool_hook_after(const execution_attempt_t *attempt, const char *result);

#endif /* DSCO_TOOL_HOOKS_H */
