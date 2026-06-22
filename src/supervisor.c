/* src/supervisor.c — Higher-order process supervisor for dsco
 *
 * Runs the real dsco as a managed child process. On abnormal death:
 *   1. Classifies the exit (clean / nonzero / fatal signal / OOM-kill)
 *   2. Captures diagnostics from /tmp/dsco_crash.log + per-pid backtrace
 *   3. Restarts with exponential backoff + crash-loop circuit breaker
 *
 * The child inherits the controlling terminal directly (no pipe), so the
 * interactive TUI works unchanged. Signals are forwarded.
 *
 * See include/supervisor.h for the full design doc and tunables. */

#define _POSIX_C_SOURCE 200809L

#include "supervisor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>

/* ── Defaults ─────────────────────────────────────────────────────────── */
#define DEFAULT_MAX_RESTARTS    8
#define DEFAULT_WINDOW_S        60
#define DEFAULT_STABLE_S        30
#define DEFAULT_BACKOFF_MS      250
#define DEFAULT_BACKOFF_MAX_MS  30000

static int env_int(const char *name, int def, int min_v, int max_v) {
    const char *v = getenv(name);
    if (!v || !v[0]) return def;
    int n = atoi(v);
    if (n < min_v) return min_v;
    if (n > max_v) return max_v;
    return n;
}

static double now_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

/* ── Crash classification ─────────────────────────────────────────────── */
typedef enum {
    EXIT_CLEAN = 0,       /* WIFEXITED, status 0 */
    EXIT_NONZERO,         /* WIFEXITED, status != 0 */
    EXIT_SIGNAL,          /* WIFSIGNALED — SIGSEGV, SIGBUS, SIGABRT, etc. */
    EXIT_OOM_KILL,        /* WIFSIGNALED, SIGKILL — likely jetsam/OOM */
} crash_class_t;

static const char *crash_class_name(crash_class_t c) {
    switch (c) {
        case EXIT_CLEAN:     return "clean";
        case EXIT_NONZERO:   return "nonzero";
        case EXIT_SIGNAL:    return "signal";
        case EXIT_OOM_KILL:  return "oom_kill";
    }
    return "unknown";
}

static crash_class_t classify_exit(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status) == 0 ? EXIT_CLEAN : EXIT_NONZERO;
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig == SIGKILL) return EXIT_OOM_KILL;
        return EXIT_SIGNAL;
    }
    return EXIT_NONZERO;
}

/* ── Diagnostic capture ───────────────────────────────────────────────── */
static void capture_diagnostics(pid_t child_pid, crash_class_t cls, int sig) {
    (void)cls; (void)sig;
    /* Read the child's crash log (written by crash_handler in main.c). */
    char path[64];
    snprintf(path, sizeof(path), "/tmp/dsco_crash.log");
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        fprintf(stderr, "\n[supervisor] ── crash log (/tmp/dsco_crash.log) ──\n");
        while (fgets(line, sizeof(line), f)) {
            fputs(line, stderr);
        }
        fclose(f);
        fprintf(stderr, "[supervisor] ── end crash log ──\n\n");
    }

    /* Check for per-pid backtrace from the debugger. */
    snprintf(path, sizeof(path), "/tmp/dsco_bt_%d.txt", (int)child_pid);
    f = fopen(path, "r");
    if (f) {
        char line[512];
        fprintf(stderr, "[supervisor] ── debugger backtrace (%s) ──\n", path);
        while (fgets(line, sizeof(line), f)) {
            fputs(line, stderr);
        }
        fclose(f);
        fprintf(stderr, "[supervisor] ── end backtrace ──\n\n");
    }
}

/* ── Signal forwarding ────────────────────────────────────────────────── */
/* We want signals delivered to the supervisor to be forwarded to the child.
 * Since the child inherits the controlling terminal, terminal signals (SIGINT,
 * SIGTSTP, SIGQUIT) go directly to the child's process group. But SIGTERM
 * sent explicitly to the supervisor needs forwarding. */
static volatile sig_atomic_t g_forward_pid = -1;

static void forward_signal(int sig) {
    pid_t pid = g_forward_pid;
    if (pid > 0) kill(pid, sig);
    /* Re-install for System V semantics */
    signal(sig, forward_signal);
}

int supervisor_run(int child_argc, char **child_argv) {
    (void)child_argc;

    int max_restarts   = env_int("DSCO_SUPERVISE_MAX_RESTARTS", DEFAULT_MAX_RESTARTS, 1, 100);
    int window_s       = env_int("DSCO_SUPERVISE_WINDOW_S",     DEFAULT_WINDOW_S,     5, 3600);
    int stable_s       = env_int("DSCO_SUPERVISE_STABLE_S",      DEFAULT_STABLE_S,    5, 3600);
    int backoff_ms     = env_int("DSCO_SUPERVISE_BACKOFF_MS",    DEFAULT_BACKOFF_MS,  10, 60000);
    int backoff_max_ms = env_int("DSCO_SUPERVISE_BACKOFF_MAX_MS", DEFAULT_BACKOFF_MAX_MS, 100, 300000);

    /* Build the child argv: self-path + DSCO_NO_SUPERVISE=1 + passed args. */
    /* child_argv[0] is the original argv[0] (the dsco binary path). */
    /* The args after "supervise" were already stripped by main.c. */

    /* Set env so the child knows it's supervised and enables crash handlers. */
    setenv("DSCO_NO_SUPERVISE", "1", 1);
    setenv("DSCO_CRASH_DEBUGGER", "1", 1);

    /* Install signal forwarders. Terminal signals (SIGINT, SIGTSTP, SIGQUIT)
     * go directly to the child's process group since it owns the tty, but
     * SIGTERM and SIGHUP sent to the supervisor need forwarding. */
    signal(SIGTERM, forward_signal);
    signal(SIGHUP,  forward_signal);

    int restart_count = 0;
    double first_crash_time = 0;
    double last_start_time = 0;
    int current_backoff = backoff_ms;

    for (;;) {
        /* Reset the crash log so we don't re-read stale data. */
        unlink("/tmp/dsco_crash.log");

        char supervise_level[16];
        snprintf(supervise_level, sizeof(supervise_level), "%d", restart_count + 1);
        setenv("DSCO_SUPERVISED", supervise_level, 1);

        last_start_time = now_monotonic();

        pid_t child = fork();
        if (child < 0) {
            fprintf(stderr, "[supervisor] fork failed: %s\n", strerror(errno));
            return 1;
        }

        if (child == 0) {
            /* Child: restore default signal handlers, then exec. */
            signal(SIGTERM, SIG_DFL);
            signal(SIGHUP,  SIG_DFL);
            execvp(child_argv[0], child_argv);
            /* If exec fails, the child exits with an error. */
            fprintf(stderr, "[supervisor] exec failed: %s: %s\n",
                    child_argv[0], strerror(errno));
            _exit(127);
        }

        /* Parent: wait for the child. */
        g_forward_pid = child;
        int status = 0;
        pid_t wr;
        do {
            wr = waitpid(child, &status, 0);
        } while (wr < 0 && errno == EINTR);

        g_forward_pid = -1;

        if (wr < 0) {
            fprintf(stderr, "[supervisor] waitpid failed: %s\n", strerror(errno));
            return 1;
        }

        crash_class_t cls = classify_exit(status);

        /* Clean exit — we're done. */
        if (cls == EXIT_CLEAN) {
            if (restart_count > 0) {
                fprintf(stderr, "[supervisor] child exited cleanly after %d restart(s).\n",
                        restart_count);
            }
            return 0;
        }

        /* Abnormal exit — classify and decide whether to restart. */
        int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        double now = now_monotonic();
        double uptime = now - last_start_time;

        fprintf(stderr, "\n[supervisor] child pid=%d died: %s",
                (int)child, crash_class_name(cls));
        if (sig > 0) fprintf(stderr, " (signal %d)", sig);
        fprintf(stderr, " after %.1fs\n", uptime);

        /* Capture diagnostics. */
        capture_diagnostics(child, cls, sig);

        /* ── Circuit breaker logic ─────────────────────────────────────── */
        /* If the child was stable for >= stable_s, reset the restart counter
         * and backoff — this was a "good run" that happened to crash. */
        if (uptime >= (double)stable_s) {
            if (restart_count > 0) {
                fprintf(stderr, "[supervisor] child was stable for %.0fs — resetting crash counter.\n",
                        uptime);
            }
            restart_count = 0;
            first_crash_time = 0;
            current_backoff = backoff_ms;
        }

        /* Track rapid restarts. */
        if (first_crash_time == 0) {
            first_crash_time = now;
        }
        restart_count++;

        /* Check if we're in a crash loop. */
        double window_elapsed = now - first_crash_time;
        if (restart_count > max_restarts && window_elapsed < (double)window_s) {
            fprintf(stderr, "[supervisor] ── CRASH LOOP ──\n");
            fprintf(stderr, "[supervisor] %d restarts in %.0fs (limit: %d in %ds).\n",
                    restart_count, window_elapsed, max_restarts, window_s);
            fprintf(stderr, "[supervisor] Circuit breaker tripped. Giving up.\n");
            fprintf(stderr, "[supervisor] Last exit: %s", crash_class_name(cls));
            if (sig > 0) fprintf(stderr, " (signal %d)", sig);
            fprintf(stderr, ".\n");
            fprintf(stderr, "[supervisor] Check /tmp/dsco_crash.log for details.\n");
            return sig > 0 ? 128 + sig : 1;
        }

        /* If we exceeded the window but still have restarts, slide the window. */
        if (window_elapsed >= (double)window_s) {
            restart_count = 1;
            first_crash_time = now;
            current_backoff = backoff_ms;
        }

        /* Exponential backoff. */
        fprintf(stderr, "[supervisor] restarting in %dms (attempt %d)...\n",
                current_backoff, restart_count + 1);
        sleep_ms(current_backoff);
        current_backoff *= 2;
        if (current_backoff > backoff_max_ms) current_backoff = backoff_max_ms;
    }
}
