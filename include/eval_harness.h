#ifndef DSCO_EVAL_HARNESS_H
#define DSCO_EVAL_HARNESS_H

/*
 * eval_harness — `dsco eval run <set> [--json]` (Wave B #11)
 *
 * Loads a JSONL eval set (one case object per nonblank line with fields
 * `id`, `input`, `expected`, `verifier`), executes the cases, and emits
 * a human table or a machine JSON report.
 *
 * Verifiers (v1, provider-free):
 *   tool_dispatch_smoke   — trivial in-process tool dispatch through the
 *                           governance gate (tools_execute_for_tier).
 *   gate_fs_write_denied  — expect a capability-gate DENY for fs_write
 *                           with DSCO_ALLOW_WRITE=0 and no file written.
 *   mcp_tools_list        — spawn `dsco mcp serve` as a child, drive
 *                           initialize / initialized / tools/list JSON-RPC
 *                           over stdin/stdout, expect a tools array.
 *
 * Exit status: 0 all pass, 1 any case failed/errored, 2 usage or
 * malformed/invalid set.
 *
 * Naming note: include/eval.h and src/eval.c are the pre-existing
 * mathematical expression evaluator (used by the builtin `eval` tool and
 * math_fastpath.c). Per
 * .workspace/harness-parity/swarm-20260827/03-eval-harness-design.md this
 * harness is a separate translation unit and must not grow into eval.c.
 */

/* CLI entry point. `argv` is the normalized dispatch argv where argv[0] is
 * the program name and argv[1] is "eval" (e.g. dsco eval run smoke --json).
 * Returns the process exit status. */
int eval_cli(int argc, char **argv);

#endif /* DSCO_EVAL_HARNESS_H */
