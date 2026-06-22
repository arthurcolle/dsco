#ifndef DSCO_SUPERVISOR_H
#define DSCO_SUPERVISOR_H

/* ── Higher-order process supervisor ──────────────────────────────────────
 *
 * A foreground watcher that runs the real dsco as a managed child and keeps
 * it alive through anything the OS or a bug can throw at it — SIGSEGV, SIGBUS,
 * SIGABRT, and crucially the uncatchable SIGKILL the macOS jetsam/OOM killer
 * sends ("zsh: killed dsco"). On abnormal death it:
 *
 *   1. Classifies the exit (clean / nonzero / fatal signal / OOM-kill).
 *   2. Captures diagnostics: the child's /tmp/dsco_crash.log + per-pid
 *      backtrace, and (if a debugger is installed and ptrace wasn't denied)
 *      a fresh lldb/gdb postmortem.
 *   3. Restarts the child with exponential backoff + a crash-loop circuit
 *      breaker, relying on dsco's _autosave.json to resume the session.
 *
 * The child inherits the real controlling terminal directly (no pipe), so the
 * interactive TUI works unchanged. Signals are forwarded so Ctrl-C, window
 * resizes, and clean terminate behave as if the supervisor weren't there.
 *
 * Invoked as `dsco supervise [args...]` / `dsco --supervise [args...]`.
 * The child is launched with DSCO_NO_SUPERVISE=1 so it never recurses, plus
 * DSCO_SUPERVISED=<n> and DSCO_CRASH_DEBUGGER=1 so its crash handler engages.
 *
 * Tunables (env):
 *   DSCO_SUPERVISE_MAX_RESTARTS  cap on rapid restarts before giving up (8)
 *   DSCO_SUPERVISE_WINDOW_S      window that defines "rapid", seconds     (60)
 *   DSCO_SUPERVISE_STABLE_S      uptime that resets the backoff, seconds  (30)
 *   DSCO_SUPERVISE_BACKOFF_MS    initial backoff in ms, doubles each crash(250)
 *   DSCO_SUPERVISE_BACKOFF_MAX_MS backoff ceiling in ms                 (30000)
 *   DSCO_CRASH_DEBUGGER          1 = attach lldb/gdb on crash (default 1 here)
 * ─────────────────────────────────────────────────────────────────────── */

/* Run argv as a supervised dsco child. Blocks until the child exits cleanly
 * or the circuit breaker trips. Returns the child's final exit status (or a
 * nonzero code if supervision gave up). */
int supervisor_run(int child_argc, char **child_argv);

/* Install fatal-signal crash handlers (SIGSEGV/SIGBUS/SIGABRT/SIGFPE/SIGILL).
 * Defined in main.c; declared here so the supervised child path can share it. */
void main_install_crash_handlers(void);

#endif /* DSCO_SUPERVISOR_H */
