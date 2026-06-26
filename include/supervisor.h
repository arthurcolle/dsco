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
 * Beyond reacting to death, it actively samples the live child's resident-set
 * size (proc_pid_rusage on macOS, /proc/<pid>/statm on Linux) every poll tick.
 * As the footprint approaches a memory budget it pre-empts the kernel's
 * uncatchable SIGKILL with a graceful, controlled restart of its own —
 * SIGTERM → grace window → SIGKILL, then relaunch. The restarted child is
 * handed DSCO_MEM_PRESSURE=1 and DSCO_RESUME_AFTER_CRASH=1 so it can come back
 * leaner and resume from its last checkpoint instead of dying with no warning.
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
 *   DSCO_SUPERVISE_MEM_LIMIT_MB  child RSS budget; 0/unset = 60% of RAM,
 *                                clamped to [1024, 6144]                  (0)
 *   DSCO_SUPERVISE_MEM_SOFT_PCT  % of budget that logs a soft warning     (75)
 *   DSCO_SUPERVISE_POLL_MS       RSS sampling interval, ms               (250)
 *   DSCO_SUPERVISE_TERM_GRACE_MS SIGTERM→SIGKILL grace on pre-empt, ms  (4000)
 *   DSCO_SUPERVISE_METRICS_SECS  child RSS JSONL sample cadence; 0=off     (5)
 *
 * Runtime artifacts:
 *   ~/.dsco/supervisor.log                 human-readable supervisor events
 *   ~/.dsco/incidents/incident-*.json      per-abnormal-exit incident report
 *   ~/.dsco/last_incident.json             most recent incident report
 *   ~/.dsco/child-metrics-<pid>.jsonl      low-rate child RSS samples
 * ─────────────────────────────────────────────────────────────────────── */

/* Reserved nonzero exits that are expected process outcomes, not crashes. */
#define DSCO_EXIT_CONFIG 78
#define DSCO_EXIT_USER_REQUESTED 79

/* Run argv as a supervised dsco child. Blocks until the child exits cleanly
 * or the circuit breaker trips. Returns the child's final exit status (or a
 * nonzero code if supervision gave up). */
int supervisor_run(int child_argc, char **child_argv);

/* If dsco is the top-level command in an iTerm session, a voluntary exit would
 * otherwise end the pty and close the tab. This helper execs the user's shell
 * only for that interactive no-parent-shell case; it returns when not needed or
 * if exec fails. */
void dsco_maybe_exec_shell_to_keep_terminal(void);

/* Install fatal-signal crash handlers (SIGSEGV/SIGBUS/SIGABRT/SIGFPE/SIGILL).
 * Defined in main.c; declared here so the supervised child path can share it. */
void main_install_crash_handlers(void);

#endif /* DSCO_SUPERVISOR_H */
