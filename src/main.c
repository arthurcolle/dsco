#include "agent.h"
#include "orchestrator.h"
#include "config.h"
#include "tui.h"
#include "llm.h"
#include "tools.h"
#include "pets.h"
#include "self_improve.h"
#include "anim.h"
#include "json_util.h"
#include "ipc.h"
#include "md.h"
#include "baseline.h"
#include "setup.h"
#include "provider.h"
#include "openrouter_cache.h"
#include "topology.h"
#include "workspace.h"
#include "trace.h"
#include "output_guard.h"
#include "router.h"
#include "arena_alloc.h"
#include "event_loop.h"
#include "vm.h"
#include "scheduler.h"
#include "vfs.h"
#include "semantic.h"
#include "memory_tier.h"
#include "vecstore.h"
#include "pheromone.h"
#include "ooda.h"
#include "governance.h"
#include "killswitch.h"
#include "talons.h"
#include "tamper.h"
#include "sealed_store.h"
#include "se_store.h"
#include "audit_log.h"
#include "watchdog.h"
#include "env_guard.h"
#include "heartbeat.h"
#include "mesh.h"
#include "extension/numerical_backend.h"
#include "extension/eigen_backend.h"
#include "extension/fftw_backend.h"
#include "net_server.h"
#include "peer_bootstrap.h"
#include "cost_model.h"
#include "plan_cache.h"
#include "plan_optimizer.h"
#include "dsco_dht.h"
#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
extern void dsco_net_routes_register(void *srv_opaque);
#endif
#include "presence.h"
#include "touchid.h"
#include "project.h"
#include "project_mux.h"
#include "dsco_accel.h"
#include "dsco_mlx.h"
#include "dsco_pool.h"
#include "fingerprint.h"
#include "trust.h"
#include "toolmgmt.h"
#include "connector.h"
#include "startup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>
#include <glob.h>
#include <sys/stat.h>
#include <execinfo.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "supervisor.h"

static md_renderer_t s_oneshot_md;

/* --cheap mode: only ALWAYS-core tools (5) + no compact catalog */
int g_cheap_mode = 0;

static bool is_legacy_sonnet_default_model(const char *model) {
    if (!model || !model[0]) return false;
    const char *resolved = model_resolve_alias(model);
    return resolved && strstr(resolved, "claude-sonnet") != NULL;
}

/* Opt-in startup timing. Enable with DSCO_PERF=1. */
static bool   g_perf_enabled = false;
static bool   g_perf_json = false;
static double g_perf_t0_ms = 0.0;
static double g_perf_last_ms = 0.0;
static dsco_profile_t g_perf_profile = DSCO_PROFILE_FULL;
static dsco_caps_t g_perf_caps = 0;

static double perf_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static bool perf_env_enabled(const char *v) {
    return v && (v[0] == '1' || strcasecmp(v, "true") == 0 ||
                 strcasecmp(v, "yes") == 0 || strcasecmp(v, "trace") == 0 ||
                 strcasecmp(v, "json") == 0 || strcasecmp(v, "jsonl") == 0);
}

static void perf_init(void) {
    const char *perf = getenv("DSCO_PERF");
    g_perf_json = perf && (strcasecmp(perf, "json") == 0 ||
                           strcasecmp(perf, "jsonl") == 0);
    g_perf_enabled = perf_env_enabled(perf);
    if (!g_perf_enabled) return;
    g_perf_t0_ms = perf_now_ms();
    g_perf_last_ms = g_perf_t0_ms;
    if (g_perf_json) {
        fprintf(stderr,
                "{\"type\":\"startup\",\"event\":\"begin\",\"total_ms\":0.000}\n");
    } else {
        fprintf(stderr, "perf startup begin\n");
    }
}

static void perf_set_startup_context(dsco_profile_t profile, dsco_caps_t caps) {
    g_perf_profile = profile;
    g_perf_caps = caps;
}

static void perf_mark(const char *label) {
    if (!g_perf_enabled) return;
    double now = perf_now_ms();
    if (g_perf_json) {
        char caps[192];
        fprintf(stderr,
                "{\"type\":\"startup\",\"event\":\"mark\",\"label\":\"%s\","
                "\"delta_ms\":%.3f,\"total_ms\":%.3f,"
                "\"profile\":\"%s\",\"caps\":\"%s\"}\n",
                label ? label : "mark",
                now - g_perf_last_ms,
                now - g_perf_t0_ms,
                dsco_profile_name(g_perf_profile),
                dsco_caps_to_string(g_perf_caps, caps, sizeof(caps)));
    } else {
        fprintf(stderr, "perf %-28s +%7.2f ms total=%7.2f ms\n",
                label ? label : "mark", now - g_perf_last_ms, now - g_perf_t0_ms);
    }
    g_perf_last_ms = now;
}

static void perf_finish(const char *label) {
    perf_mark(label ? label : "finish");
}

/* ── Post-LLM Virtual OS subsystems ────────────────────────────────── */
vm_t                g_vm;          /* §3: bytecode dispatch VM (extern'd by tools.c) */
static scheduler_t  g_scheduler;   /* §1/§7: cooperative task scheduler */
static ev_loop_t   *g_ev_loop;     /* §6: event loop */
static vfs_db_t    *g_vfs;         /* §8: embedded persistence */
static bool         g_scheduler_ready = false;
static bool         g_arena_ready = false;
static bool         g_trace_ready = false;
static bool         g_ipc_ready = false;
static bool         g_startup_initialized = false;

/* ── Native networking globals ──────────────────────────────────────────── */
#if defined(HAVE_LIBSODIUM)
mesh_node_t      *g_mesh_node  = NULL;
#endif
#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
dsco_net_server_t *g_net_server = NULL;
/* Forward declaration — defined in net_tool.c (also declared above near includes) */
#endif

/* Signal handler for clean IPC shutdown in sub-agent mode */
static volatile sig_atomic_t g_main_interrupted = 0;

static void init_trace_runtime(void) {
#ifdef DSCO_DEV_BINARY
    if (!getenv("DSCO_TRACE")) {
        setenv("DSCO_TRACE", "debug", 1);
    }
#endif
    TRACE_INIT();
    g_trace_ready = true;
}

static void main_sigterm_handler(int sig) {
    (void)sig;
    g_main_interrupted = 1;
}

static void main_zero32(uint8_t key[32]);
static bool init_secure_store_required(uint8_t out_key[32], int *timed_out);

/* Crash handler — save diagnostic info before dying.
 *
 * Only async-signal-safe operations are strictly guaranteed here (write,
 * _exit, fork). backtrace()/backtrace_symbols_fd() are practically safe and
 * the process is dying anyway, so we accept best-effort. The supervisor
 * (see supervisor.c) reads /tmp/dsco_crash.log and the per-pid backtrace to
 * decide how to rescue/restart, so this is the bridge to the watcher. */
static volatile sig_atomic_t g_in_crash = 0;
static volatile sig_atomic_t g_alarm_fired = 0;
static void crash_alarm_handler(int sig) { (void)sig; g_alarm_fired = 1; }

static void crash_write_sig(int fd, int sig, const char *name) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "\n[dsco crash] signal=%s(%d) pid=%d ppid=%d "
                     "supervised=%s build=%s last_state=%s/.dsco/last_heartbeat.json\n",
                     name, sig, getpid(), getppid(),
                     getenv("DSCO_SUPERVISED") ? getenv("DSCO_SUPERVISED") : "0",
                     GIT_HASH, getenv("HOME") ? getenv("HOME") : "/tmp");
    if (n > 0) (void)!write(fd, buf, (size_t)n);
}

/* Best-effort: spawn lldb (macOS) or gdb (Linux) to attach to the crashing
 * process and dump a full backtrace to a per-pid report. Gated on
 * DSCO_CRASH_DEBUGGER so it never fires in normal/unsupervised runs (and only
 * works when anti-ptrace was skipped — see tamper_init/DSCO_DEBUG).
 *
 * The crashing thread is still alive inside this handler, so the debugger
 * attaches to a LIVE target and captures the real faulting frames. We fork
 * the debugger and block (waitpid) until it detaches before re-raising, so
 * the process doesn't die out from under it. */
static void crash_spawn_debugger(int crashing_pid) {
    const char *want = getenv("DSCO_CRASH_DEBUGGER");
    if (!want || want[0] == '0' || want[0] == '\0') return;

    char pidbuf[16], report[64];
    snprintf(pidbuf, sizeof(pidbuf), "%d", crashing_pid);
    snprintf(report, sizeof(report), "/tmp/dsco_bt_%d.txt", crashing_pid);

    pid_t dbg = fork();
    if (dbg < 0) return;
    if (dbg == 0) {
        /* child: redirect output to the report file, then exec the debugger */
        int rfd = open(report, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (rfd >= 0) { dup2(rfd, STDOUT_FILENO); dup2(rfd, STDERR_FILENO); close(rfd); }
#ifdef __APPLE__
        execlp("lldb", "lldb", "-p", pidbuf, "--batch",
               "-o", "thread backtrace all", "-o", "register read",
               "-o", "detach", "-o", "quit", (char *)NULL);
        execlp("gdb", "gdb", "-p", pidbuf, "-batch", "-nx",
               "-ex", "thread apply all bt full", "-ex", "detach", "-ex", "quit",
               (char *)NULL);
#else
        execlp("gdb", "gdb", "-p", pidbuf, "-batch", "-nx",
               "-ex", "thread apply all bt full",
               "-ex", "info registers", "-ex", "detach", "-ex", "quit",
               (char *)NULL);
        execlp("lldb", "lldb", "-p", pidbuf, "--batch",
               "-o", "thread backtrace all", "-o", "detach", "-o", "quit",
               (char *)NULL);
#endif
        _exit(127);  /* no debugger installed */
    }
    /* parent (crashing): wait for the debugger to finish, but don't hang
     * forever if it stalls — SIGALRM breaks the waitpid. */
    alarm(20);
    int st;
    while (waitpid(dbg, &st, 0) < 0 && errno == EINTR && !g_alarm_fired)
        ;
    alarm(0);
}

static void crash_handler(int sig) {
    if (g_in_crash) { signal(sig, SIG_DFL); raise(sig); return; }
    g_in_crash = 1;

    const char *name = sig == SIGSEGV ? "SIGSEGV" :
                       sig == SIGBUS  ? "SIGBUS"  :
                       sig == SIGABRT ? "SIGABRT" :
                       sig == SIGFPE  ? "SIGFPE"  :
                       sig == SIGILL  ? "SIGILL"  : "UNKNOWN";

    /* In-process backtrace via execinfo — works without a debugger or ptrace. */
    void *frames[64];
    int nframes = backtrace(frames, 64);

    int fd = open("/tmp/dsco_crash.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        crash_write_sig(fd, sig, name);
        if (nframes > 0) backtrace_symbols_fd(frames, nframes, fd);
        close(fd);
    }
    /* Also to stderr so a foreground user / supervisor sees it immediately. */
    crash_write_sig(STDERR_FILENO, sig, name);
    if (nframes > 0) backtrace_symbols_fd(frames, nframes, STDERR_FILENO);

    /* Optional richer postmortem via an attached debugger. */
    crash_spawn_debugger(getpid());

    /* Re-raise with default handler so the OS produces a core / the
     * supervisor's waitpid() sees WIFSIGNALED with the real signal. */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Register the full set of fatal-signal crash handlers. Idempotent.
 *
 * The fatal handlers run on a dedicated alternate stack (SA_ONSTACK + a
 * sigaltstack installed below). Without this, a stack-overflow SIGSEGV — or
 * any fault that left the thread stack unusable — would re-fault the instant
 * the handler pushed a frame, producing the silent "no crash log at all"
 * failure mode: the OS kills us before /tmp/dsco_crash.log is ever written.
 * The alt stack guarantees the in-process backtrace path always runs. */
void main_install_crash_handlers(void) {
    /* Install the alternate signal stack once. 64 KiB comfortably holds the
     * handler + backtrace()/backtrace_symbols_fd() working set. Static so it
     * outlives this call; aligned for the platform. */
    static _Alignas(16) char alt_stack[64 * 1024];
    static int alt_installed = 0;
    if (!alt_installed) {
        stack_t ss;
        memset(&ss, 0, sizeof(ss));
        ss.ss_sp    = alt_stack;
        ss.ss_size  = sizeof(alt_stack);
        ss.ss_flags = 0;
        if (sigaltstack(&ss, NULL) == 0) alt_installed = 1;
    }

    struct sigaction sa_crash;
    memset(&sa_crash, 0, sizeof(sa_crash));
    sa_crash.sa_handler = crash_handler;
    sigemptyset(&sa_crash.sa_mask);
    sa_crash.sa_flags = alt_installed ? SA_ONSTACK : 0;
    sigaction(SIGSEGV, &sa_crash, NULL);
    sigaction(SIGBUS,  &sa_crash, NULL);
    sigaction(SIGABRT, &sa_crash, NULL);
    sigaction(SIGFPE,  &sa_crash, NULL);
    sigaction(SIGILL,  &sa_crash, NULL);

    /* Non-restarting SIGALRM so the crash-time debugger wait can time out.
     * Kept on the normal stack — it only fires during the (already-running)
     * crash handler's waitpid, where the stack is known-good. */
    struct sigaction sa_alrm;
    memset(&sa_alrm, 0, sizeof(sa_alrm));
    sa_alrm.sa_handler = crash_alarm_handler;
    sigemptyset(&sa_alrm.sa_mask);
    sa_alrm.sa_flags = 0;  /* no SA_RESTART */
    sigaction(SIGALRM, &sa_alrm, NULL);
}

static void main_atexit_handler(void) {
    if (heartbeat_running()) {
        audit_log("runtime", "clean exit");
        heartbeat_stop();
    }

    /* Persist self-improvement learnings (strategy weights, session stats)
     * for the next run. Guarded by initialized so non-agent fast paths skip. */
    if (g_self_improve.initialized) {
        self_improve_consolidate(&g_self_improve);
        self_improve_save_history(&g_self_improve);
    }

    /* Shutdown Post-LLM OS subsystems */
    if (g_vfs)     { vfs_close(g_vfs); g_vfs = NULL; }
    if (g_ev_loop) { ev_loop_free(g_ev_loop); g_ev_loop = NULL; }
    if (g_scheduler_ready) {
        sched_destroy(&g_scheduler);
        g_scheduler_ready = false;
    }
    if (g_arena_ready) {
        arena_subsystem_shutdown();
        g_arena_ready = false;
    }
    if (g_trace_ready) {
        TRACE_SHUTDOWN();
        g_trace_ready = false;
    }
    if (g_ipc_ready) {
        ipc_shutdown();
        g_ipc_ready = false;
    }


    /* ── Cost model + plan cache flush ─────────────────────────────────── */
    cost_model_flush();
    plan_cache_flush();
    /* ── Native networking teardown ─────────────────────────────────────── */
#if defined(HAVE_LIBSODIUM)
    peer_bootstrap_stop();
    if (g_mesh_node) { mesh_node_destroy(g_mesh_node); g_mesh_node = NULL; }
#endif
#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
    if (g_net_server) { netsrv_destroy(g_net_server); g_net_server = NULL; }
#endif
}

static void init_vos_subsystems(void) {
    /* SQLite serialized mode: multiple concurrent tool threads share the
     * same vfs_db_t connection (and its prepared statements) via the
     * global g_vfs / g_tools_vfs. System libsqlite3 on macOS ships as
     * THREADSAFE=2 (multi-thread), which means a single connection is
     * NOT safe to use from more than one thread simultaneously. Without
     * this call, vfs_result_put racing with itself from concurrent
     * tool threads corrupts sqlite's internal allocator and crashes
     * inside sqlite3VdbeTransferError with a KERN_INVALID_ADDRESS
     * pointing at ASCII hex characters (the tool key buffer).
     *
     * SQLITE_CONFIG_SERIALIZED switches the process default to full
     * serialization (sqlite takes an internal per-connection mutex on
     * every API call). Must run before any sqlite3_open(). The overhead
     * of one uncontended mutex lock per call is negligible compared to
     * the I/O sqlite performs. See crash report
     * dsco-2026-04-08-202809.ips. */
    sqlite3_config(SQLITE_CONFIG_SERIALIZED);
    sqlite3_initialize();

    /* §2: Arena allocator — scratch (per-turn) + session (per-run) */
    arena_subsystem_init();
    g_arena_ready = true;

    /* §6: Event loop — kqueue on macOS, poll fallback */
    g_ev_loop = ev_loop_new();

    /* §3: Bytecode VM — computed-goto dispatch for tool routing */
    vm_init(&g_vm);

    /* §1/§7: Cooperative scheduler — priority-aware task scheduling */
    sched_init(&g_scheduler);
    g_scheduler_ready = true;

    /* §8: Embedded persistence — SQLite VFS layer */
    char vfs_path[512];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(vfs_path, sizeof(vfs_path), "%s/.dsco/vfs.db", home);
        g_vfs = vfs_open(vfs_path);
    }

    /* Cross-module wiring: connect subsystems to each other */
    ooda_set_scheduler(&g_scheduler);  /* §1/§7→OODA: schedule phases as tasks */
    topology_set_scheduler(&g_scheduler); /* §1/§7→topology: schedule nodes as tasks */
    if (g_vfs) {
        /* §8→baseline: mirror baseline events to VFS event log */
        baseline_set_vfs(g_vfs);
        /* §8→memory: persist semantic memories to VFS */
        memory_store_set_vfs(g_vfs);
        /* §9→memory: embedding-backed semantic search */
        memory_store_set_vecstore(vecstore_open(g_vfs, "memory"));
        /* §8→governance: persist audit trail to VFS event log */
        governance_set_vfs(g_vfs);
        /* §8→killswitch: persist kill switch state to VFS */
        killswitch_set_vfs(g_vfs);
        /* §8→tools: deterministic tool result cache */
        tools_set_vfs(g_vfs);
        /* §8→semantic: cache TF-IDF index metadata and log queries */
        semantic_set_vfs(g_vfs);
        /* §8→talons: persist strategy win rates and tournament results */
        talons_set_vfs(g_vfs);
    }

    /* §6→IPC: non-blocking heartbeat via event loop timer */
    ipc_set_event_loop(g_ev_loop);
}

static bool main_argv_has(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

static bool main_argv_has_value(int argc, char **argv, const char *flag,
                                const char **out_value) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            if (out_value) *out_value = argv[i + 1];
            return true;
        }
    }
    return false;
}

static dsco_profile_t main_runtime_profile(int argc, char **argv) {
    dsco_profile_t profile = DSCO_PROFILE_FULL;

    const char *base = argv && argv[0] ? strrchr(argv[0], '/') : NULL;
    base = base ? base + 1 : (argv && argv[0] ? argv[0] : "");
    if (strcmp(base, "dsco-lite") == 0)
        profile = DSCO_PROFILE_LITE;

    dsco_profile_t parsed;
    const char *env_profile = getenv("DSCO_PROFILE");
    if (dsco_profile_parse(env_profile, &parsed))
        profile = parsed;

    const char *cli_profile = NULL;
    if (main_argv_has_value(argc, argv, "--profile", &cli_profile) &&
        dsco_profile_parse(cli_profile, &parsed)) {
        profile = parsed;
    }

    if (getenv("DSCO_WORKER") ||
        main_argv_has(argc, argv, "--worker") ||
        main_argv_has(argc, argv, "--worker-lite")) {
        profile = DSCO_PROFILE_WORKER;
    }

    return profile;
}

static dsco_caps_t main_plan_startup_caps(int argc, char **argv,
                                          dsco_profile_t profile) {
    if (profile == DSCO_PROFILE_FULL)
        return DSCO_CAP_FULL;

    dsco_caps_t caps = DSCO_CAP_TRACE;
    if (profile == DSCO_PROFILE_WORKER)
        caps |= DSCO_CAP_PROVIDER | DSCO_CAP_TOOLS;

    bool has_prompt = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) break;
        if (strcmp(argv[i], "--tool-exec") == 0) {
            caps |= DSCO_CAP_TOOLS;
            if (i + 2 < argc) i += 2;
            continue;
        }
        if (strcmp(argv[i], "--tools-json") == 0) {
            caps |= DSCO_CAP_TOOLS;
            continue;
        }
        if (strcmp(argv[i], "--models-json") == 0 ||
            strcmp(argv[i], "--version") == 0 ||
            strcmp(argv[i], "-v") == 0 ||
            strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h") == 0) {
            continue;
        }
        if (argv[i][0] != '-') {
            if (i == 1 && (strcmp(argv[i], "login") == 0 ||
                           strcmp(argv[i], "status") == 0 ||
                           strcmp(argv[i], "tools") == 0 ||
                           strcmp(argv[i], "connect") == 0 ||
                           strcmp(argv[i], "mux") == 0)) {
                continue;
            }
            has_prompt = true;
            break;
        }
        if ((strcmp(argv[i], "-m") == 0 ||
             strcmp(argv[i], "-k") == 0 ||
             strcmp(argv[i], "-M") == 0 ||
             strcmp(argv[i], "--worker-model") == 0 ||
             strcmp(argv[i], "--exec") == 0 ||
             strcmp(argv[i], "-e") == 0 ||
             strcmp(argv[i], "--provider") == 0 ||
             strcmp(argv[i], "--profile") == 0 ||
             strcmp(argv[i], "--timeline-port") == 0 ||
             strcmp(argv[i], "--timeline-instance") == 0 ||
             strcmp(argv[i], "--topology") == 0) && i + 1 < argc) {
            i++;
        }
    }

    if (has_prompt ||
        main_argv_has(argc, argv, "-i") ||
        main_argv_has(argc, argv, "--interactive") ||
        main_argv_has(argc, argv, "-e") ||
        main_argv_has(argc, argv, "--exec") ||
        main_argv_has(argc, argv, "--provider")) {
        caps |= DSCO_CAP_PROVIDER | DSCO_CAP_TOOLS;
    }

    if (main_argv_has(argc, argv, "-i") ||
        main_argv_has(argc, argv, "--interactive") ||
        main_argv_has(argc, argv, "--ui") ||
        main_argv_has(argc, argv, "-ui")) {
        caps |= DSCO_CAP_TUI;
    }

    if (main_argv_has(argc, argv, "--orchestrate") ||
        main_argv_has(argc, argv, "-O") ||
        main_argv_has(argc, argv, "--mux") ||
        main_argv_has(argc, argv, "mux") ||
        main_argv_has(argc, argv, "--topology") ||
        main_argv_has(argc, argv, "--topology-auto") ||
        main_argv_has(argc, argv, "--timeline-server")) {
        caps |= DSCO_CAP_PROVIDER | DSCO_CAP_TOOLS | DSCO_CAP_VOS |
                DSCO_CAP_IPC | DSCO_CAP_MEMORY;
    }

    return caps;
}

void dsco_startup_init(dsco_profile_t profile, dsco_caps_t caps) {
    if (g_startup_initialized) return;
    heartbeat_set_phase("startup_init");
    if (profile == DSCO_PROFILE_FULL)
        caps = DSCO_CAP_FULL;
    if (profile == DSCO_PROFILE_WORKER)
        caps &= ~(DSCO_CAP_SECURITY | DSCO_CAP_TRUST);
    perf_set_startup_context(profile, caps);

    /* Register extension backends early so skills/tools can resolve capabilities. */
    numerical_backend_register(&numerical_backend_gsl);
    eigen_backend_init();
    fftw_backend_init();

    bool is_worker = profile == DSCO_PROFILE_WORKER;

    /* Clean first paint: wipe whatever the previous program left on the
     * terminal so the startup banner doesn't bleed through a stale screen. */
    if (!is_worker) tui_screen_reset_full();

    if (caps & DSCO_CAP_SECURITY) {
        if (!is_worker) heartbeat_set_phase("startup_security");
        tamper_init();          /* must be first: deny ptrace, hash code, watch binary */
        perf_mark("tamper");
        {   /* audit log opens here so every subsequent init step is captured */
            char alog_path[4096];
            const char *h = getenv("HOME");
            if (!h) h = "/tmp";
            snprintf(alog_path, sizeof(alog_path), "%s/.dsco/audit.log", h);
            audit_log_global_init(alog_path);
        }
        perf_mark("audit");
        if (!is_worker) {
            heartbeat_set_phase("startup_secure_store");
            uint8_t se_key[32] = {0};
            int timed_out = 0;
            bool show_secure_wait = isatty(STDERR_FILENO);
            if (show_secure_wait) {
                fprintf(stderr, "dsco: initializing secure store...\n");
                fflush(stderr);
            }
            if (!init_secure_store_required(se_key, &timed_out)) {
                audit_log("se_store", timed_out
                          ? "secure store init timed out"
                          : "secure store init failed");
                fprintf(stderr,
                        "error: secure store initialization %s; refusing to start\n"
                        "  unlock the login keychain/Secure Enclave and retry\n"
                        "  set DSCO_SECURE_STORE_AUTH_UI=1 if macOS should show an auth prompt\n"
                        "  set DSCO_SECURE_STORE_TIMEOUT_MS=<ms> to allow more time\n",
                        timed_out ? "timed out" : "failed");
                main_zero32(se_key);
                exit(1);
            }
            sealed_store_set_master_key(se_key);
            main_zero32(se_key);
            perf_mark("secure_store");
        }
        tamper_register_wiper((tamper_wiper_fn)se_store_wipe, NULL);
        if (!is_worker) heartbeat_set_phase("startup_sealed_store");
        if (!is_worker) sealed_store_init();
        perf_mark("sealed_store");
        env_guard_init();
        audit_log("startup", is_worker ? "dsco worker init" : "dsco init");
        if (!is_worker) {
            heartbeat_set_phase("startup_heartbeat");
            heartbeat_start();
        }

        /* ── Priority 1/3/4: plan optimizer, cost model, plan cache ── */
        if (!is_worker) {
            heartbeat_set_phase("startup_cost_state");
            cost_model_init();
            plan_cache_init();
        }

#if defined(HAVE_LIBSODIUM)
        if (!is_worker) {
            heartbeat_set_phase("startup_mesh");
            /* ── Mesh P2P layer ─────────────────────────────────────── */
            uint16_t mesh_port = 7337;
            const char *mp_env = getenv("DSCO_MESH_PORT");
            if (mp_env && atoi(mp_env) > 0) mesh_port = (uint16_t)atoi(mp_env);

            g_mesh_node = mesh_node_create(mesh_port);
            if (g_mesh_node) {
                /* Callbacks wired in net_tool.c via dsco_net_node() */
                if (mesh_node_start(g_mesh_node)) {
                    audit_log("net", "mesh started");
                    /* Bootstrap: discover peers via mDNS + ~/.dsco/peers.txt */
                    peer_bootstrap_init(g_mesh_node, mesh_port);

                    /* ── DHT peer discovery (opt-in via DSCO_DHT_SWARM) ──────
                     * Joins a private Kademlia overlay; discovered peers are
                     * written to ~/.dsco/peers.txt and dialed over the mesh. */
                    const char *swarm = getenv("DSCO_DHT_SWARM");
                    if (swarm && *swarm) {
                        uint16_t dht_port = 7600;
                        const char *dp = getenv("DSCO_DHT_PORT");
                        if (dp && atoi(dp) > 0) dht_port = (uint16_t)atoi(dp);
                        dsco_dht_config_t dc = {
                            .udp_port  = dht_port,
                            .mesh_port = mesh_port,
                            .swarm_key = swarm,
                        };
                        if (dsco_dht_start(&dc))
                            audit_log("net", "dht started");
                    }
                } else {
                    audit_log("net", "mesh start failed");
                    mesh_node_destroy(g_mesh_node);
                    g_mesh_node = NULL;
                }
            }
        }
#endif /* HAVE_LIBSODIUM */

#if defined(HAVE_MBEDTLS) && defined(HAVE_LIBSODIUM)
        if (!is_worker) {
            heartbeat_set_phase("startup_http");
            /* ── HTTP/TLS API server ─────────────────────────────────── */
            uint16_t http_port = NETSRV_DEFAULT_PORT;
            const char *hp_env = getenv("DSCO_HTTP_PORT");
            if (hp_env && atoi(hp_env) > 0) http_port = (uint16_t)atoi(hp_env);

            /* Auto-generate TLS cert if not present */
            char cert_path[512], key_path[512];
            const char *home = getenv("HOME");
            snprintf(cert_path, sizeof(cert_path), "%s/.dsco/server.crt", home ? home : "/tmp");
            snprintf(key_path,  sizeof(key_path),  "%s/.dsco/server.key", home ? home : "/tmp");

            if (access(cert_path, F_OK) != 0)
                netsrv_gen_tls_cert(cert_path, key_path, "dsco-node");

            g_net_server = netsrv_create(http_port, true, cert_path, key_path);
            if (g_net_server) {
                /* Routes registered by dsco_net_routes_register() in tools.c */
                dsco_net_routes_register(g_net_server);
                if (netsrv_start(g_net_server)) {
                    audit_log("net", "http server started");
                } else {
                    audit_log("net", "http server start failed");
                    netsrv_destroy(g_net_server);
                    g_net_server = NULL;
                }
            }
        }
#endif /* HAVE_MBEDTLS && HAVE_LIBSODIUM */
        perf_mark("env_heartbeat");
    }

    if (caps & DSCO_CAP_ACCEL) {
        if (!is_worker) heartbeat_set_phase("startup_accel");
        dsco_mlx_init();
        dsco_accel_init();
        dsco_pool_global_init(0);
        perf_mark("accel_pool");
    }

    if ((caps & DSCO_CAP_TRUST) || (caps & DSCO_CAP_ACCEL)) {
        dsco_fingerprint_refresh();
        if ((caps & DSCO_CAP_TRUST) && !is_worker) {
            dsco_trust_config_t tcfg;
            dsco_trust_default_config(&tcfg);
            if (!tcfg.opt_out && dsco_trust_init(&tcfg) == 0) {
                dsco_trust_emit_attest();
            }
        }
        perf_mark("fingerprint_trust");
    }

    if ((caps & DSCO_CAP_ACCEL) && getenv("DSCO_ACCEL_BANNER")) {
        const dsco_accel_info_t *ai = dsco_accel_info();
        const dsco_fingerprint_t *fp = dsco_fingerprint_get();
        char fpline[256];
        if (fp) dsco_fingerprint_summary(fp, fpline, sizeof(fpline));
        if (ai) fprintf(stderr, "%s%s%s\n  host: %s\n  trust: %s\n",
                        ai->banner,
                        dsco_mlx_library_path() ? " | mlx=" : "",
                        dsco_mlx_library_path() ? dsco_mlx_library_path() : "",
                        fpline,
                        dsco_trust_is_active() ? dsco_trust_endpoint() : "off");
    }

    if ((caps & DSCO_CAP_TUI) && isatty(STDIN_FILENO)) {
        extern void tui_lock_engage(void);
        const char *idle_s_str = getenv("DSCO_IDLE_LOCK_S");
        int idle_s = idle_s_str ? atoi(idle_s_str) : 300;
        if (idle_s > 0) {
            presence_init(idle_s, (presence_lock_fn)tui_lock_engage, NULL);
            presence_start();
        }
        perf_mark("presence");
    }

    (void)atexit(main_atexit_handler);

    if (caps & DSCO_CAP_TRACE) {
        init_trace_runtime();
        perf_mark("trace");
    }

    if (caps & DSCO_CAP_VOS) {
        init_vos_subsystems();
        perf_mark("vos");
    } else if (caps & DSCO_CAP_TOOLS) {
        vm_init(&g_vm);
        perf_mark("vm");
    }

    g_startup_initialized = true;
    if (!is_worker) heartbeat_set_phase("startup_ready");
    perf_mark("startup ready");
}

/* ── Executor registry ─────────────────────────────────────────────── */

/* External CLI executors (claude, codex) */
typedef struct {
    const char *name;          /* "claude", "codex" */
    const char *bin;           /* binary name for PATH lookup */
    const char *oneshot_cmd;   /* subcommand for oneshot, NULL if none */
    const char *model_flag;    /* flag name for model, NULL = no model support */
    const char *print_flag;    /* flag to enable print/oneshot mode, NULL if none */
    const char *desc;          /* human label */
} exec_reg_t;

static const exec_reg_t EXEC_REGISTRY[] = {
    { "claude", "claude", NULL,   "--model", "-p",   "Claude Code (Anthropic)" },
    { "codex",  "codex",  "exec", "-m",      NULL,   "Codex CLI (OpenAI)"      },
    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/* Native API providers (these use dsco's built-in streaming) */

/* Capability flags */
#define CAP_TOOLS       (1 << 0)   /* function/tool calling */
#define CAP_MULTITURN   (1 << 1)   /* multi-turn conversation */
#define CAP_STREAMING   (1 << 2)   /* SSE streaming */
#define CAP_VISION      (1 << 3)   /* image input */
#define CAP_THINKING    (1 << 4)   /* extended thinking / reasoning */
#define CAP_JSON        (1 << 5)   /* structured JSON output */
#define CAP_CACHE       (1 << 6)   /* prompt caching */

typedef struct {
    const char *name;
    const char *desc;
    const char *env_key;
    const char *example_model;
    int         caps;              /* CAP_* bitmask */
    int         tier;              /* 1-4 capability tier */
} native_provider_t;

static const native_provider_t NATIVE_PROVIDERS[] = {
    { "anthropic",  "Anthropic Claude API",      "ANTHROPIC_API_KEY",   "claude-opus-4-6",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_VISION|CAP_THINKING|CAP_JSON|CAP_CACHE, 4 },
    { "openai",     "OpenAI API",                "OPENAI_API_KEY",      "gpt-4.1",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_VISION|CAP_JSON, 3 },
    { "openrouter", "OpenRouter (multi-model)",   "OPENROUTER_API_KEY", "z-ai/glm-5.2",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_VISION|CAP_JSON, 4 },
    { "google",     "Google Gemini API",         "GOOGLE_API_KEY",      "gemini-2.5-pro",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_VISION|CAP_JSON, 3 },
    { "groq",       "Groq (fast inference)",     "GROQ_API_KEY",        "llama-3.3-70b-versatile",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_JSON, 2 },
    { "deepseek",   "DeepSeek API",              "DEEPSEEK_API_KEY",    "deepseek-chat",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_THINKING|CAP_JSON, 3 },
    { "mistral",    "Mistral AI API",            "MISTRAL_API_KEY",     "mistral-large-latest",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_JSON, 3 },
    { "xai",        "xAI Grok API",              "XAI_API_KEY",         "grok-4-fast",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_VISION|CAP_THINKING|CAP_JSON, 3 },
    { "together",   "Together AI",               "TOGETHER_API_KEY",    "meta-llama/Llama-4-Maverick-17B-128E-Instruct-FP8",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING, 2 },
    { "perplexity", "Perplexity AI",             "PERPLEXITY_API_KEY",  "sonar-pro",
      CAP_MULTITURN|CAP_STREAMING, 2 },
    { "cerebras",   "Cerebras (fast inference)",  "CEREBRAS_API_KEY",   "qwen-3-235b-a22b-instruct-2507",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING, 2 },
    { "cohere",     "Cohere API",                "COHERE_API_KEY",      "command-a-03-2025",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_JSON, 3 },
    { "moonshot",   "Moonshot Kimi API",         "MOONSHOT_API_KEY",    "kimi-k2.7-code-highspeed",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_VISION|CAP_THINKING|CAP_JSON, 4 },
    { NULL, NULL, NULL, NULL, 0, 0 }
};

static const exec_reg_t *exec_find(const char *name) {
    for (int i = 0; EXEC_REGISTRY[i].name; i++) {
        if (strcmp(EXEC_REGISTRY[i].name, name) == 0)
            return &EXEC_REGISTRY[i];
    }
    return NULL;
}

static const native_provider_t *native_find(const char *name) {
    for (int i = 0; NATIVE_PROVIDERS[i].name; i++) {
        if (strcmp(NATIVE_PROVIDERS[i].name, name) == 0)
            return &NATIVE_PROVIDERS[i];
    }
    return NULL;
}

static bool exec_bin_available(const char *bin) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", bin);
    return system(cmd) == 0;
}

static bool main_claude_exec_ready(void) {
    const exec_reg_t *e = exec_find("claude");
    if (!e || !exec_bin_available(e->bin)) return false;
    const char *key = provider_resolve_request_api_key("anthropic", NULL);
    return key && key[0];
}

static bool main_codex_exec_ready(void) {
    const exec_reg_t *e = exec_find("codex");
    if (!e || !exec_bin_available(e->bin)) return false;

    const char *home = getenv("HOME");
    if (home && home[0]) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/.codex/auth.json", home);
        if (access(path, R_OK) == 0) return true;
    }
    return false;
}

static bool prompt_looks_code_task(const char *prompt) {
    if (!prompt || !prompt[0]) return false;

    char lower[1024];
    size_t n = strlen(prompt);
    if (n >= sizeof(lower)) n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++)
        lower[i] = (char)tolower((unsigned char)prompt[i]);
    lower[n] = '\0';

    const char *keywords[] = {
        "code", "bug", "debug", "refactor", "compile",
        "test", "function", "repo", "patch", "implement",
        "file", "fix", "c++", "python", "javascript", NULL
    };
    for (int i = 0; keywords[i]; i++) {
        if (strstr(lower, keywords[i])) return true;
    }
    return false;
}

static bool model_supports_executor(const char *model, const char *executor_name) {
    if (!model || !model[0] || !executor_name || !executor_name[0]) return true;
    const char *resolved = model_resolve_alias(model);
    const char *family = provider_model_family(resolved);
    if (strcmp(executor_name, "claude") == 0)
        return strcmp(family, "anthropic") == 0;
    if (strcmp(executor_name, "codex") == 0)
        return strcmp(family, "openai") == 0;
    return false;
}

static void print_executor_model_mismatch(const exec_reg_t *e, const char *model) {
    const char *resolved = model_resolve_alias(model);
    const char *family = provider_model_family(resolved);
    const char *provider = provider_detect(resolved, NULL);
    fprintf(stderr, "error: executor '%s' cannot run model '%s' (family: %s)\n",
            e && e->name ? e->name : "unknown",
            resolved && resolved[0] ? resolved : "(none)",
            family && family[0] ? family : "unknown");
    fprintf(stderr, "  use --provider %s or --exec auto for this model\n",
            provider && provider[0] ? provider : "openrouter");
}

static const exec_reg_t *select_auto_executor(const char *model,
                                              const char *prompt,
                                              bool user_set_model) {
    const exec_reg_t *claude = exec_find("claude");
    const exec_reg_t *codex = exec_find("codex");
    bool claude_ready = main_claude_exec_ready();
    bool codex_ready = main_codex_exec_ready();

    if (user_set_model) {
        if (claude_ready && model_supports_executor(model, "claude")) return claude;
        if (codex_ready && model_supports_executor(model, "codex")) return codex;
        return NULL;
    }

    if (prompt_looks_code_task(prompt) && codex_ready) return codex;
    if (claude_ready) return claude;
    if (codex_ready) return codex;
    return NULL;
}

static const char *default_model_for_executor(const exec_reg_t *e) {
    if (!e) return NULL;
    if (strcmp(e->name, "claude") == 0) return "claude-sonnet-4-6";
    if (strcmp(e->name, "codex") == 0) return "gpt-5.3-codex-spark";
    return NULL;
}

static const char *normalize_model_for_executor(const exec_reg_t *e, const char *model) {
    static char normalized[128];
    const char *resolved = model_resolve_alias(model);
    if (!e || !resolved || !resolved[0]) return resolved;

    if (strcmp(e->name, "claude") == 0 &&
        strncmp(resolved, "anthropic/", 10) == 0) {
        const char *src = resolved + 10;
        size_t n = strlen(src);
        if (n >= sizeof(normalized)) n = sizeof(normalized) - 1;
        for (size_t i = 0; i < n; i++) {
            normalized[i] = (src[i] == '.') ? '-' : src[i];
        }
        normalized[n] = '\0';
        return normalized;
    }

    if (strcmp(e->name, "codex") == 0 &&
        strncmp(resolved, "openai/", 7) == 0) {
        snprintf(normalized, sizeof(normalized), "%s", resolved + 7);
        return normalized;
    }

    return resolved;
}

static void exec_list(void) {
    fprintf(stderr, "\n  \033[1mExternal CLIs\033[0m\n");
    fprintf(stderr, "  %-12s %-28s %s\n", "NAME", "DESCRIPTION", "STATUS");
    fprintf(stderr, "  %-12s %-28s %s\n", "────", "───────────", "──────");
    for (int i = 0; EXEC_REGISTRY[i].name; i++) {
        const exec_reg_t *e = &EXEC_REGISTRY[i];
        bool avail = exec_bin_available(e->bin);
        fprintf(stderr, "  %-12s %-28s %s%s%s\n",
                e->name, e->desc,
                avail ? "\033[32m" : "\033[31m",
                avail ? "ready" : "not found",
                "\033[0m");
    }
    fprintf(stderr, "\n  \033[1mNative API Providers\033[0m\n");
    fprintf(stderr, "  %-12s %-24s %-10s %-18s %s\n",
            "NAME", "DESCRIPTION", "STATUS", "CAPABILITIES", "DEFAULT MODEL");
    fprintf(stderr, "  %-12s %-24s %-10s %-18s %s\n",
            "────", "───────────", "──────", "────────────", "─────────────");
    for (int i = 0; NATIVE_PROVIDERS[i].name; i++) {
        const native_provider_t *np = &NATIVE_PROVIDERS[i];
        bool has_key = provider_has_usable_key(np->name, NULL);

        /* Build capability string */
        char caps[64] = "";
        int clen = 0;
        if (np->caps & CAP_TOOLS)    clen += snprintf(caps+clen, sizeof(caps)-clen, "T");
        if (np->caps & CAP_MULTITURN)clen += snprintf(caps+clen, sizeof(caps)-clen, "M");
        if (np->caps & CAP_STREAMING)clen += snprintf(caps+clen, sizeof(caps)-clen, "S");
        if (np->caps & CAP_VISION)   clen += snprintf(caps+clen, sizeof(caps)-clen, "V");
        if (np->caps & CAP_THINKING) clen += snprintf(caps+clen, sizeof(caps)-clen, "R");
        if (np->caps & CAP_JSON)     clen += snprintf(caps+clen, sizeof(caps)-clen, "J");
        if (np->caps & CAP_CACHE)    clen += snprintf(caps+clen, sizeof(caps)-clen, "C");
        (void)clen;

        fprintf(stderr, "  %-12s %-24s %s%-10s%s %-18s %s\n",
                np->name, np->desc,
                has_key ? "\033[32m" : "\033[2m",
                has_key ? "ready" : "no key",
                "\033[0m",
                caps,
                np->example_model);
    }
    fprintf(stderr, "\n  %sCapabilities: T=tools M=multi-turn S=streaming V=vision R=reasoning J=json C=cache%s\n",
            "\033[2m", "\033[0m");
    fprintf(stderr, "  %sSpecial: auto, smart, list, bench, bench-tools, smoke, smoke-full%s\n\n",
            "\033[2m", "\033[0m");
}

static void exec_prepare_env(const exec_reg_t *e, const char *fallback_api_key) {
    if (!e || strcmp(e->name, "claude") != 0) return;

    const char *resolved = provider_resolve_request_api_key("anthropic", fallback_api_key);
    if (!resolved || !resolved[0] ||
        !llm_anthropic_uses_claude_code_auth(resolved)) {
        return;
    }

    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("ANTHROPIC_AUTH_TOKEN");
    unsetenv("ANTHROPIC_BASE_URL");
    unsetenv("ANTHROPIC_MODEL");
    setenv("DSCO_CLAUDE_CODE_OAUTH_TOKEN", resolved, 1);
    setenv("CLAUDE_CODE_OAUTH_TOKEN", resolved, 1);
}

typedef enum {
    SMOKE_EXECUTOR = 0,
    SMOKE_NATIVE,
    SMOKE_OPENROUTER
} smoke_kind_t;

typedef struct {
    const char *label;
    smoke_kind_t kind;
    const char *route;
    const char *model;
    bool full_only;
} smoke_case_t;

typedef struct {
    bool skipped;
    bool ok;
    bool timed_out;
    int exit_code;
    double elapsed_sec;
    char detail[224];
} smoke_result_t;

static double main_now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static bool main_env_truthy(const char *value) {
    return value && (value[0] == '1' || strcasecmp(value, "true") == 0 ||
                     strcasecmp(value, "yes") == 0);
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t done_cv;
    bool done;
    bool ok;
    uint8_t key[32];
} secure_store_init_state_t;

static void main_zero32(uint8_t key[32]) {
    volatile uint8_t *z = key;
    for (int i = 0; i < 32; i++) z[i] = 0;
}

static int secure_store_timeout_ms(void) {
    const char *env = getenv("DSCO_SECURE_STORE_TIMEOUT_MS");
    if (!env || !env[0]) return 20000;
    char *end = NULL;
    long ms = strtol(env, &end, 10);
    if (!end || *end != '\0' || ms < 1000 || ms > 120000) return 20000;
    return (int)ms;
}

static void secure_store_deadline(struct timespec *ts, int timeout_ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ms / 1000;
    ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static void *secure_store_init_worker(void *arg) {
    secure_store_init_state_t *st = arg;
    uint8_t key[32] = {0};
    bool ok = se_store_init(key);

    pthread_mutex_lock(&st->lock);
    st->ok = ok;
    if (ok) memcpy(st->key, key, sizeof(st->key));
    st->done = true;
    pthread_cond_signal(&st->done_cv);
    pthread_mutex_unlock(&st->lock);

    main_zero32(key);
    return NULL;
}

static bool init_secure_store_required(uint8_t out_key[32], int *timed_out) {
    if (timed_out) *timed_out = 0;
    if (!out_key) return false;
    memset(out_key, 0, 32);

    secure_store_init_state_t *st = calloc(1, sizeof(*st));
    if (!st) return false;
    pthread_mutex_init(&st->lock, NULL);
    pthread_cond_init(&st->done_cv, NULL);

    pthread_t tid;
    if (pthread_create(&tid, NULL, secure_store_init_worker, st) != 0) {
        pthread_cond_destroy(&st->done_cv);
        pthread_mutex_destroy(&st->lock);
        free(st);
        return false;
    }

    struct timespec deadline;
    secure_store_deadline(&deadline, secure_store_timeout_ms());

    pthread_mutex_lock(&st->lock);
    while (!st->done) {
        int rc = pthread_cond_timedwait(&st->done_cv, &st->lock, &deadline);
        if (rc == ETIMEDOUT && !st->done) {
            if (timed_out) *timed_out = 1;
            pthread_mutex_unlock(&st->lock);
            pthread_detach(tid);
            /* Intentionally leak st on timeout: the blocked Security.framework
             * call may still complete while the process is exiting. */
            return false;
        }
    }

    bool ok = st->ok;
    if (ok) memcpy(out_key, st->key, 32);
    main_zero32(st->key);
    pthread_mutex_unlock(&st->lock);

    pthread_join(tid, NULL);
    pthread_cond_destroy(&st->done_cv);
    pthread_mutex_destroy(&st->lock);
    free(st);
    return ok;
}

static void smoke_sanitize_text(const char *src, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;

    size_t pos = 0;
    bool last_space = false;
    for (size_t i = 0; src[i] && pos + 1 < dst_len; ) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == 0x1b) {
            i++;
            if (src[i] == '[') {
                i++;
                while (src[i] && !isalpha((unsigned char)src[i])) i++;
                if (src[i]) i++;
            }
            continue;
        }
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            if (!last_space && pos + 1 < dst_len) {
                dst[pos++] = ' ';
                last_space = true;
            }
            i++;
            continue;
        }
        if (isspace(ch)) {
            if (!last_space && pos + 1 < dst_len) {
                dst[pos++] = ' ';
                last_space = true;
            }
            i++;
            continue;
        }
        dst[pos++] = (char)ch;
        last_space = false;
        i++;
    }
    dst[pos] = '\0';
}

static void smoke_extract_detail(const char *raw, char *detail, size_t detail_len) {
    if (!detail || detail_len == 0) return;
    detail[0] = '\0';
    if (!raw || !raw[0]) {
        snprintf(detail, detail_len, "no output");
        return;
    }

    const char *error_line = strstr(raw, "error: stream failed");
    if (!error_line) error_line = strstr(raw, "HTTP 401");
    if (!error_line) error_line = strstr(raw, "HTTP 402");
    if (!error_line) error_line = strstr(raw, "HTTP 403");
    if (!error_line) error_line = strstr(raw, "HTTP 429");
    if (!error_line) error_line = strstr(raw, "invalid");
    if (!error_line) error_line = strstr(raw, "Authentication Fails");
    if (!error_line) error_line = strstr(raw, "dsco:");
    if (error_line && strstr(error_line, "trace log")) error_line = NULL;
    if (!error_line) error_line = strstr(raw, "error:");
    if (error_line) {
        const char *line_end = strchr(error_line, '\n');
        char line[256];
        size_t n = line_end ? (size_t)(line_end - error_line) : strlen(error_line);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, error_line, n);
        line[n] = '\0';
        smoke_sanitize_text(line, detail, detail_len);
        return;
    }

    const char *auth = strstr(raw, "[auth]");
    if (auth) {
        const char *line_end = strchr(auth, '\n');
        char line[256];
        size_t n = line_end ? (size_t)(line_end - auth) : strlen(auth);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, auth, n);
        line[n] = '\0';
        smoke_sanitize_text(line, detail, detail_len);
        return;
    }

    smoke_sanitize_text(raw, detail, detail_len);
    if (detail[0] == '\0')
        snprintf(detail, detail_len, "output empty");
}

static void smoke_extract_success_detail(const smoke_case_t *sc,
                                         const char *raw,
                                         char *detail,
                                         size_t detail_len) {
    if (!detail || detail_len == 0) return;
    detail[0] = '\0';

    if (raw && raw[0]) {
        const char *auth = strstr(raw, "[auth]");
        if (auth) {
            const char *line_end = strchr(auth, '\n');
            char line[256];
            size_t n = line_end ? (size_t)(line_end - auth) : strlen(auth);
            if (n >= sizeof(line)) n = sizeof(line) - 1;
            memcpy(line, auth, n);
            line[n] = '\0';
            smoke_sanitize_text(line, detail, detail_len);
            if (detail[0]) return;
        }
    }

    if (sc && sc->kind == SMOKE_EXECUTOR) {
        snprintf(detail, detail_len, "completed");
        return;
    }

    if (raw && strstr(raw, "DSCOSMOKEOK")) {
        snprintf(detail, detail_len, "completed");
        return;
    }

    smoke_extract_detail(raw, detail, detail_len);
}

static bool smoke_case_ready(const smoke_case_t *sc, char *reason, size_t reason_len) {
    if (!reason || reason_len == 0) return false;
    reason[0] = '\0';
    if (!sc) {
        snprintf(reason, reason_len, "missing case");
        return false;
    }

    switch (sc->kind) {
        case SMOKE_EXECUTOR:
            if (strcmp(sc->route, "claude") == 0) {
                if (main_claude_exec_ready()) return true;
                snprintf(reason, reason_len, "Claude Code not ready");
                return false;
            }
            if (strcmp(sc->route, "codex") == 0) {
                if (main_codex_exec_ready()) return true;
                snprintf(reason, reason_len, "Codex not ready");
                return false;
            }
            snprintf(reason, reason_len, "unknown executor");
            return false;
        case SMOKE_NATIVE:
            if (provider_has_usable_key(sc->route, NULL)) return true;
            snprintf(reason, reason_len, "no credential");
            return false;
        case SMOKE_OPENROUTER:
            if (provider_has_usable_key("openrouter", NULL)) return true;
            snprintf(reason, reason_len, "no OPENROUTER_API_KEY");
            return false;
    }

    snprintf(reason, reason_len, "unhandled");
    return false;
}

static smoke_result_t smoke_run_case(const char *self_path, const smoke_case_t *sc,
                                     int timeout_sec) {
    static const char *smoke_prompt = "Reply with exactly DSCOSMOKEOK";
    smoke_result_t result;
    memset(&result, 0, sizeof(result));
    result.exit_code = -1;

    char reason[96];
    if (!smoke_case_ready(sc, reason, sizeof(reason))) {
        result.skipped = true;
        snprintf(result.detail, sizeof(result.detail), "%s", reason);
        return result;
    }

    const char *argv_exec[12];
    int argc_exec = 0;
    argv_exec[argc_exec++] = self_path;
    argv_exec[argc_exec++] = "--cheap";
    if (sc->kind == SMOKE_EXECUTOR || sc->kind == SMOKE_NATIVE) {
        argv_exec[argc_exec++] = "-e";
        argv_exec[argc_exec++] = sc->route;
    }
    argv_exec[argc_exec++] = "-m";
    argv_exec[argc_exec++] = sc->model;
    argv_exec[argc_exec++] = smoke_prompt;
    argv_exec[argc_exec] = NULL;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(result.detail, sizeof(result.detail), "pipe failed");
        return result;
    }

    double started = main_now_sec();
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(result.detail, sizeof(result.detail), "fork failed");
        return result;
    }

    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        setenv("NO_COLOR", "1", 1);
        setenv("TERM", "dumb", 1);
        setenv("DSCO_MAX_AGENT_TURNS", "1", 1);
        setenv("DSCO_DISABLE_DEFAULT_FALLBACKS", "1", 1);
        setenv("DSCO_OR_MAX_TOOLS", "0", 1);
        setenv("DSCO_OR_DISABLE_TOOLS", "1", 1);
        setenv("DSCO_DEBUG_AUTH", "1", 1);
        setenv("DSCO_TRACE", "0", 1);
        unsetenv("DSCO_TRACE_STDERR");
        execvp(self_path, (char *const *)argv_exec);
        perror(self_path);
        _exit(127);
    }

    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    char raw[4096];
    size_t used = 0;
    bool child_done = false;
    bool pipe_eof = false;
    int status = 0;
    while (1) {
        if (!child_done) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) child_done = true;
        }

        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = pipefd[0];
        pfd.events = POLLIN | POLLHUP;
        (void)poll(&pfd, 1, 150);

        while (used + 1 < sizeof(raw)) {
            ssize_t nread = read(pipefd[0], raw + used, sizeof(raw) - used - 1);
            if (nread > 0) {
                used += (size_t)nread;
                raw[used] = '\0';
                continue;
            }
            if (nread == 0) pipe_eof = true;
            break;
        }

        if (child_done && pipe_eof) break;
        if ((main_now_sec() - started) > timeout_sec) {
            result.timed_out = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
    }

    close(pipefd[0]);
    raw[used] = '\0';
    result.elapsed_sec = main_now_sec() - started;
    if (result.timed_out) {
        snprintf(result.detail, sizeof(result.detail), "timed out");
        return result;
    }

    if (WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exit_code = 128 + WTERMSIG(status);

    result.ok = (result.exit_code == 0 && strstr(raw, "DSCOSMOKEOK") != NULL);
    if (result.ok)
        smoke_extract_success_detail(sc, raw, result.detail, sizeof(result.detail));
    else
        smoke_extract_detail(raw, result.detail, sizeof(result.detail));
    return result;
}

static int run_provider_smoke(const char *self_path, bool full) {
    static const smoke_case_t smoke_cases[] = {
        { "Claude Code",      SMOKE_EXECUTOR,   "claude",     "anthropic/claude-sonnet-4.6", false },
        { "Codex",            SMOKE_EXECUTOR,   "codex",      "openai/gpt-5.4",              false },
        { "Anthropic",        SMOKE_NATIVE,     "anthropic",  "claude-sonnet-4-6",           false },
        { "OpenAI",           SMOKE_NATIVE,     "openai",     "gpt-4.1",                     true  },
        { "OpenRouter",       SMOKE_NATIVE,     "openrouter", "x-ai/grok-4.20-beta",         false },
        { "Google",           SMOKE_NATIVE,     "google",     "gemini-2.5-pro",              true  },
        { "Groq",             SMOKE_NATIVE,     "groq",       "llama-3.3-70b-versatile",     false },
        { "DeepSeek",         SMOKE_NATIVE,     "deepseek",   "deepseek-chat",               true  },
        { "xAI",              SMOKE_NATIVE,     "xai",        "grok-4-fast",                 false },
        { "Together",         SMOKE_NATIVE,     "together",   "meta-llama/Llama-4-Maverick-17B-128E-Instruct-FP8", true },
        { "Cerebras",         SMOKE_NATIVE,     "cerebras",   "qwen-3-235b-a22b-instruct-2507", false },
        { "Moonshot HS",      SMOKE_NATIVE,     "moonshot",   "kimi-k2.7-code-highspeed",     false },
        { "Moonshot",         SMOKE_NATIVE,     "moonshot",   "kimi-k2.7-code",              false },
        { "Mistral",          SMOKE_NATIVE,     "mistral",    "mistral-large-latest",        true  },
        { "Cohere",           SMOKE_NATIVE,     "cohere",     "command-a-03-2025",           true  },
        { "Perplexity",       SMOKE_NATIVE,     "perplexity", "sonar-pro",                   true  },
        { "OR Anthropic",     SMOKE_OPENROUTER, NULL,         "anthropic/claude-sonnet-4.6", true  },
        { "OR Anthropic Opus",SMOKE_OPENROUTER, NULL,         "anthropic/claude-opus-4.6",   true  },
        { "OR OpenAI",        SMOKE_OPENROUTER, NULL,         "openai/gpt-5.4",              true  },
        { "OR OpenAI o4",     SMOKE_OPENROUTER, NULL,         "openai/o4-mini",              true  },
        { "OR OpenAI OSS",    SMOKE_OPENROUTER, NULL,         "openai/gpt-oss-120b",         true  },
        { "OR xAI",           SMOKE_OPENROUTER, NULL,         "x-ai/grok-4.20-beta",         true  },
        { "OR Google",        SMOKE_OPENROUTER, NULL,         "google/gemini-2.5-pro",       true  },
        { "OR Google Flash",  SMOKE_OPENROUTER, NULL,         "google/gemini-2.5-flash",     true  },
        { "OR Google 3 Pro",  SMOKE_OPENROUTER, NULL,         "google/gemini-3.1-pro-preview", true },
        { "OR DeepSeek",      SMOKE_OPENROUTER, NULL,         "deepseek/deepseek-chat",      true  },
        { "OR Moonshot",      SMOKE_OPENROUTER, NULL,         "moonshotai/kimi-k2.7-code",   true  },
        { "OR Moonshot HS",   SMOKE_OPENROUTER, NULL,         "moonshotai/kimi-k2.7-code-highspeed", true },
        { "OR Moonshot Think",SMOKE_OPENROUTER, NULL,         "moonshotai/kimi-k2-thinking", true  },
        { "OR Qwen",          SMOKE_OPENROUTER, NULL,         "qwen/qwen3.5-plus-02-15",     true  },
        { "OR Qwen Coder",    SMOKE_OPENROUTER, NULL,         "qwen/qwen3-coder-next",       true  },
        { "OR Mistral",       SMOKE_OPENROUTER, NULL,         "mistralai/mistral-large-2512", true },
        { "OR Codestral",     SMOKE_OPENROUTER, NULL,         "mistralai/codestral-2508",    true  },
        { "OR Cohere",        SMOKE_OPENROUTER, NULL,         "cohere/command-a",            true  },
        { "OR GLM",           SMOKE_OPENROUTER, NULL,         "z-ai/glm-5.2",                true  },
        { "OR Llama",         SMOKE_OPENROUTER, NULL,         "meta-llama/llama-4-maverick", true  },
        { "OR MiniMax",       SMOKE_OPENROUTER, NULL,         "minimax/minimax-m2.5",        true  },
        { "OR Nova",          SMOKE_OPENROUTER, NULL,         "amazon/nova-premier-v1",      true  },
        { "OR Writer",        SMOKE_OPENROUTER, NULL,         "writer/palmyra-x5",           true  },
        { "OR NVIDIA",        SMOKE_OPENROUTER, NULL,         "nvidia/nemotron-3-super-120b-a12b:free", true },
        { "OR Hermes",        SMOKE_OPENROUTER, NULL,         "nousresearch/hermes-4-405b",  true  },
        { "OR StepFun",       SMOKE_OPENROUTER, NULL,         "stepfun/step-3.5-flash",      true  },
        { "OR Mercury",       SMOKE_OPENROUTER, NULL,         "inception/mercury-2",         true  },
        { "OR ERNIE",         SMOKE_OPENROUTER, NULL,         "baidu/ernie-4.5-21b-a3b",     true  },
        { "OR Arcee",         SMOKE_OPENROUTER, NULL,         "arcee-ai/trinity-large-preview:free", true },
        { "OR Xiaomi",        SMOKE_OPENROUTER, NULL,         "xiaomi/mimo-v2-flash",        true  },
        { "OR Aion",          SMOKE_OPENROUTER, NULL,         "aion-labs/aion-2.0",          true  },
        { "OR KwaiPilot",     SMOKE_OPENROUTER, NULL,         "kwaipilot/kat-coder-pro-v2",  true  },
        { "OR Seed",          SMOKE_OPENROUTER, NULL,         "bytedance-seed/seed-2.0-lite", true },
        { NULL, 0, NULL, NULL, false }
    };

    int passed = 0, failed = 0, skipped = 0;
    fprintf(stderr, "\n  \033[1mProvider Smoke%s\033[0m\n",
            full ? " (full)" : "");
    fprintf(stderr, "  %-14s %-10s %-32s %-7s %-6s %s\n",
            "TARGET", "PATH", "MODEL", "STATUS", "TIME", "DETAIL");
    fprintf(stderr, "  %-14s %-10s %-32s %-7s %-6s %s\n",
            "──────", "────", "─────", "──────", "────", "──────");

    for (int i = 0; smoke_cases[i].label; i++) {
        const smoke_case_t *sc = &smoke_cases[i];
        if (sc->full_only && !full) continue;

        smoke_result_t sr = smoke_run_case(self_path, sc, 45);
        const char *path_name = sc->kind == SMOKE_EXECUTOR ? sc->route :
                                sc->kind == SMOKE_NATIVE ? "native" : "openrouter";
        if (sr.skipped) {
            skipped++;
            fprintf(stderr, "  %-14s %-10s %-32.32s %s%-7s%s %-6s %s\n",
                    sc->label, path_name, sc->model,
                    "\033[2m", "skip", "\033[0m", "-",
                    sr.detail);
            continue;
        }

        if (sr.ok) passed++;
        else failed++;

        fprintf(stderr, "  %-14s %-10s %-32.32s %s%-7s%s %5.1fs %s\n",
                sc->label, path_name, sc->model,
                sr.ok ? "\033[32m" : "\033[31m",
                sr.ok ? "ok" : "FAIL",
                "\033[0m",
                sr.elapsed_sec,
                sr.detail);
    }

    fprintf(stderr, "\n  summary: passed=%d failed=%d skipped=%d\n\n",
            passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}

/* Build argv and exec. Never returns on success. */
static void exec_dispatch(const exec_reg_t *e, const char *prompt,
                          const char *model_override, char **extra, int nextra,
                          const char *fallback_api_key) {
    const char *av[64];
    int ac = 0;

    av[ac++] = e->bin;
    if (prompt && e->oneshot_cmd)
        av[ac++] = e->oneshot_cmd;
    else if (!prompt && e->oneshot_cmd)
        { /* interactive codex — no subcmd */ }

    if (prompt && e->print_flag)
        av[ac++] = e->print_flag;

    if (model_override && e->model_flag) {
        av[ac++] = e->model_flag;
        av[ac++] = model_override;
    }

    /* passthrough flags (from -- separator) */
    for (int i = 0; i < nextra && ac < 60; i++)
        av[ac++] = extra[i];

    if (prompt)
        av[ac++] = prompt;

    av[ac] = NULL;
    exec_prepare_env(e, fallback_api_key);
    execvp(e->bin, (char *const *)av);
    /* only reached on failure */
    perror(e->bin);
}

/* Global provider override — set by -e <native_provider> */
static const char *g_provider_override = NULL;

/* ── Account info helpers ──────────────────────────────────────────── */

/* Extract a JSON string value from raw JSON text without a full parser.
 * Finds "key":"value" and copies value into buf.  Returns true on success. */
static bool acct_json_str(const char *json, const char *key,
                           char *buf, size_t buf_len) {
    if (!json || !key || !buf || buf_len == 0) return false;
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') p++;
    size_t n = 0;
    while (*p && *p != '"' && *p != ',' && *p != '}' && *p != '\n' && n < buf_len - 1)
        buf[n++] = *p++;
    buf[n] = '\0';
    return n > 0;
}

static void show_claude_account_info(void) {
    char sub[128] = "", tier[128] = "";
    if (provider_claude_code_get_account_info(sub, sizeof(sub), tier, sizeof(tier))) {
        if (sub[0])  fprintf(stderr, "  \033[2mplan: %s\033[0m\n", sub);
        if (tier[0]) fprintf(stderr, "  \033[2mtier: %s\033[0m\n", tier);
    }
}

static void show_codex_account_info(void) {
    const char *home = getenv("HOME");
    if (!home) return;
    char auth_path[512];
    snprintf(auth_path, sizeof(auth_path), "%s/.codex/auth.json", home);
    FILE *f = fopen(auth_path, "r");
    if (!f) return;
    char *buf = malloc(32768);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, 32767, f);
    fclose(f);
    if (!n) { free(buf); return; }
    buf[n] = '\0';

    char mode[64] = "";
    acct_json_str(buf, "auth_mode", mode, sizeof(mode));
    if (mode[0]) {
        /* humanise: "chatgpt" → "ChatGPT subscription", "api_key" → "API key" */
        const char *label = strcmp(mode, "chatgpt") == 0 ? "ChatGPT subscription"
                          : strcmp(mode, "api_key") == 0 ? "API key"
                          : mode;
        fprintf(stderr, "  \033[2mplan: %s\033[0m\n", label);
    }

    char refresh[64] = "";
    acct_json_str(buf, "last_refresh", refresh, sizeof(refresh));
    if (refresh[0]) {
        /* trim to date portion: 2026-05-13T23:34:54.073420Z → 2026-05-13 */
        char *t = strchr(refresh, 'T');
        if (t) *t = '\0';
        fprintf(stderr, "  \033[2mlast refresh: %s\033[0m\n", refresh);
    }

    free(buf);
}

static int run_status_flow(void) {
    const char *active_exec  = getenv("DSCO_EXEC");
    const char *active_model = getenv("DSCO_MODEL");
    bool claude_auth = main_claude_exec_ready();
    bool codex_auth  = main_codex_exec_ready();

    fprintf(stderr, "\n  \033[1mdsco status\033[0m\n\n");

    if (active_exec && active_exec[0]) {
        fprintf(stderr, "  active backend : \033[1m%s\033[0m", active_exec);
        if (active_model && active_model[0])
            fprintf(stderr, "  (%s)", active_model);
        fprintf(stderr, "\n\n");
    } else {
        fprintf(stderr, "  active backend : not set  —  run \033[1mdsco login\033[0m\n\n");
    }

    fprintf(stderr, "  \033[1mClaude Code (Anthropic)\033[0m\n");
    if (claude_auth) {
        const char *src = provider_claude_code_oauth_source();
        fprintf(stderr, "  \033[32m● authenticated\033[0m via %s\n", src);
        show_claude_account_info();
    } else {
        fprintf(stderr, "  \033[2m○ not authenticated\033[0m\n");
    }

    fprintf(stderr, "\n  \033[1mChatGPT Codex (OpenAI)\033[0m\n");
    if (codex_auth) {
        fprintf(stderr, "  \033[32m● authenticated\033[0m via ~/.codex/auth.json\n");
        show_codex_account_info();
    } else {
        fprintf(stderr, "  \033[2m○ not authenticated\033[0m\n");
    }

    fprintf(stderr, "\n  env file: %s\n\n", dsco_setup_env_path());
    return 0;
}

/* ── Login flow ────────────────────────────────────────────────────── */

static void login_read_key_noecho(char *buf, size_t buf_len) {
    struct termios old, raw;
    bool term_ok = tcgetattr(STDIN_FILENO, &old) == 0;
    if (term_ok) {
        raw = old;
        raw.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    if (fgets(buf, (int)buf_len, stdin)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    } else {
        buf[0] = '\0';
    }
    if (term_ok) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        fprintf(stderr, "\n");
    }
}

static char *login_trim(char *s) {
    if (!s) return s;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ')) s[--n] = '\0';
    return s;
}

static int run_login_flow(void) {
    bool claude_bin  = exec_bin_available("claude");
    bool codex_bin   = exec_bin_available("codex");
    bool claude_auth = main_claude_exec_ready();
    bool codex_auth  = main_codex_exec_ready();

    fprintf(stderr, "\n  \033[1mdsco login\033[0m — connect your AI backend\n\n");
    fprintf(stderr, "  %-3s %-33s %-22s %s\n", " ", "BACKEND", "MODEL", "STATUS");
    fprintf(stderr, "  %-3s %-33s %-22s %s\n", " ", "───────", "─────", "──────");

    const char *claude_status_col = claude_auth ? "\033[32m" : (claude_bin ? "\033[33m" : "\033[2m");
    const char *claude_status     = claude_auth ? "● ready"  : (claude_bin ? "○ not authenticated" : "○ claude not installed");
    const char *codex_status_col  = codex_auth  ? "\033[32m" : (codex_bin  ? "\033[33m" : "\033[2m");
    const char *codex_status      = codex_auth  ? "● ready"  : (codex_bin  ? "○ not authenticated" : "○ codex not installed");

    fprintf(stderr, "  \033[1m[1]\033[0m %-33s %-22s %s%s\033[0m\n",
            "Claude Code  (Anthropic)", "claude-sonnet-4-6",
            claude_status_col, claude_status);
    fprintf(stderr, "  \033[1m[2]\033[0m %-33s %-22s %s%s\033[0m\n",
            "ChatGPT Codex  (OpenAI)", "auto (gpt-5.5)",
            codex_status_col, codex_status);

    fprintf(stderr, "\n  \033[2m[q] quit\033[0m\n\n> ");
    fflush(stderr);

    char line[128];
    if (!fgets(line, sizeof(line), stdin)) { fprintf(stderr, "\n"); return 0; }
    char top = line[0];

    if (top == 'q' || top == 'Q' || top == '\n' || top == '\r') {
        fprintf(stderr, "\n");
        return 0;
    }

    int pidx = -1;
    if (top == '1') pidx = 0;
    else if (top == '2') pidx = 1;
    else { fprintf(stderr, "\n  Unknown choice.\n\n"); return 1; }

    const char *pname   = pidx == 0 ? "claude"                 : "codex";
    const char *plabel  = pidx == 0 ? "Claude Code (Anthropic)": "ChatGPT Codex (OpenAI)";
    const char *pmodel  = "";
    const char *penvkey = pidx == 0 ? "ANTHROPIC_API_KEY"      : "OPENAI_API_KEY";
    bool pbin   = pidx == 0 ? claude_bin  : codex_bin;
    bool pready = pidx == 0 ? claude_auth : codex_auth;

    fprintf(stderr, "\n  \033[1m%s\033[0m\n\n", plabel);

    if (pready) {
        if (pidx == 0) {
            const char *src = provider_claude_code_oauth_source();
            fprintf(stderr, "  \033[32m● Already authenticated\033[0m");
            if (src && src[0] && strcmp(src, "missing") != 0 && strcmp(src, "disabled") != 0)
                fprintf(stderr, " (via %s)", src);
            fprintf(stderr, "\n");
            show_claude_account_info();
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  \033[32m● Already authenticated\033[0m (via ~/.codex/auth.json)\n");
            show_codex_account_info();
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "  Set as active backend? [Y/n] ");
        fflush(stderr);
        if (!fgets(line, sizeof(line), stdin)) { fprintf(stderr, "\n"); return 0; }
        if (line[0] == 'n' || line[0] == 'N') {
            fprintf(stderr, "\n  No changes made.\n\n");
            return 0;
        }
        goto save_active;
    }

    fprintf(stderr, "  \033[33m○ Not authenticated\033[0m\n\n");

    if (pbin) {
        const char *lcmd = pidx == 0 ? "claude auth login" : "codex login";
        fprintf(stderr, "  \033[1m[a]\033[0m Run `%s`  (recommended)\n", lcmd);
    }
    fprintf(stderr, "  \033[1m[k]\033[0m Enter %s manually\n", penvkey);
    fprintf(stderr, "  \033[2m[c]\033[0m Cancel\n\n> ");
    fflush(stderr);

    if (!fgets(line, sizeof(line), stdin)) { fprintf(stderr, "\n"); return 0; }
    char achoice = line[0];

    if (achoice == 'c' || achoice == 'C') {
        fprintf(stderr, "\n  Cancelled.\n\n");
        return 0;
    }

    if (pbin && (achoice == 'a' || achoice == 'A')) {
        const char *lcmd = pidx == 0 ? "claude auth login" : "codex login";
        fprintf(stderr, "\n  Running: %s\n\n", lcmd);
        fflush(stderr);
        int rc = system(lcmd);
        bool now_ready = pidx == 0 ? main_claude_exec_ready() : main_codex_exec_ready();
        if (rc != 0 || !now_ready) {
            fprintf(stderr, "\n  \033[33mWarning: login may not have completed.\033[0m\n");
            if (!now_ready && !pbin) {
                fprintf(stderr, "  Try entering the API key manually instead.\n\n");
                return 1;
            }
            if (!now_ready) {
                /* Offer key entry as fallback */
                fprintf(stderr, "\n  Enter %s (or press Enter to cancel): ", penvkey);
                fflush(stderr);
                char key_buf[512] = {0};
                login_read_key_noecho(key_buf, sizeof(key_buf));
                char *trimmed = login_trim(key_buf);
                if (!trimmed[0]) { fprintf(stderr, "  No key entered.\n\n"); return 1; }
                setenv(penvkey, trimmed, 1);
                dsco_setup_set_key(penvkey, trimmed);
                fprintf(stderr, "  \033[32m● Key saved.\033[0m\n\n");
            }
        } else {
            fprintf(stderr, "\n  \033[32m● Authentication successful!\033[0m\n\n");
        }
    } else {
        /* Manual key entry (also handles 'k' and unknown choice fallback) */
        fprintf(stderr, "\n  Enter %s: ", penvkey);
        fflush(stderr);
        char key_buf[512] = {0};
        login_read_key_noecho(key_buf, sizeof(key_buf));
        char *trimmed = login_trim(key_buf);
        if (!trimmed[0]) { fprintf(stderr, "  No key entered. Exiting.\n\n"); return 1; }
        setenv(penvkey, trimmed, 1);
        dsco_setup_set_key(penvkey, trimmed);
        fprintf(stderr, "  \033[32m● Key saved.\033[0m\n\n");
    }

save_active:
    setenv("DSCO_EXEC",  pname,  1);
    setenv("DSCO_MODEL", pmodel, 1);
    dsco_setup_set_key("DSCO_EXEC",  pname);
    dsco_setup_set_key("DSCO_MODEL", pmodel);

    const char *pdisplay = (pmodel && pmodel[0]) ? pmodel : "auto";
    fprintf(stderr,
            "  \033[32m✓\033[0m Active backend: \033[1m%s\033[0m (%s)\n"
            "  Saved to %s\n\n"
            "  Run \033[1mdsco -i\033[0m to start chatting.\n\n",
            plabel, pdisplay, dsco_setup_env_path());
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "dsco v%s — thin agentic CLI (streaming + prompt caching)\n"
        "\n"
        "Usage: %s [options] [prompt]\n"
        "       %s login          Choose Claude Code or ChatGPT Codex backend\n"
        "       %s status         Show auth state and account info for all backends\n"
        "\n"
        "Options:\n"
        "  -m MODEL    Model name (default: %s)\n"
        "  -k KEY      API key (default: provider env for selected model)\n"
        "  --profile full|lite|worker  Runtime startup profile (default: full)\n"
        "  --login                Interactive backend login (Claude Code / Codex)\n"
        "  --setup                Save detected API keys/tokens into dsco env file\n"
        "  --setup-force          Overwrite existing saved values from current env\n"
        "  --setup-report         Show masked setup/config status\n"
        "  --workspace-bootstrap  Create claw workspace files under ~/.dsco/workspace\n"
        "  --workspace-status     Show claw workspace status\n"
        "  --timeline-server        Run local timeline web server\n"
        "  --timeline-port PORT     Timeline webserver port (default: 8421)\n"
        "  --timeline-instance ID   Filter timeline to one instance ID\n"
        "  -O, --orchestrate      Orchestrator mode: Haiku routes to specialist workers\n"
        "  -M, --worker-model M   Worker model for orchestrate mode (default: kimi-k2.7-code-highspeed)\n"
        "  --topology NAME        Run/select an agent topology\n"
        "  --topology-auto        Auto-pick a topology for the task\n"
        "  --topology-list        List available topologies\n"
        "  --ui [PORT]            Launch web UI (default port: 3141)\n"
        "  -i, --interactive      Start an interactive REPL (no prompt required)\n"
        "  -e, --exec BACKEND    Execute via CLI/provider (claude, codex, auto, smart, list, bench, bench-tools, smoke, smoke-full, <provider>)\n"
        "  --provider NAME       Force a native provider (anthropic, openai, openrouter, xai, ...)\n"
        "  --                    Pass remaining args to executor (after -e)\n"
        "  -C, --cheap            Cheap mode: 5 core tools + discover/load (env: DSCO_CHEAP=1)\n"
        "  --version              Print version and build info\n"
        "  -h          Show this help\n"
        "\n"
        "Prompt mode:      %s 'write a hello world in C and compile it'\n"
        "Interactive:      %s -i     (or just %s when run in a terminal)\n"
        "\n"
        "Environment:\n"
        "  ANTHROPIC_API_KEY   Anthropic API key (Claude Code OAuth is auto-detected when available)\n"
        "  OPENROUTER_API_KEY  OpenRouter API key for namespaced org/model IDs\n"
        "  DSCO_MODEL          Default model override\n"
        "  DSCO_PROFILE        Runtime startup profile when full/lite/worker\n"
        "  DSCO_ENV_FILE       Override setup env file path\n"
        "  DSCO_BASELINE_DB    Override sqlite baseline path\n"
        "  DSCO_EXEC           Default executor (claude, codex, auto)\n"
        "  DSCO_BUDGET         Session cost budget in dollars (0=unlimited)\n"
        "  DSCO_DAILY_BUDGET   Daily cost budget in dollars (0=unlimited)\n",
    DSCO_VERSION, prog, prog, prog, DEFAULT_MODEL, prog, prog, prog);
}

static void print_topology_list(void) {
    int count = 0;
    const topology_t *tops = topology_registry(&count);
    printf("Available topologies (%d):\n", count);
    for (int i = 0; i < count; i++) {
        printf("  T%02d %-18s %-12s agents=%d latency=%.1fx\n",
               tops[i].id, tops[i].name, topo_category_label(tops[i].category),
               tops[i].total_agents, tops[i].est_latency_mult);
    }
}

static void print_topology_show(const char *name) {
    topology_registry_init();
    if (!name || !name[0]) {
        /* Show all topologies with tree diagrams */
        int count = 0;
        const topology_t *tops = topology_registry(&count);
        for (int i = 0; i < count; i++) {
            char buf[4096];
            topology_render_ascii(&tops[i], buf, sizeof(buf));
            printf("%s\n", buf);
        }
        return;
    }
    const topology_t *t = topology_find(name);
    if (!t) {
        fprintf(stderr, "error: topology '%s' not found\n", name);
        return;
    }
    char buf[4096];
    topology_render_ascii(t, buf, sizeof(buf));
    printf("%s\n%s\n", t->description, buf);
}

static int run_oneshot_topology(const char *api_key, const char *model,
                                const char *topology_name, bool topology_auto,
                                const char *prompt) {
    topology_plan_t plan;
    if (!topology_plan_build(topology_name, topology_auto, prompt, &plan)) {
        fprintf(stderr, "error: topology not found\n");
        return 1;
    }

    char *result = safe_malloc(MAX_RESPONSE_SIZE);
    topology_run_stats_t stats;
    bool ok = topology_plan_run(&plan, api_key, model, prompt, result, MAX_RESPONSE_SIZE, &stats);
    md_reset(&s_oneshot_md);
    md_feed_str(&s_oneshot_md, result);
    md_flush(&s_oneshot_md);
    printf("\n");
    fprintf(stderr, "  %stopology:%s %s  %sagents:%s %d  %siterations:%s %d  %sest:$%.4f%s\n",
            TUI_DIM, TUI_RESET, plan.topology.name,
            TUI_DIM, TUI_RESET, stats.agents_spawned,
            TUI_DIM, TUI_RESET, stats.iterations,
            TUI_BCYAN, stats.est_cost_usd, TUI_RESET);
    if (plan.rationale[0]) {
        fprintf(stderr, "  %splan:%s %s\n", TUI_DIM, TUI_RESET, plan.rationale);
    }
    baseline_log("swarm", ok ? "topology_run" : "topology_run_failed", plan.topology.name, NULL);
    free(result);
    return ok ? 0 : 1;
}

/* One-shot mode: simple print callbacks */
static void oneshot_text_cb(const char *text, void *ctx) {
    (void)ctx;
    TRACE_DEBUG("text_cb len=%zu first16=%.16s", strlen(text), text);
    md_feed_str(&s_oneshot_md, text);
    fflush(stdout);
}

static void oneshot_tool_cb(const char *name, const char *id, void *ctx) {
    (void)id; (void)ctx;
    /* Powerline-style tool announce for oneshot mode */
    {
        tui_tool_type_t tt = tui_classify_tool(name);
        tui_rgb_t rgb = tui_tool_rgb(tt);
        const tui_glyphs_t *gl = tui_glyph();
        bool use_pl = tui_detect_color_level() >= TUI_COLOR_256;
        if (use_pl && gl->pl_right[0]) {
            int r = (int)(rgb.r * 0.55), g2 = (int)(rgb.g * 0.55), b = (int)(rgb.b * 0.55);
            fprintf(stderr, "\033[48;2;%d;%d;%dm\033[38;2;220;220;220m %s %s \033[0m"
                            "\033[38;2;%d;%d;%dm%s\033[0m\n",
                    r, g2, b, gl->icon_lightning, name, r, g2, b, gl->pl_right);
        } else {
            fprintf(stderr, "\033[2m\033[36m► %s\033[0m\n", name);
        }
    }
    fflush(stderr);
    baseline_log("tool", name, "tool_use started", NULL);
}

static void json_print_escaped(FILE *out, const char *s) {
    if (!out) out = stdout;
    if (!s) return;
    for (const char *p = s; *p; p++) {
        if (*p == '"')       fputs("\\\"", out);
        else if (*p == '\\') fputs("\\\\", out);
        else if (*p == '\n') fputs("\\n", out);
        else if (*p == '\r') fputs("\\r", out);
        else if (*p == '\t') fputs("\\t", out);
        else if ((unsigned char)*p < 0x20) fprintf(out, "\\u%04x", (unsigned char)*p);
        else                 fputc(*p, out);
    }
}

static void print_models_json(void) {
    printf("[");
    for (int j = 0; MODEL_REGISTRY[j].alias; j++) {
        const model_info_t *m = &MODEL_REGISTRY[j];
        if (j > 0) printf(",");
        printf("{\"alias\":\"%s\",\"model_id\":\"%s\","
               "\"context_window\":%d,\"max_output\":%d,"
               "\"input_price\":%.2f,\"output_price\":%.2f,"
               "\"cache_read_price\":%.2f,\"cache_write_price\":%.2f,"
               "\"supports_thinking\":%d}",
               m->alias, m->model_id, m->context_window, m->max_output,
               m->input_price, m->output_price,
               m->cache_read_price, m->cache_write_price,
               m->supports_thinking);
    }
    printf("]\n");
}

/* ── OpenRouter catalog listing (indexed via openrouter_cache) ─────────── */

static void or_model_json_cb(const or_model_view_t *m, void *ud) {
    int *n = ud;
    if ((*n)++ > 0) printf(",");
    printf("{\"id\":\"%s\",\"name\":\"", m->id);
    json_print_escaped(stdout, m->name ? m->name : "");
    printf("\",\"org\":\"%s\","
           "\"context_window\":%d,\"max_output\":%d,"
           "\"input_price\":%.4f,\"output_price\":%.4f,"
           "\"cache_read_price\":%.4f,\"cache_write_price\":%.4f,"
           "\"supports_thinking\":%d,\"multimodal\":%d,\"created\":%ld}",
           m->org ? m->org : "", m->context_window, m->max_output,
           m->input_price, m->output_price,
           m->cache_read_price, m->cache_write_price,
           m->supports_thinking, m->multimodal, m->created);
}

static void or_model_text_cb(const or_model_view_t *m, void *ud) {
    (void)ud;
    printf("%-44s %9d  $%7.3f/$%-7.3f  %s%s  %s\n",
           m->id, m->context_window, m->input_price, m->output_price,
           m->supports_thinking ? "R" : "-",
           m->multimodal ? "M" : "-",
           m->name ? m->name : "");
}

static int print_or_models(bool json) {
    int n = openrouter_cache_load_sync();
    if (n <= 0) {
        if (json) printf("[]\n");
        else fprintf(stderr, "openrouter: catalog unavailable (offline and no disk cache)\n");
        return n > 0 ? 0 : 1;
    }
    if (json) {
        int count = 0;
        printf("[");
        openrouter_cache_foreach(or_model_json_cb, &count);
        printf("]\n");
    } else {
        fprintf(stderr, "# %d OpenRouter models indexed (R=reasoning, M=multimodal)\n", n);
        openrouter_cache_foreach(or_model_text_cb, NULL);
    }
    return 0;
}

static void print_tools_json_fast(dsco_profile_t profile) {
    if (profile == DSCO_PROFILE_LITE || profile == DSCO_PROFILE_WORKER)
        tools_init_profile(TOOLS_CORE);
    else
        tools_init_local_only();
    int count = 0;
    const tool_def_t *tools = tools_get_all(&count);
    printf("[");
    int emitted = 0;
    for (int j = 0; j < count; j++) {
        const tool_def_t *t = &tools[j];
        if (!t->name) continue;
        if (!tools_profile_allows_index(j)) continue;
        if (emitted++ > 0) printf(",");
        printf("{\"name\":\"%s\",\"description\":\"", t->name);
        json_print_escaped(stdout, t->description ? t->description : "");
        printf("\",\"input_schema\":%s}",
               (t->input_schema_json && t->input_schema_json[0])
                   ? t->input_schema_json
                   : "{\"type\":\"object\",\"properties\":{}}");
    }
    printf("]\n");
}

static bool main_json_string_needs_escape(const char *s) {
    if (!s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == '"' || *p == '\\') return true;
    }
    return false;
}

static void main_print_tool_exec_json(bool ok, const char *result) {
    const char *prefix = ok ? "{\"ok\":true,\"result\":\""
                            : "{\"ok\":false,\"result\":\"";
    if (!main_json_string_needs_escape(result)) {
        (void)write(STDOUT_FILENO, prefix, strlen(prefix));
        if (result) (void)write(STDOUT_FILENO, result, strlen(result));
        (void)write(STDOUT_FILENO, "\"}\n", 3);
        return;
    }

    fputs(prefix, stdout);
    json_print_escaped(stdout, result ? result : "");
    fputs("\"}\n", stdout);
}

static int run_tool_exec_fast(dsco_profile_t profile,
                              const char *name,
                              const char *input_json) {
    if ((profile == DSCO_PROFILE_LITE || profile == DSCO_PROFILE_WORKER) &&
        name && strcmp(name, "cwd") == 0 &&
        (!input_json || !strstr(input_json, "path"))) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) {
            main_print_tool_exec_json(false, strerror(errno));
            return 1;
        }
        main_print_tool_exec_json(true, cwd);
        return 0;
    }

    if (profile == DSCO_PROFILE_LITE || profile == DSCO_PROFILE_WORKER)
        tools_init_profile(TOOLS_CORE);
    else
        tools_init_local_only();
    char result[256 * 1024] = {0};
    bool ok = tools_execute(name, input_json, result, sizeof(result));
    printf("{\"ok\":%s,\"result\":\"", ok ? "true" : "false");
    json_print_escaped(stdout, result);
    printf("\"}\n");
    return ok ? 0 : 1;
}

static void main_tools_init_for_runtime(dsco_profile_t profile) {
    if (profile == DSCO_PROFILE_LITE || profile == DSCO_PROFILE_WORKER)
        tools_init_profile(TOOLS_CORE);
    else
        tools_init();

    /* Self-improvement meta-loop: observe tool/turn performance and carry
     * learnings across sessions. init() must precede any record call; the
     * record hooks in agent.c early-return until this runs. */
    self_improve_init(&g_self_improve);
    self_improve_load_history(&g_self_improve);
}

/* Return -1 when not handled, otherwise the process exit code. */
/* ── Live multi-project pet dashboard ──────────────────────────────────────
 * Polls the shared IPC agent registry (which every dsco process/project writes
 * to) and renders one companion pet per agent across all projects, refreshing
 * ~2×/s in an alternate screen. This is the "manage hundreds of projects /
 * thousands of agents" view: the roster caps to the terminal height and reports
 * the overflow, sorted working-first. Read-only — never registers itself. */
static volatile sig_atomic_t g_pets_watch_stop = 0;
static void pets_watch_sigint(int sig) { (void)sig; g_pets_watch_stop = 1; }

static uint32_t pets_id_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h & 0x7fffffffu;
}

static const char *pets_resolve_ipc_db(int argc, char **argv) {
    if (argc >= 4 && argv[3][0]) return argv[3];          /* explicit path */
    const char *env = getenv("DSCO_IPC_DB");
    if (env && env[0]) return env;
    /* else the most-recently-touched session DB across both default locations */
    static char newest[1024];
    newest[0] = '\0';
    char home_pat[1024];
    const char *home = getenv("HOME");
    snprintf(home_pat, sizeof(home_pat), "%s/.dsco/ipc/dsco_ipc_*.db",
             home ? home : "/tmp");
    const char *patterns[] = { home_pat, "/tmp/dsco_ipc_*.db" };
    time_t best = 0;
    for (int p = 0; p < 2; p++) {
        glob_t g;
        if (glob(patterns[p], 0, NULL, &g) != 0) continue;
        for (size_t i = 0; i < g.gl_pathc; i++) {
            struct stat st;
            if (stat(g.gl_pathv[i], &st) == 0 && st.st_mtime >= best) {
                best = st.st_mtime;
                snprintf(newest, sizeof(newest), "%s", g.gl_pathv[i]);
            }
        }
        globfree(&g);
    }
    return newest[0] ? newest : NULL;
}

static pet_status_t pets_map_ipc(ipc_agent_status_t s) {
    switch (s) {
        case IPC_AGENT_STARTING: return PET_ST_PENDING;
        case IPC_AGENT_WORKING:  return PET_ST_WORKING;
        case IPC_AGENT_DONE:     return PET_ST_DONE;
        case IPC_AGENT_ERROR:    return PET_ST_ERROR;
        case IPC_AGENT_DEAD:     return PET_ST_ERROR;
        case IPC_AGENT_IDLE:     return PET_ST_IDLE;
        default:                 return PET_ST_IDLE;
    }
}

static int run_pets_watch(int argc, char **argv) {
    const char *db = pets_resolve_ipc_db(argc, argv);
    ipc_init(db, NULL);   /* open the shared DB; never ipc_register → invisible */

    struct termios orig, raw;
    bool tty = isatty(STDIN_FILENO);
    if (tty && tcgetattr(STDIN_FILENO, &orig) == 0) {
        raw = orig;
        raw.c_lflag &= ~((tcflag_t)(ICANON | ECHO));  /* keep ISIG so Ctrl-C works */
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = pets_watch_sigint;
    sigaction(SIGINT, &sa, &old);
    fputs("\033[?1049h\033[?25l", stdout);   /* enter alt screen, hide cursor */

    pet_roster_t roster;
    pet_roster_init(&roster);

    while (!g_pets_watch_stop) {
        static ipc_agent_info_t agents[1024];
        int n = ipc_list_agents(agents, 1024);
        for (int i = 0; i < n; i++) {
            int key = (int)pets_id_hash(agents[i].id);
            const char *lbl = agents[i].current_task[0] ? agents[i].current_task
                            : (agents[i].role[0] ? agents[i].role : agents[i].id);
            pet_status_t st = pets_map_ipc(agents[i].status);
            pet_roster_upsert(&roster, key, agents[i].depth, lbl, agents[i].id, st);
            pet_roster_set_status(&roster, key, st, 0);
        }

        int h = tui_term_height();
        if (h < 8) h = 24;
        fputs("\033[2J\033[H", stdout);
        printf("  \033[1m\033[96mdsco · live agent pets\033[0m   \033[2m%s\033[0m\n\n",
               db ? db : "(no ipc db — set DSCO_IPC_DB or pass a path)");
        pet_roster_render(stdout, &roster, tui_term_width(), h - 6);
        printf("\n  \033[2mrefresh 0.5s · q / Ctrl-C to quit · %d agents in registry\033[0m\n", n);
        fflush(stdout);

        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, 500) > 0 && (pfd.revents & POLLIN)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1 && (c == 'q' || c == 27)) break;
        }
    }

    fputs("\033[?25h\033[?1049l", stdout);   /* show cursor, leave alt screen */
    fflush(stdout);
    if (tty) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    sigaction(SIGINT, &old, NULL);
    pet_roster_free(&roster);
    return 0;
}

static int maybe_run_early_fast_path(int argc, char **argv,
                                     dsco_profile_t profile) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--models-json") == 0) {
            perf_mark("fast models-json");
            print_models_json();
            perf_finish("fast exit");
            return 0;
        }
        if (strcmp(argv[i], "--tools-json") == 0) {
            perf_mark("fast tools-json begin");
            print_tools_json_fast(profile);
            perf_finish("fast exit");
            return 0;
        }
        if (strcmp(argv[i], "--codebase-stats") == 0) {
            extern int introspect_print_codebase_stats(FILE *);
            introspect_print_codebase_stats(stdout);
            perf_finish("fast exit");
            return 0;
        }
        if (strcmp(argv[i], "--selves") == 0) {
            extern int introspect_run_selves(FILE *, int, const char *);
            int nselves = 4;
            const char *prompt = NULL;
            if (i + 1 < argc) {
                char *end = NULL;
                long v = strtol(argv[i+1], &end, 10);
                if (end && *end == 0 && v > 0 && i + 2 < argc) {
                    nselves = (int)v;
                    prompt = argv[i+2];
                } else {
                    prompt = argv[i+1];
                }
            }
            if (!prompt) {
                fprintf(stderr, "usage: dsco --selves [N] <prompt>\n");
                perf_finish("fast error");
                return 1;
            }
            int rc = introspect_run_selves(stdout, nselves, prompt);
            perf_finish("fast exit");
            return rc;
        }
        if (strcmp(argv[i], "--tool-exec") == 0) {
            if (i + 2 >= argc) {
                fprintf(stderr, "error: --tool-exec requires <name> <json>\n");
                perf_finish("fast error");
                return 1;
            }
            perf_mark("fast tool-exec begin");
            int rc = run_tool_exec_fast(profile, argv[i + 1], argv[i + 2]);
            perf_finish("fast exit");
            return rc;
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    perf_init();
    heartbeat_set_context(argc, argv);
    heartbeat_set_phase("main_entry");

    /* Fault-injection hook for exercising the crash handler + supervisor end
     * to end. Completely inert unless DSCO_TEST_CRASH is set. Installs the
     * crash handlers itself so the in-process backtrace path is exercised.
     * Optional "@N" suffix crashes only on the Nth supervised generation so
     * the supervisor's restart/backoff/recovery can be observed live. */
    {
        const char *tc = getenv("DSCO_TEST_CRASH");
        bool is_supervise_launcher = argc >= 2 &&
            (strcmp(argv[1], "supervise") == 0 || strcmp(argv[1], "--supervise") == 0);
        if (tc && tc[0] && !is_supervise_launcher) {
            const char *at = strchr(tc, '@');
            bool fire = true;
            if (at) {
                const char *gen = getenv("DSCO_SUPERVISED");
                fire = gen && atoi(gen) == atoi(at + 1);
            }
            if (fire) {
                heartbeat_set_phase("fault_injection");
                main_install_crash_handlers();
                fprintf(stderr, "[dsco] DSCO_TEST_CRASH=%s — injecting fault "
                        "(gen %s)\n", tc, getenv("DSCO_SUPERVISED") ? getenv("DSCO_SUPERVISED") : "0");
                if (strncmp(tc, "abort", 5) == 0) abort();
                if (strncmp(tc, "exit", 4) == 0)  return 42;
                if (strncmp(tc, "kill", 4) == 0)  raise(SIGKILL);  /* mimic OOM/jetsam */
                volatile int *p = NULL;  /* default: SIGSEGV */
                *p = 1;
            }
        }
    }
    dsco_profile_t runtime_profile = main_runtime_profile(argc, argv);
    dsco_caps_t startup_caps = main_plan_startup_caps(argc, argv, runtime_profile);
    perf_set_startup_context(runtime_profile, startup_caps);

    /* `dsco tools …` drives the external Tool Management API. Dispatch first so
     * its own -h/subcommands aren't swallowed by the global flag loop below; it
     * manages its own config + auth and never touches the keychain. */
    if (argc >= 2 && strcmp(argv[1], "tools") == 0)
        return toolmgmt_cli(argc, argv);

    /* `dsco pets [gallery|roll|roster] [seed]` renders companion sprites
     * straight to the terminal (real newlines + ANSI), unlike `--tool-exec`
     * which wraps the art in escaped JSON. */
    if (argc >= 2 && strcmp(argv[1], "pets") == 0) {
        const char *action = argc >= 3 ? argv[2] : "gallery";
        if (strcmp(action, "watch") == 0) {
            return run_pets_watch(argc, argv);
        } else if (strcmp(action, "roll") == 0) {
            const char *seed = argc >= 4 ? argv[3] : "anon";
            pet_t p;
            memset(&p, 0, sizeof(p));
            pet_roll(seed, &p.bones);
            snprintf(p.name, sizeof(p.name), "%s", seed);
            p.status = PET_ST_IDLE;
            pet_card_print(stdout, &p, 0);
        } else if (strcmp(action, "roster") == 0) {
            pet_roster_render(stdout, pet_roster_global(), tui_term_width(), 0);
        } else {
            pet_gallery_print(stdout, 0);
        }
        return 0;
    }

    /* `dsco connect …` drives the future-proof baseline connector: a single
     * seam every external system plugs into (tools today; chains, credit,
     * robotics, neural/haptic next). Dispatched first for the same reasons. */
    if (argc >= 2 && strcmp(argv[1], "connect") == 0)
        return connector_cli(argc, argv);

    /* `dsco supervise [args...]` (or `dsco --supervise [args...]`) runs the
     * real dsco as a managed child: a higher-order watcher that catches every
     * crash/kill (incl. OOM SIGKILL), captures a backtrace via the debugger,
     * and restarts/rescues the session in realtime. The child gets
     * DSCO_NO_SUPERVISE=1 so it never recurses. */
    if (!getenv("DSCO_NO_SUPERVISE") &&
        argc >= 2 && (strcmp(argv[1], "supervise") == 0 ||
                      strcmp(argv[1], "--supervise") == 0)) {
        /* Hand the supervisor a clean argv whose [0] is the dsco binary and
         * whose tail is the post-"supervise" args (overwrite argv[1], which
         * held "supervise", with the binary path). child_argv stays NULL-
         * terminated because argv[argc] == NULL. */
        argv[1] = argv[0];
        return supervisor_run(argc - 1, argv + 1);
    }

    /* Trivial info flags must short-circuit BEFORE any keychain / secure-store
     * touch. Otherwise wrappers (web server, scripts) calling `dsco --version`
     * trigger a macOS keychain prompt for every invocation. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("dsco v%s (built %s, %s)\n", DSCO_VERSION, BUILD_DATE, GIT_HASH);
            perf_finish("version exit");
            return 0;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            perf_finish("help exit");
            return 0;
        }
    }

    int fast_rc = maybe_run_early_fast_path(argc, argv, runtime_profile);
    if (fast_rc >= 0) return fast_rc;
    dsco_startup_init(runtime_profile, startup_caps);

    /* Catch fatal signals on the primary path too (previously only sub-agents
     * installed these). On a real crash we dump a backtrace + crash log that
     * the supervisor reads to rescue the session. */
    main_install_crash_handlers();

    /* Bare `dsco` in a TTY drops into the interactive REPL.
     * Non-TTY (pipe/redirect) keeps the old behavior of printing usage + error,
     * so scripts that accidentally forget the prompt fail loudly instead of
     * hanging on stdin. Override with DSCO_NO_AUTO_INTERACTIVE=1. */
    if (argc == 1) {
        const char *no_auto = getenv("DSCO_NO_AUTO_INTERACTIVE");
        bool auto_ok = !(no_auto && (no_auto[0] == '1' || no_auto[0] == 't' || no_auto[0] == 'T'));
        if (auto_ok && isatty(STDIN_FILENO) && isatty(STDERR_FILENO)) {
            /* Synthesize argv as if user had passed -i so the rest of the
             * option parser / allows_no_prompt logic just works. */
            static char *synth_argv[2];
            static char iflag[] = "-i";
            synth_argv[0] = argv[0];
            synth_argv[1] = iflag;
            argv = synth_argv;
            argc = 2;
        } else {
            usage(argv[0]);
            fprintf(stderr, "\nerror: prompt required (try -i for interactive mode)\n");
            return 1;
        }
    }

    const char *cli_profile = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            cli_profile = argv[i + 1];
            break;
        }
    }
    dsco_profile_t ignored_runtime_profile;
    if (cli_profile && cli_profile[0] &&
        !dsco_profile_parse(cli_profile, &ignored_runtime_profile)) {
        setenv("DSCO_PROFILE", cli_profile, 1);
    }

    bool arg_requests_setup = false;
    bool arg_skip_bootstrap = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            arg_skip_bootstrap = true;
        }
        if (strcmp(argv[i], "--setup") == 0 ||
            strcmp(argv[i], "--setup-force") == 0 ||
            strcmp(argv[i], "--setup-report") == 0) {
            arg_requests_setup = true;
            break;
        }
    }

    /* Detect `login` / `status` subcommands early (before bootstrap) */
    bool arg_login  = false;
    bool arg_status = false;
    if (argc >= 2 && strcmp(argv[1], "login") == 0)  arg_login  = true;
    if (argc >= 2 && strcmp(argv[1], "status") == 0) arg_status = true;
    for (int i = 1; i < argc && !arg_login && !arg_status; i++) {
        if (strcmp(argv[i], "--login") == 0)  { arg_login  = true; break; }
        if (strcmp(argv[i], "--status") == 0) { arg_status = true; break; }
    }

    const char *pre_setup_model = getenv("DSCO_MODEL");
    bool model_preexisted_setup = pre_setup_model && pre_setup_model[0];

    int loaded_env_count = dsco_setup_load_saved_env();
    char bootstrap_msg[512];
    if (!arg_requests_setup && !arg_skip_bootstrap && !arg_login && !arg_status) {
        int bootstrap_state = dsco_setup_bootstrap_from_env(bootstrap_msg, sizeof(bootstrap_msg));
        if (bootstrap_state > 0) {
            fprintf(stderr, "%s\n", bootstrap_msg);
            loaded_env_count += dsco_setup_load_saved_env();
        }
    }

    if (arg_login)  return run_login_flow();
    if (arg_status) return run_status_flow();

    const char *api_key = NULL;
    const char *env_model = getenv("DSCO_MODEL");
    bool model_from_env = env_model && env_model[0];
    if (model_from_env && !model_preexisted_setup &&
        is_legacy_sonnet_default_model(env_model)) {
        unsetenv("DSCO_MODEL");
        env_model = NULL;
        model_from_env = false;
    }
    const char *model = model_from_env ? env_model : DEFAULT_MODEL;
    char *oneshot_prompt = NULL;
    bool timeline_server_mode = false;
    bool setup_mode = false;
    bool setup_force = false;
    bool setup_report_mode = false;
    bool workspace_bootstrap_mode = false;
    bool workspace_status_mode = false;
    int timeline_port = 8421;
    const char *timeline_instance_filter = NULL;
    const char *topology_name = NULL;
    bool topology_auto = false;
    bool topology_list_mode = false;
    const char *topology_show_name = NULL;
    bool topology_show_mode = false;
    const char *exec_backend = NULL;  /* "claude", "codex", "auto", "list" */
    bool exec_backend_from_env = false;
    char **exec_extra = NULL;         /* passthrough args after -- */
    int exec_nextra = 0;
    bool user_set_model = false;
    bool ui_mode = false;
    int ui_port = 3141;
    bool orchestrate_mode = false;
    bool interactive_mode = false;
    bool mux_mode = false;
    const char *mux_initial_root = NULL;
    const char *worker_model = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("dsco v%s (built %s, %s)\n", DSCO_VERSION, BUILD_DATE, GIT_HASH);
            free(oneshot_prompt);
            return 0;
        }
        if (strcmp(argv[i], "--models-json") == 0) {
            printf("[");
            for (int j = 0; MODEL_REGISTRY[j].alias; j++) {
                const model_info_t *m = &MODEL_REGISTRY[j];
                if (j > 0) printf(",");
                printf("{\"alias\":\"%s\",\"model_id\":\"%s\","
                       "\"context_window\":%d,\"max_output\":%d,"
                       "\"input_price\":%.2f,\"output_price\":%.2f,"
                       "\"cache_read_price\":%.2f,\"cache_write_price\":%.2f,"
                       "\"supports_thinking\":%d}",
                       m->alias, m->model_id, m->context_window, m->max_output,
                       m->input_price, m->output_price,
                       m->cache_read_price, m->cache_write_price,
                       m->supports_thinking);
            }
            printf("]\n");
            free(oneshot_prompt);
            return 0;
        }
        if (strcmp(argv[i], "--or-models") == 0) {
            int rc = print_or_models(false);
            free(oneshot_prompt);
            return rc;
        }
        if (strcmp(argv[i], "--or-models-json") == 0) {
            int rc = print_or_models(true);
            free(oneshot_prompt);
            return rc;
        }
        if (strcmp(argv[i], "--tools-json") == 0) {
            main_tools_init_for_runtime(runtime_profile);
            int count = 0;
            const tool_def_t *tools = tools_get_all(&count);
            printf("[");
            for (int j = 0; j < count; j++) {
                const tool_def_t *t = &tools[j];
                if (!t->name) continue;
                if (j > 0) printf(",");
                /* description needs JSON string escaping */
                printf("{\"name\":\"%s\",\"description\":\"", t->name);
                const char *d = t->description ? t->description : "";
                for (; *d; d++) {
                    if (*d == '"')       printf("\\\"");
                    else if (*d == '\\') printf("\\\\");
                    else if (*d == '\n') printf("\\n");
                    else if (*d == '\r') printf("\\r");
                    else if (*d == '\t') printf("\\t");
                    else                 putchar(*d);
                }
                printf("\",\"input_schema\":%s}",
                       (t->input_schema_json && t->input_schema_json[0])
                           ? t->input_schema_json
                           : "{\"type\":\"object\",\"properties\":{}}");
            }
            printf("]\n");
            free(oneshot_prompt);
            return 0;
        }
        if (strcmp(argv[i], "--anim") == 0) {
            /* --anim <kind|json> [json] — run a direct-to-terminal cell
             * animation (life|rule|attractor|rain). Animation needs real time
             * and a TTY, so it lives here on the direct path, not as a tool.
             *   ./dsco --anim life
             *   ./dsco --anim '{"kind":"life","pattern":"gun"}'
             *   ./dsco --anim rule '{"rule":30}' */
            char jbuf[2048];
            const char *a1 = (i + 1 < argc) ? argv[++i] : "life";
            const char *spec = jbuf;
            if (a1[0] == '{') {
                spec = a1;
            } else if (i + 1 < argc && argv[i + 1][0] == '{') {
                const char *extra = argv[++i];
                snprintf(jbuf, sizeof jbuf, "{\"kind\":\"%s\",%s", a1, extra + 1);
            } else {
                snprintf(jbuf, sizeof jbuf, "{\"kind\":\"%s\"}", a1);
            }
            int frames = anim_dispatch(spec);
            free(oneshot_prompt);
            return frames >= 0 ? 0 : 1;
        }
        if (strcmp(argv[i], "--tool-exec-raw") == 0 && i + 2 < argc) {
            /* --tool-exec-raw <name> <json> — execute a single tool and write its
             * result straight to the terminal (real newlines + ANSI), unescaped.
             * For visual tools (plot, pets, …) whose output is art, not data,
             * `--tool-exec`'s JSON escaping renders newlines as literal "\n". */
            const char *tname = argv[++i];
            const char *tjson = argv[++i];
            main_tools_init_for_runtime(runtime_profile);
            /* Raw dump is for humans (plot/avatar art) — never apply the agent
             * loop's context-economy truncation, or the render gets cut off. */
            tools_set_inline_truncation(false);
            char result[256 * 1024] = {0};
            bool ok = tools_execute(tname, tjson, result, sizeof(result));
            (void)write(STDOUT_FILENO, result, strlen(result));
            size_t rlen = strlen(result);
            if (rlen == 0 || result[rlen - 1] != '\n')
                (void)write(STDOUT_FILENO, "\n", 1);
            free(oneshot_prompt);
            return ok ? 0 : 1;
        }
        if (strcmp(argv[i], "--tool-exec") == 0 && i + 2 < argc) {
            /* --tool-exec <name> <json>  — execute a single tool and print result */
            const char *tname  = argv[++i];
            const char *tjson  = argv[++i];
            main_tools_init_for_runtime(runtime_profile);
            char result[256 * 1024] = {0};
            bool ok = tools_execute(tname, tjson, result, sizeof(result));
            /* Always emit valid JSON: {"ok":bool,"result":"..."} */
            printf("{\"ok\":%s,\"result\":\"", ok ? "true" : "false");
            for (const char *p = result; *p; p++) {
                if (*p == '"')       printf("\\\"");
                else if (*p == '\\') printf("\\\\");
                else if (*p == '\n') printf("\\n");
                else if (*p == '\r') printf("\\r");
                else if (*p == '\t') printf("\\t");
                else                 putchar(*p);
            }
            printf("\"}\n");
            free(oneshot_prompt);
            return ok ? 0 : 1;
        }
        if (strcmp(argv[i], "--") == 0) {
            /* Everything after -- is passthrough to the executor */
            exec_extra = argv + i + 1;
            exec_nextra = argc - i - 1;
            break;
        }
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            model = argv[++i];
            user_set_model = true;
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            api_key = argv[++i];
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "--setup") == 0) {
            setup_mode = true;
        } else if (strcmp(argv[i], "--setup-force") == 0) {
            setup_mode = true;
            setup_force = true;
        } else if (strcmp(argv[i], "--setup-report") == 0) {
            setup_mode = true;
            setup_report_mode = true;
        } else if (strcmp(argv[i], "--workspace-bootstrap") == 0) {
            workspace_bootstrap_mode = true;
        } else if (strcmp(argv[i], "--workspace-status") == 0) {
            workspace_status_mode = true;
        } else if (strcmp(argv[i], "--timeline-server") == 0) {
            timeline_server_mode = true;
        } else if (strcmp(argv[i], "--timeline-port") == 0 && i + 1 < argc) {
            timeline_port = atoi(argv[++i]);
            if (timeline_port <= 0 || timeline_port > 65535) {
                fprintf(stderr, "error: invalid timeline port\n");
                free(oneshot_prompt);
                return 1;
            }
        } else if (strcmp(argv[i], "--timeline-instance") == 0 && i + 1 < argc) {
            timeline_instance_filter = argv[++i];
        } else if (strcmp(argv[i], "--topology") == 0 && i + 1 < argc) {
            topology_name = argv[++i];
        } else if (strcmp(argv[i], "--topology-auto") == 0) {
            topology_auto = true;
        } else if (strcmp(argv[i], "--topology-list") == 0) {
            topology_list_mode = true;
        } else if (strcmp(argv[i], "--topology-show") == 0) {
            topology_show_mode = true;
            if (i + 1 < argc && argv[i+1][0] != '-')
                topology_show_name = argv[++i];
        } else if (strcmp(argv[i], "--ui") == 0 || strcmp(argv[i], "-ui") == 0) {
            ui_mode = true;
            /* Optional port: --ui 8080 | -ui 8080 */
            if (i + 1 < argc && argv[i+1][0] != '-') {
                int p = atoi(argv[i+1]);
                if (p > 0 && p <= 65535) { ui_port = p; i++; }
            }
        } else if (strcmp(argv[i], "--cheap") == 0 || strcmp(argv[i], "-C") == 0) {
            g_cheap_mode = 1;
        } else if (strcmp(argv[i], "--orchestrate") == 0 || strcmp(argv[i], "-O") == 0) {
            orchestrate_mode = true;
        } else if (strcmp(argv[i], "--mux") == 0 || strcmp(argv[i], "mux") == 0) {
            mux_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                mux_initial_root = argv[++i];
            }
        } else if (strcmp(argv[i], "--worker") == 0 ||
                   strcmp(argv[i], "--worker-lite") == 0) {
            /* Spawned by the mux. The project id arg (next token) is informational
             * — env DSCO_PROJECT_ID is already set, and cwd was set by the parent. */
            interactive_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') i++;
        } else if (strcmp(argv[i], "--interactive") == 0 || strcmp(argv[i], "-i") == 0) {
            interactive_mode = true;
        } else if ((strcmp(argv[i], "-M") == 0 || strcmp(argv[i], "--worker-model") == 0) && i + 1 < argc) {
            worker_model = argv[++i];
        } else if ((strcmp(argv[i], "--exec") == 0 || strcmp(argv[i], "-e") == 0) && i + 1 < argc) {
            exec_backend = argv[++i];
        } else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
            exec_backend = argv[++i];
        } else {
            size_t total = 0;
            for (int j = i; j < argc; j++) {
                if (strcmp(argv[j], "--") == 0) break;
                total += strlen(argv[j]) + 1;
            }
            oneshot_prompt = safe_malloc(total + 1);
            oneshot_prompt[0] = '\0';
            for (int j = i; j < argc; j++) {
                if (strcmp(argv[j], "--") == 0) break;
                if (j > i) snprintf(oneshot_prompt + strlen(oneshot_prompt), sizeof(oneshot_prompt) - strlen(oneshot_prompt), " ");
                snprintf(oneshot_prompt + strlen(oneshot_prompt), sizeof(oneshot_prompt) - strlen(oneshot_prompt), "%s", argv[j]);
            }
            /* find -- after prompt if we haven't already */
            for (int j = i; j < argc; j++) {
                if (strcmp(argv[j], "--") == 0) {
                    exec_extra = argv + j + 1;
                    exec_nextra = argc - j - 1;
                    break;
                }
            }
            break;
        }
    }

    /* DSCO_EXEC env var as default */
    if (!exec_backend && !interactive_mode) {
        const char *env_exec = getenv("DSCO_EXEC");
        if (env_exec && env_exec[0]) {
            exec_backend = env_exec;
            exec_backend_from_env = true;
        }
    }

    if (exec_backend_from_env && (user_set_model || model_from_env)) {
        const exec_reg_t *env_exec = exec_find(exec_backend);
        if (env_exec && !model_supports_executor(model, env_exec->name)) {
            fprintf(stderr,
                    "  \033[2mignoring DSCO_EXEC=%s for model %s; using native routing\033[0m\n",
                    exec_backend, model_resolve_alias(model));
            exec_backend = NULL;
            exec_backend_from_env = false;
        }
    }

    /* DSCO_CHEAP env var: "1" or "true" enables cheap mode */
    if (!g_cheap_mode) {
        const char *env_cheap = getenv("DSCO_CHEAP");
        if (env_cheap && (env_cheap[0] == '1' || env_cheap[0] == 't' || env_cheap[0] == 'T'))
            g_cheap_mode = 1;
    }

    /* Output guard redirects stdout/stderr through helper threads. That is fine
     * for native execution, but it breaks external CLI exec because execvp()
     * replaces those helper threads. Decide after saved env has loaded so a
     * persisted DSCO_EXEC=claude/codex is honored. */
    if (!interactive_mode && !(exec_backend && exec_backend[0])) {
        (void)output_guard_init();
    }
    TRACE_INFO("main start");

    bool allows_no_prompt =
        interactive_mode ||
        setup_mode || setup_report_mode ||
        workspace_bootstrap_mode || workspace_status_mode ||
        timeline_server_mode || topology_list_mode || topology_show_mode ||
        ui_mode || (exec_backend && exec_backend[0]);
    if (!oneshot_prompt && !allows_no_prompt) {
        usage(argv[0]);
        fprintf(stderr, "\nerror: prompt required\n");
        return 1;
    }

    {
        char workspace_summary[768];
        int ws_created = dsco_workspace_bootstrap(workspace_summary, sizeof(workspace_summary));
        if (ws_created < 0) {
            fprintf(stderr, "warning: %s\n", workspace_summary);
        } else if (workspace_bootstrap_mode) {
            printf("%s\n", workspace_summary);
            free(oneshot_prompt);
            return 0;
        } else if (ws_created > 0) {
            fprintf(stderr, "%s\n", workspace_summary);
        }
    }

    if (workspace_status_mode) {
        dsco_workspace_status_t st;
        char workspace_summary[768];
        if (dsco_workspace_status(&st, workspace_summary, sizeof(workspace_summary)) < 0) {
            fprintf(stderr, "workspace status failed\n");
            free(oneshot_prompt);
            return 1;
        }
        printf("%s\n", workspace_summary);
        free(oneshot_prompt);
        return 0;
    }

    if (topology_list_mode) {
        print_topology_list();
        free(oneshot_prompt);
        return 0;
    }

    if (topology_show_mode) {
        print_topology_show(topology_show_name);
        free(oneshot_prompt);
        return 0;
    }

    if (setup_mode) {
        if (setup_report_mode) {
            char report[32768];
            if (dsco_setup_report(report, sizeof(report)) < 0) {
                fprintf(stderr, "setup report failed\n");
                free(oneshot_prompt);
                return 1;
            }
            printf("%s", report);
            free(oneshot_prompt);
            return 0;
        }

        char summary[768];
        int discovered = dsco_setup_autopopulate(setup_force, true, summary, sizeof(summary));
        if (discovered < 0) {
            fprintf(stderr, "%s\n", summary);
            free(oneshot_prompt);
            return 1;
        }
        printf("%s\n", summary);
        printf("profile=%s env_file=%s\n",
               dsco_setup_profile_name(), dsco_setup_env_path());
        if (loaded_env_count > 0) {
            printf("startup loaded %d key(s) from %s\n",
                   loaded_env_count, dsco_setup_env_path());
        }
        free(oneshot_prompt);
        return 0;
    }

    if (ui_mode) {
        /* Launch the FastAPI web UI server */
        char server_path[PATH_MAX];
        /* Try: <binary_dir>/../web/server.py, then ./web/server.py */
        char exe_path[PATH_MAX];
        bool found_server = false;
#ifdef __APPLE__
        uint32_t exe_size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &exe_size) == 0) {
#elif defined(__linux__)
        if (readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1) > 0) {
#else
        if (0) {
#endif
            char *resolved = realpath(exe_path, NULL);
            if (resolved) {
                char *dir = dirname(resolved);
                snprintf(server_path, sizeof(server_path), "%s/../web/server.py", dir);
                char *sp = realpath(server_path, NULL);
                if (sp) { strncpy(server_path, sp, sizeof(server_path) - 1); free(sp); found_server = true; }
                free(resolved);
            }
        }
        if (!found_server) {
            snprintf(server_path, sizeof(server_path), "%s/web/server.py", getenv("PWD") ? getenv("PWD") : ".");
        }

        char port_str[16], cwd_str[PATH_MAX];
        snprintf(port_str, sizeof(port_str), "%d", ui_port);
        if (!getcwd(cwd_str, sizeof(cwd_str))) strncpy(cwd_str, ".", sizeof(cwd_str));

        fprintf(stderr,
            "\033[36m"
            "  dsco --ui launching web server...\n"
            "  server: %s\n"
            "  port:   %s\n"
            "  dir:    %s\n"
            "\033[0m\n", server_path, port_str, cwd_str);

        free(oneshot_prompt);

        /* Prefer the project-local venv (has fastapi, anthropic, etc.); fall
         * back to system python3 if the venv doesn't exist. */
        char venv_python[PATH_MAX];
        snprintf(venv_python, sizeof(venv_python), "%s/.web-venv/bin/python",
                 cwd_str);
        if (access(venv_python, X_OK) == 0) {
            execlp(venv_python, venv_python, server_path,
                   "--port", port_str,
                   "--dir", cwd_str,
                   "--model", model,
                   "--open",
                   (char *)NULL);
        } else {
            execlp("python3", "python3", server_path,
                   "--port", port_str,
                   "--dir", cwd_str,
                   "--model", model,
                   "--open",
                   (char *)NULL);
        }
        /* Only reached on exec failure */
        perror("failed to launch web UI (is python3 + fastapi installed?)");
        fprintf(stderr, "  install deps: uv pip install --python .web-venv/bin/python -r web/requirements.txt\n");
        return 1;
    }

    if (timeline_server_mode) {
        if (!baseline_start(model, "timeline-server")) {
            fprintf(stderr, "error: failed to start baseline sqlite storage\n");
            return 1;
        }
        baseline_log("setup", "env_loaded", dsco_setup_env_path(), NULL);
        if (loaded_env_count > 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "loaded %d key(s) from setup env", loaded_env_count);
            baseline_log("setup", "keys_loaded", msg, NULL);
        }
        int rc = baseline_serve_http(timeline_port, timeline_instance_filter);
        baseline_stop();
        return rc == 0 ? 0 : 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Kick off the background OpenRouter catalog refresh: loads the on-disk
     * cache immediately, then refreshes from the live /models endpoint when
     * stale, so any real slug resolves with current context/pricing. */
    openrouter_cache_init();

    /* --exec: dispatch to external CLI or force native provider */
    if (exec_backend && exec_backend[0]) {
        if (strcmp(exec_backend, "list") == 0) {
            exec_list();
            free(oneshot_prompt);
            return 0;
        }

        /* "smart" — use router to pick best provider+model for the task */
        if (strcmp(exec_backend, "smart") == 0) {
            if (!oneshot_prompt) {
                fprintf(stderr, "error: smart mode requires a prompt\n");
                return 1;
            }
            task_complexity_t tc = router_classify_task(oneshot_prompt, 0, 0, 0);
            bool prefer_code = prompt_looks_code_task(oneshot_prompt);
            const exec_reg_t *smart_exec =
                select_auto_executor(model, oneshot_prompt, user_set_model);

            if (smart_exec) {
                const char *smart_model = user_set_model
                    ? normalize_model_for_executor(smart_exec, model)
                    : default_model_for_executor(smart_exec);
                fprintf(stderr, "  \033[2m[smart] task=%s → %s executor (%s)\033[0m\n",
                        task_complexity_name(tc), smart_exec->name,
                        smart_model && smart_model[0] ? smart_model : "default");
                exec_dispatch(smart_exec, oneshot_prompt, smart_model,
                              exec_extra, exec_nextra, api_key);
                free(oneshot_prompt);
                return 1;
            }

            if (!user_set_model) {
                model = provider_select_default_primary_model(prefer_code);
            }
            fprintf(stderr, "  \033[2m[smart] task=%s → native %s (%s)\033[0m\n",
                    task_complexity_name(tc),
                    provider_route_for_model(model, api_key, g_provider_override),
                    model);
            goto native_path;
        }

        /* "bench-tools" — test tool calling across providers */
        if (strcmp(exec_backend, "bench-tools") == 0) {
            main_tools_init_for_runtime(runtime_profile);
            fprintf(stderr, "\n  \033[1mTool Calling Benchmark\033[0m\n");
            fprintf(stderr, "  Tests: tool invocation, multi-turn, arg parsing\n\n");
            fprintf(stderr, "  %-12s %-24s %7s  %-7s %-7s %-7s %s\n",
                    "PROVIDER", "MODEL", "TIME", "INVOKE", "ARGS", "MULTI", "NOTES");
            fprintf(stderr, "  %-12s %-24s %7s  %-7s %-7s %-7s %s\n",
                    "────────", "─────", "────", "──────", "────", "─────", "─────");

            for (int i = 0; NATIVE_PROVIDERS[i].name; i++) {
                const native_provider_t *tp = &NATIVE_PROVIDERS[i];
                const char *tkey = provider_resolve_request_api_key(tp->name, NULL);
                if (!tkey || !tkey[0]) {
                    fprintf(stderr, "  %-12s %-24s %7s  %-7s %-7s %-7s %s\n",
                            tp->name, tp->example_model, "-", "-", "-", "-", "no key");
                    continue;
                }
                if (!(tp->caps & CAP_TOOLS)) {
                    fprintf(stderr, "  %-12s %-24s %7s  %-7s %-7s %-7s %s\n",
                            tp->name, tp->example_model, "-", "-", "-", "-", "no tool support");
                    continue;
                }

                session_state_t tsess;
                session_state_init(&tsess, tp->example_model);
                provider_t *tprov = provider_create(tp->name);
                provider_prepare(tprov);
                conversation_t tconv;
                conv_init(&tconv);

                /* Test 1: Does the model invoke a tool? */
                conv_add_user_text(&tconv, "Read the file README.md and tell me the first line.");

                struct timeval tt0, tt1;
                gettimeofday(&tt0, NULL);

                char *treq = tprov->build_request(tprov, &tconv, &tsess, 1024, tkey);
                stream_result_t tsr = {0};
                if (treq) {
                    tsr = provider_stream_reuse(tprov, tkey, treq,
                                                NULL, NULL, NULL, NULL);
                    free(treq);
                }

                gettimeofday(&tt1, NULL);
                double telapsed = (tt1.tv_sec - tt0.tv_sec) + (tt1.tv_usec - tt0.tv_usec) / 1e6;

                bool tool_invoked = false;
                bool args_valid = false;
                char tool_name[64] = "";
                if (tsr.ok) {
                    for (int bi = 0; bi < tsr.parsed.count; bi++) {
                        content_block_t *blk = &tsr.parsed.blocks[bi];
                        if (blk->type && strcmp(blk->type, "tool_use") == 0) {
                            tool_invoked = true;
                            if (blk->tool_name)
                                snprintf(tool_name, sizeof(tool_name), "%s", blk->tool_name);
                            /* Check if args is valid JSON with expected field */
                            if (blk->tool_input && blk->tool_input[0] == '{')
                                args_valid = true;
                            break;
                        }
                    }
                }

                /* Test 2: Multi-turn — add tool result and see if model continues */
                bool multi_turn_ok = false;
                if (tool_invoked && tsr.ok) {
                    conv_add_assistant_raw(&tconv, &tsr.parsed);
                    /* Find the tool_use id and add a result */
                    for (int bi = 0; bi < tsr.parsed.count; bi++) {
                        content_block_t *blk = &tsr.parsed.blocks[bi];
                        if (blk->type && strcmp(blk->type, "tool_use") == 0 && blk->tool_id) {
                            conv_add_tool_result_named(&tconv, blk->tool_id, blk->tool_name,
                                                       "# dsco-cli\nThin agentic CLI", false);
                            break;
                        }
                    }
                    json_free_response(&tsr.parsed);

                    /* Second turn */
                    char *treq2 = tprov->build_request(tprov, &tconv, &tsess, 1024, tkey);
                    stream_result_t tsr2 = {0};
                    if (treq2) {
                        tsr2 = provider_stream_reuse(tprov, tkey, treq2,
                                                     NULL, NULL, NULL, NULL);
                        free(treq2);
                    }
                    multi_turn_ok = tsr2.ok;
                    /* Check if response has text */
                    if (tsr2.ok) {
                        for (int bi = 0; bi < tsr2.parsed.count; bi++) {
                            if (tsr2.parsed.blocks[bi].text && tsr2.parsed.blocks[bi].text[0]) {
                                multi_turn_ok = true;
                                break;
                            }
                        }
                    }
                    json_free_response(&tsr2.parsed);
                } else {
                    json_free_response(&tsr.parsed);
                }

                gettimeofday(&tt1, NULL);
                telapsed = (tt1.tv_sec - tt0.tv_sec) + (tt1.tv_usec - tt0.tv_usec) / 1e6;

                char notes[128] = "";
                if (!tsr.ok)
                    snprintf(notes, sizeof(notes), "HTTP error");
                else if (tool_name[0])
                    snprintf(notes, sizeof(notes), "called %s", tool_name);

                fprintf(stderr, "  %-12s %-24s %6.1fs  %s%-7s%s %s%-7s%s %s%-7s%s %s\n",
                        tp->name, tp->example_model, telapsed,
                        tool_invoked ? "\033[32m" : "\033[31m",
                        tool_invoked ? "pass" : "FAIL", "\033[0m",
                        args_valid ? "\033[32m" : "\033[31m",
                        args_valid ? "pass" : "FAIL", "\033[0m",
                        multi_turn_ok ? "\033[32m" : "\033[31m",
                        multi_turn_ok ? "pass" : "FAIL", "\033[0m",
                        notes);

                conv_free(&tconv);
                provider_free(tprov);
            }
            fprintf(stderr, "\n");
            free(oneshot_prompt);
            return 0;
        }

        /* "bench" — benchmark all available providers */
        if (strcmp(exec_backend, "bench") == 0) {
            const char *bench_prompt = oneshot_prompt
                ? oneshot_prompt
                : "What is the factorial of 7? Reply with just the number.";
            fprintf(stderr, "\n  \033[1mProvider Benchmark\033[0m\n");
            fprintf(stderr, "  prompt: \"%s\"\n\n", bench_prompt);
            fprintf(stderr, "  %-12s %-28s %7s  %5s  %s\n",
                    "PROVIDER", "MODEL", "TIME", "OK", "RESPONSE");
            fprintf(stderr, "  %-12s %-28s %7s  %5s  %s\n",
                    "────────", "─────", "────", "──", "────────");

            for (int i = 0; NATIVE_PROVIDERS[i].name; i++) {
                const native_provider_t *np2 = &NATIVE_PROVIDERS[i];
                const char *bkey = provider_resolve_request_api_key(np2->name, NULL);
                if (!bkey || !bkey[0]) {
                    fprintf(stderr, "  %-12s %-28s %7s  %5s  %s\n",
                            np2->name, np2->example_model, "-", "-", "no key");
                    continue;
                }

                /* Build a minimal oneshot request and stream it */
                session_state_t bsess;
                session_state_init(&bsess, np2->example_model);
                provider_t *bprov = provider_create(np2->name);
                provider_prepare(bprov);
                conversation_t bconv;
                conv_init(&bconv);
                conv_add_user_text(&bconv, bench_prompt);

                char *breq = bprov->build_request(bprov, &bconv, &bsess, 256, bkey);
                struct timeval bt0, bt1;
                gettimeofday(&bt0, NULL);

                /* Capture text into buffer */
                char bench_text[512] = "";
                int bench_text_len = 0;

                (void)bench_text_len;
                stream_result_t bsr = {0};

                if (breq) {
                    bsr = provider_stream_reuse(bprov, bkey, breq,
                                                NULL, NULL, NULL, NULL);
                    free(breq);
                }

                gettimeofday(&bt1, NULL);
                double belapsed = (bt1.tv_sec - bt0.tv_sec) + (bt1.tv_usec - bt0.tv_usec) / 1e6;

                /* Extract text from response */
                if (bsr.ok) {
                    for (int bi = 0; bi < bsr.parsed.count; bi++) {
                        if (bsr.parsed.blocks[bi].text) {
                            snprintf(bench_text, sizeof(bench_text), "%s",
                                     bsr.parsed.blocks[bi].text);
                            break;
                        }
                    }
                }

                /* Truncate response for display */
                for (int j = 0; bench_text[j]; j++) {
                    if (bench_text[j] == '\n') bench_text[j] = ' ';
                }
                if (strlen(bench_text) > 50) {
                    bench_text[47] = '.';
                    bench_text[48] = '.';
                    bench_text[49] = '.';
                    bench_text[50] = '\0';
                }

                fprintf(stderr, "  %-12s %-28s %6.1fs  %s%-5s%s  %s\n",
                        np2->name, np2->example_model,
                        belapsed,
                        bsr.ok ? "\033[32m" : "\033[31m",
                        bsr.ok ? "ok" : "FAIL",
                        "\033[0m",
                        bsr.ok ? bench_text : "request failed");

                json_free_response(&bsr.parsed);
                conv_free(&bconv);
                provider_free(bprov);
            }

            /* done */

            fprintf(stderr, "\n");
            free(oneshot_prompt);
            return 0;
        }

        if (strcmp(exec_backend, "smoke") == 0 ||
            strcmp(exec_backend, "smoke-full") == 0) {
            bool full = strcmp(exec_backend, "smoke-full") == 0 ||
                        main_env_truthy(getenv("DSCO_SMOKE_FULL"));
            free(oneshot_prompt);
            return run_provider_smoke(argv[0], full);
        }

        /* Check if it's a native provider name */
        const native_provider_t *np = native_find(exec_backend);
        if (np) {
            /* Force this provider — let native_path resolve the best credential
               so Claude Code OAuth discovery still works for Anthropic. */
            g_provider_override = np->name;
            /* If user didn't pick a model, suggest one for this provider */
            if (!user_set_model && np->example_model) {
                model = np->example_model;
                fprintf(stderr, "  %s%s → %s%s\n", "\033[2m", np->name, model, "\033[0m");
            }
            /* Fall through to normal dsco oneshot/interactive path */
            goto native_path;
        }

        if (provider_has_custom_api_base(exec_backend) &&
            provider_has_usable_key(exec_backend, NULL)) {
            g_provider_override = exec_backend;
            if (!user_set_model) {
                fprintf(stderr, "error: generic provider '%s' requires -m MODEL\n",
                        exec_backend);
                free(oneshot_prompt);
                return 1;
            }
            fprintf(stderr, "  \033[2m%s via custom API base\033[0m\n",
                    exec_backend);
            goto native_path;
        }

        /* "auto" — pick first available external CLI */
        const exec_reg_t *ereg = NULL;
        if (strcmp(exec_backend, "auto") == 0) {
            ereg = select_auto_executor(model, oneshot_prompt, user_set_model);
            if (ereg) {
                const char *picked_model = user_set_model
                    ? normalize_model_for_executor(ereg, model)
                    : default_model_for_executor(ereg);
                fprintf(stderr, "  auto-selected: %s\n", ereg->name);
                exec_dispatch(ereg, oneshot_prompt, picked_model,
                              exec_extra, exec_nextra, api_key);
                free(oneshot_prompt);
                return 1;
            }

            if (!user_set_model) {
                model = provider_select_default_primary_model(
                    prompt_looks_code_task(oneshot_prompt));
            }
            fprintf(stderr, "  auto-selected: native %s (%s)\n",
                    provider_route_for_model(model, api_key, g_provider_override),
                    model);
            goto native_path;
        } else {
            ereg = exec_find(exec_backend);
            if (!ereg) {
                fprintf(stderr, "error: unknown executor '%s'\n", exec_backend);
                fprintf(stderr, "  external CLIs: ");
                for (int i = 0; EXEC_REGISTRY[i].name; i++)
                    fprintf(stderr, "%s%s", i ? ", " : "", EXEC_REGISTRY[i].name);
                fprintf(stderr, "\n  native providers: ");
                for (int i = 0; NATIVE_PROVIDERS[i].name; i++)
                    fprintf(stderr, "%s%s", i ? ", " : "", NATIVE_PROVIDERS[i].name);
                fprintf(stderr, "\n  special: auto, smart, list, bench, bench-tools, smoke, smoke-full\n");
                free(oneshot_prompt);
                return 1;
            }
        }

        /* exec replaces the process — never returns on success */
        if ((user_set_model || model_from_env) &&
            !model_supports_executor(model, ereg->name)) {
            print_executor_model_mismatch(ereg, model);
            free(oneshot_prompt);
            return 1;
        }
        exec_dispatch(ereg, oneshot_prompt,
                      user_set_model ? normalize_model_for_executor(ereg, model) : NULL,
                      exec_extra, exec_nextra, api_key);
        /* only reached on exec failure */
        free(oneshot_prompt);
        return 1;
    }

native_path:
    ;

    /* Resolve API key for the active provider */
    const char *active_provider = provider_route_for_model(model, api_key, g_provider_override);
    const char *resolved_api_key =
        provider_resolve_request_api_key(active_provider, api_key);
    if (!resolved_api_key || resolved_api_key[0] == '\0') {
        fprintf(stderr, "error: credentials not set for provider '%s'\n", active_provider);
        if (strcmp(active_provider, "anthropic") == 0) {
            fprintf(stderr, "  use -k KEY, export ANTHROPIC_API_KEY, or sign in with Claude Code\n");
        } else {
            fprintf(stderr, "  use -k KEY or export the provider-specific env var\n");
        }
        free(oneshot_prompt);
        return 1;
    }
    api_key = resolved_api_key;
    provider_debug_log_request(active_provider, model, api_key);

    if (!baseline_start(model, oneshot_prompt ? "oneshot" : "interactive")) {
        fprintf(stderr, "warning: baseline disabled (sqlite unavailable)\n");
    }

    if (oneshot_prompt) {
        main_tools_init_for_runtime(runtime_profile);
        tools_register_vm_dispatch(&g_vm);  /* §3: populate VM dispatch table */
        tools_set_runtime_api_key(api_key);
        tools_set_runtime_model(model);
        md_init(&s_oneshot_md, stdout);

        if (topology_name || topology_auto) {
            int rc = run_oneshot_topology(api_key, model, topology_name, topology_auto, oneshot_prompt);
            free(oneshot_prompt);
            return rc;
        }

        conversation_t conv;
        conv_init(&conv);
        conv_add_user_text(&conv, oneshot_prompt);
        baseline_log("user", "oneshot_prompt", oneshot_prompt, NULL);
        session_state_t oneshot_session;
        session_state_init(&oneshot_session, model);
        const char *oneshot_provider_name =
            provider_route_for_model(oneshot_session.model, api_key, g_provider_override);
        provider_t *oneshot_provider = provider_create(oneshot_provider_name);
        provider_prepare(oneshot_provider);
        /* Resolve the correct API key for this provider (e.g. OPENROUTER_API_KEY) */
        const char *oneshot_key =
            provider_resolve_request_api_key(oneshot_provider_name, api_key);
        if (!oneshot_key || !oneshot_key[0]) oneshot_key = api_key;

        int turns = 0;
        bool oneshot_had_error = false;
        tools_loop_control_reset();
        /* Agentic headless loop: run to the goal, bounded only by the cost
         * budget and the runaway backstop — not an arbitrary turn count. Since
         * there is no human to interrupt here, the cost gate below is the
         * primary stop. DSCO_BUDGET (dollars, 0 = unlimited) controls it;
         * defaults to $5 to match the interactive session cap. */
        int oneshot_hard_ceiling = dsco_hard_turn_ceiling();
        double oneshot_budget = 5.0;
        {
            const char *b = getenv("DSCO_BUDGET");
            if (b && b[0]) oneshot_budget = atof(b);
        }
        while (turns < oneshot_hard_ceiling && !g_main_interrupted) {
            if (oneshot_budget > 0 &&
                oneshot_session.total_reported_cost_usd >= oneshot_budget) {
                fprintf(stderr, "error: cost budget exceeded: $%.4f / $%.4f "
                        "(raise via DSCO_BUDGET)\n",
                        oneshot_session.total_reported_cost_usd, oneshot_budget);
                oneshot_had_error = true;
                break;
            }
            turns++;
            md_reset(&s_oneshot_md);

            char *req = oneshot_provider
                ? oneshot_provider->build_request(oneshot_provider, &conv, &oneshot_session,
                                                 MAX_TOKENS, oneshot_key)
                : llm_build_request_ex_for_credential(&conv, &oneshot_session,
                                                      MAX_TOKENS, oneshot_key);
            if (!req) {
                fprintf(stderr, "error: failed to build request\n");
                baseline_log("error", "request_build_failed", NULL, NULL);
                oneshot_had_error = true;
                break;
            }

            stream_result_t sr = oneshot_provider
                ? provider_stream_reuse(oneshot_provider, oneshot_key, req,
                                        oneshot_text_cb, oneshot_tool_cb,
                                        NULL, NULL)
                : llm_stream(oneshot_key, req,
                             oneshot_text_cb,
                             oneshot_tool_cb,
                             NULL, NULL);
            free(req);

            if (!sr.ok) {
                fprintf(stderr, "error: stream failed (HTTP %d)\n", sr.http_status);
                char err[64];
                snprintf(err, sizeof(err), "HTTP %d", sr.http_status);
                baseline_log("error", "stream_failed", err, NULL);
                json_free_response(&sr.parsed);
                if (turns == 1) conv_pop_last(&conv);
                oneshot_had_error = true;
                break;
            }

            /* Flush rendered markdown + newline after streamed text */
            md_flush(&s_oneshot_md);
            printf("\n");

            conv_add_assistant_raw(&conv, &sr.parsed);

            bool has_tool_use = false;
            for (int i = 0; i < sr.parsed.count; i++) {
                content_block_t *blk = &sr.parsed.blocks[i];
                if (blk->type && strcmp(blk->type, "tool_use") == 0) {
                    has_tool_use = true;
                    char *tr = safe_malloc(MAX_TOOL_RESULT);
                    tr[0] = '\0';
                    const char *tier = session_trust_tier_to_string(oneshot_session.trust_tier);
                    bool ok = tools_is_allowed_for_tier(blk->tool_name, tier, tr, MAX_TOOL_RESULT);
                    if (ok) {
                        ok = tools_execute_for_tier(blk->tool_name, blk->tool_input, tier,
                                                    tr, MAX_TOOL_RESULT);
                    } else {
                        baseline_log("security", "tool_blocked", tr, NULL);
                    }
                    conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name, tr, !ok);
                    baseline_log(ok ? "tool_result" : "tool_error",
                                 blk->tool_name ? blk->tool_name : "tool",
                                 tr, NULL);
                    free(tr);
                }
            }

            bool done = !has_tool_use ||
                        (sr.parsed.stop_reason &&
                         strcmp(sr.parsed.stop_reason, "end_turn") == 0);

            loop_control_decision_t loop_decision;
            tools_loop_control_decide(turns, done, has_tool_use,
                                      &loop_decision);
            if (loop_decision.force_continue && loop_decision.prompt[0]) {
                conv_add_user_text(&conv, loop_decision.prompt);
                done = false;
                fprintf(stderr, "  \033[2mloop construct: %s\033[0m\n",
                        loop_decision.reason[0] ? loop_decision.reason
                                                 : "continuing");
                baseline_log("agent", "loop_construct_continue",
                             loop_decision.reason, NULL);
            }
            if (loop_decision.force_done) {
                done = true;
                baseline_log("agent", "loop_construct_done",
                             loop_decision.reason, NULL);
            }

            baseline_log("turn",
                         done ? "turn_done" : "turn_continue",
                         sr.parsed.stop_reason ? sr.parsed.stop_reason : "",
                         NULL);
            json_free_response(&sr.parsed);
            if (done) break;
        }

        /* Sub-agent mode: after initial task, check IPC queue for more work */
        if (getenv("DSCO_SUBAGENT") && getenv("DSCO_IPC_DB")) {
            ipc_init(NULL, NULL);
            struct sigaction sa_term;
            sa_term.sa_handler = main_sigterm_handler;
            sa_term.sa_flags = 0;
            sigemptyset(&sa_term.sa_mask);
            sigaction(SIGTERM, &sa_term, NULL);

            /* Crash handlers — log diagnostics before dying */
            main_install_crash_handlers();

            const char *depth_s = getenv("DSCO_SWARM_DEPTH");
            ipc_register(getenv("DSCO_PARENT_INSTANCE_ID"),
                         depth_s ? atoi(depth_s) : 0, "worker", "*");
            ipc_set_status(IPC_AGENT_IDLE, "initial task complete");

            /* Check for queued tasks — long-running agent mode */
            while (!g_main_interrupted) {
                ipc_task_t task;
                if (!ipc_task_claim(&task)) break;

                ipc_task_start(task.id);
                ipc_set_status(IPC_AGENT_WORKING, task.description);

                /* Run the claimed task as a new conversation turn */
                conv_add_user_text(&conv, task.description);

                int t2 = 0;
                bool task_ok = true;
                tools_loop_control_reset();
                /* Agentic per-task loop: bounded by cost budget + runaway
                 * backstop, not a fixed turn count (see oneshot loop above). */
                int task_hard_ceiling = dsco_hard_turn_ceiling();
                double task_budget = 5.0;
                {
                    const char *b = getenv("DSCO_BUDGET");
                    if (b && b[0]) task_budget = atof(b);
                }
                while (t2 < task_hard_ceiling && !g_main_interrupted) {
                    if (task_budget > 0 &&
                        oneshot_session.total_reported_cost_usd >= task_budget) {
                        fprintf(stderr, "error: cost budget exceeded: $%.4f / "
                                "$%.4f (raise via DSCO_BUDGET)\n",
                                oneshot_session.total_reported_cost_usd, task_budget);
                        task_ok = false;
                        break;
                    }
                    t2++;
                    md_reset(&s_oneshot_md);
                    char *req2 = oneshot_provider
                        ? oneshot_provider->build_request(oneshot_provider, &conv, &oneshot_session,
                                                         MAX_TOKENS, oneshot_key)
                        : llm_build_request_ex_for_credential(&conv, &oneshot_session,
                                                              MAX_TOKENS, oneshot_key);
                    if (!req2) { task_ok = false; break; }

                    stream_result_t sr2 = oneshot_provider
                        ? provider_stream_reuse(oneshot_provider, oneshot_key, req2,
                                                oneshot_text_cb, oneshot_tool_cb,
                                                NULL, NULL)
                        : llm_stream(oneshot_key, req2,
                                     oneshot_text_cb,
                                     oneshot_tool_cb,
                                     NULL, NULL);
                    free(req2);
                    if (!sr2.ok) {
                        json_free_response(&sr2.parsed);
                        task_ok = false;
                        break;
                    }

                    md_flush(&s_oneshot_md);
                    printf("\n");
                    conv_add_assistant_raw(&conv, &sr2.parsed);

                    bool has_tu = false;
                    for (int i = 0; i < sr2.parsed.count; i++) {
                        content_block_t *blk = &sr2.parsed.blocks[i];
                        if (blk->type && strcmp(blk->type, "tool_use") == 0) {
                            has_tu = true;
                            char *tr = safe_malloc(MAX_TOOL_RESULT);
                            tr[0] = '\0';
                            const char *tier = session_trust_tier_to_string(oneshot_session.trust_tier);
                            bool ok = tools_is_allowed_for_tier(blk->tool_name, tier, tr, MAX_TOOL_RESULT);
                            if (ok) {
                                ok = tools_execute_for_tier(blk->tool_name, blk->tool_input, tier,
                                                            tr, MAX_TOOL_RESULT);
                            } else {
                                baseline_log("security", "tool_blocked", tr, NULL);
                            }
                            conv_add_tool_result_named(&conv, blk->tool_id, blk->tool_name, tr, !ok);
                            free(tr);
                        }
                    }

                    bool d2 = !has_tu || (sr2.parsed.stop_reason &&
                              strcmp(sr2.parsed.stop_reason, "end_turn") == 0);
                    loop_control_decision_t loop_decision2;
                    tools_loop_control_decide(t2, d2, has_tu, &loop_decision2);
                    if (loop_decision2.force_continue && loop_decision2.prompt[0]) {
                        conv_add_user_text(&conv, loop_decision2.prompt);
                        d2 = false;
                        baseline_log("agent", "loop_construct_continue",
                                     loop_decision2.reason, NULL);
                    }
                    if (loop_decision2.force_done) {
                        d2 = true;
                        baseline_log("agent", "loop_construct_done",
                                     loop_decision2.reason, NULL);
                    }
                    json_free_response(&sr2.parsed);
                    ipc_heartbeat();
                    if (d2) break;
                }

                /* Report task result — last assistant text */
                const char *task_result = "";
                if (conv.count > 0) {
                    message_t *last = &conv.msgs[conv.count - 1];
                    if (last->role == ROLE_ASSISTANT && last->content_count > 0 &&
                        last->content[0].text) {
                        task_result = last->content[0].text;
                    }
                }
                if (task_ok)
                    ipc_task_complete(task.id, task_result);
                else
                    ipc_task_fail(task.id, "execution failed");

                ipc_set_status(IPC_AGENT_IDLE, "");
            }
            ipc_shutdown();
        }

        provider_free(oneshot_provider);
        conv_free(&conv);
        free(oneshot_prompt);
        if (oneshot_had_error) return 1;
    } else if (mux_mode) {
        const char *root = mux_initial_root;
        char cwd_buf[PATH_MAX];
        if (!root) {
            if (getcwd(cwd_buf, sizeof(cwd_buf))) root = cwd_buf;
        }
        dsco_mux_run(api_key, root);
    } else if (orchestrate_mode) {
        agent_run_orchestrated(api_key, model, worker_model, g_provider_override);
    } else {
        agent_run(api_key, model, topology_name, topology_auto, g_provider_override);
    }

    curl_global_cleanup();
    baseline_stop();
    return 0;
}
