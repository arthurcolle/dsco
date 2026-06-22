/* introspect.h — DSCO self-introspection: codebase stats + swarm-of-selves.
 *
 * Exposes the agent's first-person view of its own substrate:
 *  - `dsco --codebase-stats`  : LOC, functions, structs, tools, topology counts
 *  - `dsco --selves N <prompt>`: spawn N parallel angles on a single prompt
 *
 * Pure-C, no LLM cost for stats. Selves uses fork+exec of dsco itself.
 */
#ifndef DSCO_INTROSPECT_H
#define DSCO_INTROSPECT_H

#include <stdio.h>

/* Print compact stats about this dsco install + repo body it was built from.
 * Returns 0 on success. */
int introspect_print_codebase_stats(FILE *out);

/* Spawn `n` parallel `dsco` children, each given an angle-tagged prompt.
 * Aggregates child outputs (truncated) into a synthesis on `out`.
 * Returns 0 if all children launched (per-child failures are reported). */
int introspect_run_selves(FILE *out, int n, const char *prompt);

#endif /* DSCO_INTROSPECT_H */
