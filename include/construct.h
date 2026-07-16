#ifndef DSCO_CONSTRUCT_H
#define DSCO_CONSTRUCT_H

#include <stdbool.h>
#include <stddef.h>

/* ── Contextual Construct — process-lifecycle control plane ────────────────
 *
 * A top-level supervisor that manages the lifetimes of long-running tool calls
 * instead of the static per-tool timeout table. It periodically inspects the
 * in-flight tool watchdogs (see watchdog_active_snapshot in tools.h) and, for
 * work that is protected / high-priority, renews the deadline before the
 * watchdog would kill it — up to an absolute lifetime cap. Low-value or
 * over-cap work is allowed to expire.
 *
 * The renewal policy is a mutable "construct" the agent reprioritizes live via
 * the `construct` tool (protect/unprotect/renew/status/tick), so goals can be
 * reweighted at runtime. This generalizes the static swarm=3660 override into
 * dynamic, renewable, priority-aware scheduling and is the substrate a
 * goal-linked scheduler layers on next.
 */

typedef enum {
    CONSTRUCT_PRIO_IDLE = 0,
    CONSTRUCT_PRIO_LOW,
    CONSTRUCT_PRIO_NORMAL,
    CONSTRUCT_PRIO_HIGH,
    CONSTRUCT_PRIO_CRITICAL,
    CONSTRUCT_PRIO_COUNT
} construct_priority_t;

/* Renewal policy for one protected tool name. */
typedef struct {
    char tool_name[64];
    construct_priority_t priority;
    int renew_quantum_s; /* deadline extension granted per renewal */
    int low_water_s;     /* renew once remaining deadline drops below this */
    int max_lifetime_s;  /* absolute cap on total runtime; 0 = unlimited */
    bool enabled;
} construct_policy_t;

/* Supervisor thread lifecycle. Both are idempotent. construct_start honors the
 * env gate DSCO_CONSTRUCT (see construct.c) and DSCO_CONSTRUCT_TICK_MS. */
void construct_start(void);
void construct_stop(void);

/* One supervisor step: renew protected in-flight calls that are near their
 * deadline and still under their lifetime cap. Runs on the supervisor thread;
 * exposed for manual driving / tests. Returns the number of renewals issued. */
int construct_tick(void);

/* Policy mutation (thread-safe). construct_protect inserts or updates the
 * policy for tool_name; construct_unprotect removes it. */
void construct_protect(const char *tool_name, construct_priority_t prio, int renew_quantum_s,
                       int low_water_s, int max_lifetime_s);
void construct_unprotect(const char *tool_name);

/* Register the agent-facing `construct` tool (via tools_register_external).
 * Call once during tool init, before the tool map is (re)built. */
void construct_register_tool(void);

#endif /* DSCO_CONSTRUCT_H */
