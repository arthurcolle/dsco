#ifndef DSCO_COMPUTE_H
#define DSCO_COMPUTE_H

/* ── Adaptive modular compute fabric ──────────────────────────────────────
 * Unifies every place work can run behind the connector seam, as swappable
 * "kinds", plus an adaptive router that picks one per task:
 *
 *   local   → the builtin "shell" kind (Apple-Silicon native exec)      [exists]
 *   api     → the builtin "tool" kind (Tool Management API over HTTP)    [exists]
 *   fleet   → SSH to a ~/bridge/fleet peer (reuses remote_cli)           [here]
 *   modal   → Modal serverless compute over HTTPS                        [here]
 *   mcp     → remote MCP server                                         [follow-up]
 *
 * Backends register once; the router scores available ones by capability,
 * reachability, and policy (local-first). Composes with `flow` like any kind. */

/* Register the compute backends as connector kinds. Idempotent — called from
 * connector_register_builtins(). */
void compute_register_backends(void);

/* CLI entry for `dsco compute …`:
 *   dsco compute --list                          backends + availability
 *   dsco compute [--on KIND] [--peer P] <task…>  run (auto-routes when no --on)
 * Returns a process exit code. */
int dsco_compute_cli(int argc, char **argv);

#endif /* DSCO_COMPUTE_H */
