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
/* macOS hides proc_pid_rusage(), rusage_info_v2 and the full sysctl surface
 * behind the Darwin namespace; _POSIX_C_SOURCE alone masks them. */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include "supervisor.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <time.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

/* ── Defaults ─────────────────────────────────────────────────────────── */
#define DEFAULT_MAX_RESTARTS    8
#define DEFAULT_WINDOW_S        60
#define DEFAULT_STABLE_S        30
#define DEFAULT_BACKOFF_MS      250
#define DEFAULT_BACKOFF_MAX_MS  30000

/* Active memory watchdog: sample the live child's RSS and pre-empt the
 * kernel's uncatchable SIGKILL with a graceful, resumable restart. */
#define DEFAULT_POLL_MS         250    /* how often to sample child RSS       */
#define DEFAULT_MEM_SOFT_PCT    75     /* % of hard limit → warn (no signal)  */
#define DEFAULT_TERM_GRACE_MS   4000   /* SIGTERM→SIGKILL grace on pre-empt   */

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

/* ── Memory introspection ─────────────────────────────────────────────── */

/* Total physical RAM in bytes (0 = unknown). */
static uint64_t system_mem_bytes(void) {
    uint64_t mem = 0;
#ifdef __APPLE__
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) != 0) return 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long psz   = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psz > 0) mem = (uint64_t)pages * (uint64_t)psz;
#endif
    return mem;
}

/* Resident-set / physical footprint of a live pid in bytes (0 = unknown). */
static uint64_t child_rss_bytes(pid_t pid) {
#ifdef __APPLE__
    struct rusage_info_v2 ri;
    if (proc_pid_rusage((int)pid, RUSAGE_INFO_V2, (rusage_info_t *)&ri) == 0)
        return ri.ri_resident_size;
    return 0;
#else
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/statm", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long size_pages = 0, rss_pages = 0;
    int got = fscanf(f, "%lu %lu", &size_pages, &rss_pages);
    fclose(f);
    if (got != 2) return 0;
    long psz = sysconf(_SC_PAGESIZE);
    return (uint64_t)rss_pages * (uint64_t)(psz > 0 ? psz : 4096);
#endif
}

/* macOS VM pressure level: 1=normal, 2=warn, 4=critical (0 = unknown). */
static int system_mem_pressure(void) {
#ifdef __APPLE__
    int level = 0;
    size_t len = sizeof(level);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &len, NULL, 0) != 0)
        return 0;
    return level;
#else
    return 0;
#endif
}

/* Resolve the child RSS hard limit (bytes). Explicit env wins; otherwise
 * 60% of physical RAM, clamped to [1 GiB, 6 GiB]. */
static uint64_t resolve_mem_hard_limit(void) {
    long mb = env_int("DSCO_SUPERVISE_MEM_LIMIT_MB", 0, 0, 1 << 20);
    if (mb > 0) return (uint64_t)mb * 1024ULL * 1024ULL;
    uint64_t sys = system_mem_bytes();
    uint64_t budget = sys ? (sys * 6ULL / 10ULL) : (4ULL << 30);
    const uint64_t cap = 6ULL << 30, floor_b = 1ULL << 30;
    if (budget > cap)     budget = cap;
    if (budget < floor_b) budget = floor_b;
    return budget;
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

    /* ── Active memory watchdog tunables ──────────────────────────────── */
    int poll_ms        = env_int("DSCO_SUPERVISE_POLL_MS",       DEFAULT_POLL_MS,     20, 5000);
    int term_grace_ms  = env_int("DSCO_SUPERVISE_TERM_GRACE_MS", DEFAULT_TERM_GRACE_MS, 100, 60000);
    int soft_pct       = env_int("DSCO_SUPERVISE_MEM_SOFT_PCT",  DEFAULT_MEM_SOFT_PCT, 10, 99);
    uint64_t mem_hard_bytes = resolve_mem_hard_limit();
    uint64_t mem_soft_bytes = mem_hard_bytes / 100ULL * (uint64_t)soft_pct;
    {
        uint64_t sys = system_mem_bytes();
        fprintf(stderr,
            "[supervisor] memory watchdog: budget %lluMB (soft %lluMB) of "
            "%lluMB RAM, sampling every %dms.\n",
            (unsigned long long)(mem_hard_bytes >> 20),
            (unsigned long long)(mem_soft_bytes >> 20),
            (unsigned long long)(sys >> 20),
            poll_ms);
    }

    /* OTP restart type (erlang supervisor child_spec Restart):
     *   transient (default) — restart only on abnormal exit; a clean exit 0
     *                         (e.g. the user typed /quit) is honoured.
     *   permanent           — always restart, even on a clean exit. Use for a
     *                         daemon that must never stay down.
     *   temporary           — never restart; just report and propagate.
     * This is the genuinely useful piece of BEAM/OTP for "never get killed":
     * supervision semantics, not the bytecode emulator (dsco already has a
     * stack VM in vm.c + a reduction-style scheduler in scheduler.c). */
    const char *rt = getenv("DSCO_SUPERVISE_RESTART");
    enum { RT_TRANSIENT, RT_PERMANENT, RT_TEMPORARY } restart_type =
        (rt && strcmp(rt, "permanent") == 0) ? RT_PERMANENT :
        (rt && strcmp(rt, "temporary") == 0) ? RT_TEMPORARY : RT_TRANSIENT;

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

        /* Parent: actively monitor the child instead of blocking on it.
         * Sampling the child's RSS lets us pre-empt the kernel's uncatchable
         * SIGKILL: when the footprint approaches the memory budget we trigger
         * a graceful, resumable restart of our own — an event we control and
         * can checkpoint — rather than waiting for jetsam to reap the process
         * (and possibly the supervisor) with no warning. */
        g_forward_pid = child;
        int status = 0;
        pid_t wr = 0;
        uint64_t peak_rss = 0;
        bool soft_warned = false;
        bool preempted = false;   /* we asked the child to restart for memory */

        for (;;) {
            wr = waitpid(child, &status, WNOHANG);
            if (wr == child) break;             /* child exited on its own */
            if (wr < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "[supervisor] waitpid failed: %s\n", strerror(errno));
                return 1;
            }

            /* wr == 0: child still alive — sample its memory. */
            uint64_t rss = child_rss_bytes(child);
            if (rss > peak_rss) peak_rss = rss;

            if (rss > 0 && rss >= mem_hard_bytes) {
                int pressure = system_mem_pressure();
                fprintf(stderr,
                    "\n[supervisor] child pid=%d RSS %lluMB ≥ budget %lluMB"
                    "%s — pre-empting the OOM killer with a graceful restart.\n",
                    (int)child,
                    (unsigned long long)(rss >> 20),
                    (unsigned long long)(mem_hard_bytes >> 20),
                    pressure >= 2 ? " (system under memory pressure)" : "");
                preempted = true;

                /* Hand the child a chance to checkpoint and exit cleanly. */
                kill(child, SIGTERM);
                int waited_ms = 0;
                while (waited_ms < term_grace_ms) {
                    pid_t g = waitpid(child, &status, WNOHANG);
                    if (g == child) { wr = child; break; }
                    if (g < 0 && errno != EINTR) break;
                    sleep_ms(50);
                    waited_ms += 50;
                }
                /* Still alive after the grace window — force it down so the
                 * restart can proceed (better us than jetsam: we resume). */
                if (wr != child) {
                    kill(child, SIGKILL);
                    do { wr = waitpid(child, &status, 0); }
                    while (wr < 0 && errno == EINTR);
                }
                break;
            }

            if (rss > 0 && rss >= mem_soft_bytes && !soft_warned) {
                soft_warned = true;
                fprintf(stderr,
                    "[supervisor] child RSS %lluMB crossed soft watermark "
                    "%lluMB (budget %lluMB) — watching closely.\n",
                    (unsigned long long)(rss >> 20),
                    (unsigned long long)(mem_soft_bytes >> 20),
                    (unsigned long long)(mem_hard_bytes >> 20));
            }

            sleep_ms(poll_ms);
        }

        g_forward_pid = -1;

        if (wr < 0) {
            fprintf(stderr, "[supervisor] waitpid failed: %s\n", strerror(errno));
            return 1;
        }

        if (peak_rss > 0) {
            fprintf(stderr, "[supervisor] child pid=%d peak RSS: %lluMB.\n",
                    (int)child, (unsigned long long)(peak_rss >> 20));
        }

        /* A memory pre-emption is an abnormal exit we manufactured: classify
         * it as an OOM event so the restart path engages and the child is told
         * to come back leaner and resume from its last checkpoint. */
        crash_class_t cls = preempted ? EXIT_OOM_KILL : classify_exit(status);

        /* Tell the next incarnation why it is restarting so it can resume the
         * session and (on memory events) start in a more conservative state. */
        if (cls == EXIT_OOM_KILL) {
            setenv("DSCO_MEM_PRESSURE", "1", 1);
            setenv("DSCO_RESUME_AFTER_CRASH", "1", 1);
        } else if (cls == EXIT_SIGNAL || cls == EXIT_NONZERO) {
            setenv("DSCO_RESUME_AFTER_CRASH", "1", 1);
        } else {
            unsetenv("DSCO_MEM_PRESSURE");
            unsetenv("DSCO_RESUME_AFTER_CRASH");
        }

        /* Clean exit — honour OTP restart semantics. transient/temporary stop;
         * only permanent keeps a cleanly-exited child alive. */
        if (cls == EXIT_CLEAN) {
            if (restart_type != RT_PERMANENT) {
                if (restart_count > 0)
                    fprintf(stderr, "[supervisor] child exited cleanly after %d restart(s).\n",
                            restart_count);
                return 0;
            }
            fprintf(stderr, "[supervisor] child exited cleanly but restart=permanent — relaunching.\n");
        }

        /* temporary children are never restarted, abnormal exit or not. */
        if (restart_type == RT_TEMPORARY) {
            int sig0 = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
            fprintf(stderr, "\n[supervisor] child died (%s) and restart=temporary — not restarting.\n",
                    crash_class_name(cls));
            capture_diagnostics(child, cls, sig0);
            return sig0 > 0 ? 128 + sig0 : (WIFEXITED(status) ? WEXITSTATUS(status) : 1);
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
