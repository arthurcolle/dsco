#include "agent.h"
#include "config.h"
#include "tui.h"
#include "llm.h"
#include "tools.h"
#include "json_util.h"
#include "ipc.h"
#include "md.h"
#include "baseline.h"
#include "setup.h"
#include "provider.h"
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
#include "memory_tier.h"
#include "pheromone.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static md_renderer_t s_oneshot_md;

/* ── Post-LLM Virtual OS subsystems ────────────────────────────────── */
vm_t                g_vm;          /* §3: bytecode dispatch VM (extern'd by tools.c) */
static scheduler_t  g_scheduler;   /* §1/§7: cooperative task scheduler */
static ev_loop_t   *g_ev_loop;     /* §6: event loop */
static vfs_db_t    *g_vfs;         /* §8: embedded persistence */

/* Signal handler for clean IPC shutdown in sub-agent mode */
static volatile sig_atomic_t g_main_interrupted = 0;

static void init_trace_runtime(void) {
#ifdef DSCO_DEV_BINARY
    if (!getenv("DSCO_TRACE")) {
        setenv("DSCO_TRACE", "debug", 1);
    }
#endif
    TRACE_INIT();
}

static void main_sigterm_handler(int sig) {
    (void)sig;
    g_main_interrupted = 1;
}

/* Crash handler — save diagnostic info before dying */
static void crash_handler(int sig) {
    /* Async-signal-safe only: write, _exit */
    const char *name = sig == SIGSEGV ? "SIGSEGV" :
                       sig == SIGBUS  ? "SIGBUS"  :
                       sig == SIGABRT ? "SIGABRT" : "UNKNOWN";
    int fd = open("/tmp/dsco_crash.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "\n[dsco crash] signal=%s(%d) pid=%d\n", name, sig, getpid());
        if (n > 0) write(fd, buf, (size_t)n);
        close(fd);
    }
    /* Re-raise with default handler */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void main_atexit_handler(void) {
    /* Shutdown Post-LLM OS subsystems */
    if (g_vfs)     { vfs_close(g_vfs); g_vfs = NULL; }
    if (g_ev_loop) { ev_loop_free(g_ev_loop); g_ev_loop = NULL; }
    sched_destroy(&g_scheduler);
    arena_subsystem_shutdown();
    TRACE_SHUTDOWN();
    ipc_shutdown();
}

static void init_vos_subsystems(void) {
    /* §2: Arena allocator — scratch (per-turn) + session (per-run) */
    arena_subsystem_init();

    /* §6: Event loop — kqueue on macOS, poll fallback */
    g_ev_loop = ev_loop_new();

    /* §3: Bytecode VM — computed-goto dispatch for tool routing */
    vm_init(&g_vm);

    /* §1/§7: Cooperative scheduler — priority-aware task scheduling */
    sched_init(&g_scheduler);

    /* §8: Embedded persistence — SQLite VFS layer */
    char vfs_path[512];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(vfs_path, sizeof(vfs_path), "%s/.dsco/vfs.db", home);
        g_vfs = vfs_open(vfs_path);
    }

    /* Cross-module wiring: connect subsystems to each other */
    if (g_vfs) {
        /* §8→baseline: mirror baseline events to VFS event log */
        baseline_set_vfs(g_vfs);
        /* §8→memory: persist semantic memories to VFS */
        memory_store_set_vfs(g_vfs);
    }
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
    { "openrouter", "OpenRouter (multi-model)",   "OPENROUTER_API_KEY", "anthropic/claude-opus-4-6",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_VISION|CAP_JSON, 4 },
    { "groq",       "Groq (fast inference)",     "GROQ_API_KEY",        "llama-3.3-70b-versatile",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_JSON, 2 },
    { "deepseek",   "DeepSeek API",              "DEEPSEEK_API_KEY",    "deepseek-chat",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_THINKING|CAP_JSON, 3 },
    { "mistral",    "Mistral AI API",            "MISTRAL_API_KEY",     "mistral-large-latest",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_JSON, 3 },
    { "xai",        "xAI Grok API",              "XAI_API_KEY",         "grok-3-beta",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_VISION|CAP_JSON, 3 },
    { "together",   "Together AI",               "TOGETHER_API_KEY",    "meta-llama/Llama-4-Maverick-17B-128E-Instruct-FP8",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING, 2 },
    { "perplexity", "Perplexity AI",             "PERPLEXITY_API_KEY",  "sonar-pro",
      CAP_MULTITURN|CAP_STREAMING, 2 },
    { "cerebras",   "Cerebras (fast inference)",  "CEREBRAS_API_KEY",   "qwen-3-235b-a22b-instruct-2507",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING, 2 },
    { "cohere",     "Cohere API",                "COHERE_API_KEY",      "command-a-03-2025",
      CAP_TOOLS|CAP_MULTITURN|CAP_STREAMING|CAP_JSON, 3 },
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
        const char *key = getenv(np->env_key);
        bool has_key = key && key[0];

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
    fprintf(stderr, "  %sSpecial: auto, smart, list, bench%s\n\n",
            "\033[2m", "\033[0m");
}

/* Build argv and exec. Never returns on success. */
static void exec_dispatch(const exec_reg_t *e, const char *prompt,
                          const char *model_override, char **extra, int nextra) {
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
    execvp(e->bin, (char *const *)av);
    /* only reached on failure */
    perror(e->bin);
}

/* Global provider override — set by -e <native_provider> */
static const char *g_provider_override = NULL;

static void usage(const char *prog) {
    fprintf(stderr,
        "dsco v%s — thin agentic CLI (streaming + prompt caching)\n"
        "\n"
        "Usage: %s [options] [prompt]\n"
        "\n"
        "Options:\n"
        "  -m MODEL    Model name (default: %s)\n"
        "  -k KEY      API key (default: provider env for selected model)\n"
        "  --profile NAME         Setup profile (default: default)\n"
        "  --setup                Save detected API keys/tokens into dsco env file\n"
        "  --setup-force          Overwrite existing saved values from current env\n"
        "  --setup-report         Show masked setup/config status\n"
        "  --workspace-bootstrap  Create claw workspace files under ~/.dsco/workspace\n"
        "  --workspace-status     Show claw workspace status\n"
        "  --timeline-server        Run local timeline web server\n"
        "  --timeline-port PORT     Timeline webserver port (default: 8421)\n"
        "  --timeline-instance ID   Filter timeline to one instance ID\n"
        "  --topology NAME        Run/select an agent topology\n"
        "  --topology-auto        Auto-pick a topology for the task\n"
        "  --topology-list        List available topologies\n"
        "  --ui [PORT]            Launch web UI (default port: 3141)\n"
        "  -e, --exec BACKEND    Execute via external CLI (claude, codex, list, auto)\n"
        "  --                    Pass remaining args to executor (after -e)\n"
        "  --version              Print version and build info\n"
        "  -h          Show this help\n"
        "\n"
        "Interactive mode: run without a prompt\n"
        "One-shot mode:    %s 'write a hello world in C and compile it'\n"
        "\n"
        "Environment:\n"
        "  ANTHROPIC_API_KEY   Anthropic API key\n"
        "  OPENROUTER_API_KEY  OpenRouter API key for namespaced org/model IDs\n"
        "  DSCO_MODEL          Default model override\n"
        "  DSCO_PROFILE        Setup profile name\n"
        "  DSCO_ENV_FILE       Override setup env file path\n"
        "  DSCO_BASELINE_DB    Override sqlite baseline path\n"
        "  DSCO_EXEC           Default executor (claude, codex, auto)\n"
        "  DSCO_BUDGET         Session cost budget in dollars (0=unlimited)\n"
        "  DSCO_DAILY_BUDGET   Daily cost budget in dollars (0=unlimited)\n",
    DSCO_VERSION, prog, DEFAULT_MODEL, prog);
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

int main(int argc, char **argv) {
    (void)atexit(main_atexit_handler);
    init_trace_runtime();
    init_vos_subsystems();  /* §1-§8: Post-LLM Virtual OS layer */

    /* Skip output guard when we're just exec'ing an external CLI —
       it redirects stdout through a pipe which breaks child processes. */
    bool skip_og = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--exec") == 0) {
            skip_og = true; break;
        }
    }
    if (!skip_og) {
        const char *env_exec = getenv("DSCO_EXEC");
        if (env_exec && env_exec[0]) skip_og = true;
    }
    if (!skip_og) (void)output_guard_init();
    TRACE_INFO("main start");

    const char *cli_profile = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            cli_profile = argv[i + 1];
            break;
        }
    }
    if (cli_profile && cli_profile[0]) {
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

    int loaded_env_count = dsco_setup_load_saved_env();
    char bootstrap_msg[512];
    if (!arg_requests_setup && !arg_skip_bootstrap) {
        int bootstrap_state = dsco_setup_bootstrap_from_env(bootstrap_msg, sizeof(bootstrap_msg));
        if (bootstrap_state > 0) {
            fprintf(stderr, "%s\n", bootstrap_msg);
            loaded_env_count += dsco_setup_load_saved_env();
        }
    }

    const char *api_key = NULL;
    const char *model = getenv("DSCO_MODEL");
    if (!model) model = DEFAULT_MODEL;
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
    const char *exec_backend = NULL;  /* "claude", "codex", "auto", "list" */
    char **exec_extra = NULL;         /* passthrough args after -- */
    int exec_nextra = 0;
    bool user_set_model = false;
    bool ui_mode = false;
    int ui_port = 3141;

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
        } else if (strcmp(argv[i], "--ui") == 0) {
            ui_mode = true;
            /* Optional port: --ui 8080 */
            if (i + 1 < argc && argv[i+1][0] != '-') {
                int p = atoi(argv[i+1]);
                if (p > 0 && p <= 65535) { ui_port = p; i++; }
            }
        } else if ((strcmp(argv[i], "--exec") == 0 || strcmp(argv[i], "-e") == 0) && i + 1 < argc) {
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
                if (j > i) strcat(oneshot_prompt, " ");
                strcat(oneshot_prompt, argv[j]);
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
    if (!exec_backend) {
        const char *env_exec = getenv("DSCO_EXEC");
        if (env_exec && env_exec[0])
            exec_backend = env_exec;
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
        execlp("python3", "python3", server_path,
               "--port", port_str,
               "--dir", cwd_str,
               "--model", model,
               "--open",
               (char *)NULL);
        /* Only reached on exec failure */
        perror("failed to launch web UI (is python3 + fastapi installed?)");
        fprintf(stderr, "  install deps: pip install -r web/requirements.txt\n");
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
            int min_tier_needed = tc == TASK_EXPERT ? 4 : tc == TASK_COMPLEX ? 3 :
                                  tc == TASK_MEDIUM ? 2 : 1;

            /* Find the best available native provider that meets the tier+caps */
            const native_provider_t *best = NULL;
            for (int i = 0; NATIVE_PROVIDERS[i].name; i++) {
                const native_provider_t *c = &NATIVE_PROVIDERS[i];
                const char *key = getenv(c->env_key);
                if (!key || !key[0]) continue;
                if (c->tier < min_tier_needed) continue;
                /* Prefer tool-capable for anything above simple */
                if (tc >= TASK_MEDIUM && !(c->caps & CAP_TOOLS)) continue;
                /* Pick highest tier, then prefer thinking capability */
                if (!best || c->tier > best->tier ||
                    (c->tier == best->tier && (c->caps & CAP_THINKING) && !(best->caps & CAP_THINKING)))
                    best = c;
            }

            if (!best) {
                /* Fall back to any available provider */
                for (int i = 0; NATIVE_PROVIDERS[i].name; i++) {
                    const char *key = getenv(NATIVE_PROVIDERS[i].env_key);
                    if (key && key[0]) { best = &NATIVE_PROVIDERS[i]; break; }
                }
            }

            if (!best) {
                fprintf(stderr, "error: no provider with API key available\n");
                free(oneshot_prompt);
                return 1;
            }

            api_key = getenv(best->env_key);
            g_provider_override = best->name;
            if (!user_set_model) model = best->example_model;
            fprintf(stderr, "  \033[2m[smart] task=%s tier=%d → %s (%s)\033[0m\n",
                    task_complexity_name(tc), min_tier_needed,
                    best->name, model);
            goto native_path;
        }

        /* "bench-tools" — test tool calling across providers */
        if (strcmp(exec_backend, "bench-tools") == 0) {
            tools_init();
            fprintf(stderr, "\n  \033[1mTool Calling Benchmark\033[0m\n");
            fprintf(stderr, "  Tests: tool invocation, multi-turn, arg parsing\n\n");
            fprintf(stderr, "  %-12s %-24s %7s  %-7s %-7s %-7s %s\n",
                    "PROVIDER", "MODEL", "TIME", "INVOKE", "ARGS", "MULTI", "NOTES");
            fprintf(stderr, "  %-12s %-24s %7s  %-7s %-7s %-7s %s\n",
                    "────────", "─────", "────", "──────", "────", "─────", "─────");

            for (int i = 0; NATIVE_PROVIDERS[i].name; i++) {
                const native_provider_t *tp = &NATIVE_PROVIDERS[i];
                const char *tkey = getenv(tp->env_key);
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
                conversation_t tconv;
                conv_init(&tconv);

                /* Test 1: Does the model invoke a tool? */
                conv_add_user_text(&tconv, "Read the file README.md and tell me the first line.");

                struct timeval tt0, tt1;
                gettimeofday(&tt0, NULL);

                char *treq = tprov->build_request(tprov, &tconv, &tsess, 1024);
                stream_result_t tsr = {0};
                if (treq) {
                    tsr = tprov->stream(tprov, tkey, treq, NULL, NULL, NULL, NULL);
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
                            conv_add_tool_result(&tconv, blk->tool_id,
                                                 "# dsco-cli\nThin agentic CLI", false);
                            break;
                        }
                    }
                    json_free_response(&tsr.parsed);

                    /* Second turn */
                    char *treq2 = tprov->build_request(tprov, &tconv, &tsess, 1024);
                    stream_result_t tsr2 = {0};
                    if (treq2) {
                        tsr2 = tprov->stream(tprov, tkey, treq2, NULL, NULL, NULL, NULL);
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
                const char *bkey = getenv(np2->env_key);
                if (!bkey || !bkey[0]) {
                    fprintf(stderr, "  %-12s %-28s %7s  %5s  %s\n",
                            np2->name, np2->example_model, "-", "-", "no key");
                    continue;
                }

                /* Build a minimal oneshot request and stream it */
                session_state_t bsess;
                session_state_init(&bsess, np2->example_model);
                provider_t *bprov = provider_create(np2->name);
                conversation_t bconv;
                conv_init(&bconv);
                conv_add_user_text(&bconv, bench_prompt);

                char *breq = bprov->build_request(bprov, &bconv, &bsess, 256);
                struct timeval bt0, bt1;
                gettimeofday(&bt0, NULL);

                /* Capture text into buffer */
                char bench_text[512] = "";
                int bench_text_len = 0;

                (void)bench_text_len;
                stream_result_t bsr = {0};

                if (breq) {
                    bsr = bprov->stream(bprov, bkey, breq,
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

        /* Check if it's a native provider name */
        const native_provider_t *np = native_find(exec_backend);
        if (np) {
            /* Force this provider — resolve its key, set override, fall through */
            const char *pkey = getenv(np->env_key);
            if (!pkey || !pkey[0]) {
                fprintf(stderr, "error: %s requires %s to be set\n",
                        np->name, np->env_key);
                free(oneshot_prompt);
                return 1;
            }
            api_key = pkey;
            g_provider_override = np->name;
            /* If user didn't pick a model, suggest one for this provider */
            if (!user_set_model && np->example_model) {
                model = np->example_model;
                fprintf(stderr, "  %s%s → %s%s\n", "\033[2m", np->name, model, "\033[0m");
            }
            /* Fall through to normal dsco oneshot/interactive path */
            goto native_path;
        }

        /* "auto" — pick first available external CLI */
        const exec_reg_t *ereg = NULL;
        if (strcmp(exec_backend, "auto") == 0) {
            for (int i = 0; EXEC_REGISTRY[i].name; i++) {
                if (exec_bin_available(EXEC_REGISTRY[i].bin)) {
                    ereg = &EXEC_REGISTRY[i];
                    break;
                }
            }
            if (!ereg) {
                fprintf(stderr, "error: no executor found in PATH (install claude or codex)\n");
                free(oneshot_prompt);
                return 1;
            }
            fprintf(stderr, "  auto-selected: %s\n", ereg->name);
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
                fprintf(stderr, "\n  special: auto, smart, list, bench\n");
                free(oneshot_prompt);
                return 1;
            }
        }

        /* exec replaces the process — never returns on success */
        exec_dispatch(ereg, oneshot_prompt,
                      user_set_model ? model : NULL,
                      exec_extra, exec_nextra);
        /* only reached on exec failure */
        free(oneshot_prompt);
        return 1;
    }

native_path:

    /* Resolve API key for the active provider */
    if (!api_key || api_key[0] == '\0') {
        const char *prov = g_provider_override
            ? g_provider_override
            : provider_detect(model, NULL);
        api_key = provider_resolve_api_key(prov);
    }
    if (!api_key || api_key[0] == '\0') {
        const char *prov = g_provider_override
            ? g_provider_override
            : provider_detect(model, NULL);
        fprintf(stderr, "error: API key not set for provider '%s'\n", prov);
        fprintf(stderr, "  use -k KEY or export the provider-specific env var\n");
        free(oneshot_prompt);
        return 1;
    }

    if (!baseline_start(model, oneshot_prompt ? "oneshot" : "interactive")) {
        fprintf(stderr, "warning: baseline disabled (sqlite unavailable)\n");
    }

    if (oneshot_prompt) {
        tools_init();
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
        const char *oneshot_provider_name = g_provider_override
            ? g_provider_override
            : provider_detect(oneshot_session.model, api_key);
        provider_t *oneshot_provider = provider_create(oneshot_provider_name);
        /* Resolve the correct API key for this provider (e.g. OPENROUTER_API_KEY) */
        const char *oneshot_key = provider_resolve_api_key(oneshot_provider_name);
        if (!oneshot_key || !oneshot_key[0]) oneshot_key = api_key;

        int turns = 0;
        bool oneshot_had_error = false;
        while (turns < dsco_max_agent_turns()) {
            turns++;
            md_reset(&s_oneshot_md);

            char *req = oneshot_provider
                ? oneshot_provider->build_request(oneshot_provider, &conv, &oneshot_session, MAX_TOKENS)
                : llm_build_request_ex(&conv, &oneshot_session, MAX_TOKENS);
            if (!req) {
                fprintf(stderr, "error: failed to build request\n");
                baseline_log("error", "request_build_failed", NULL, NULL);
                oneshot_had_error = true;
                break;
            }

            stream_result_t sr = oneshot_provider
                ? oneshot_provider->stream(oneshot_provider, oneshot_key, req,
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
                    conv_add_tool_result(&conv, blk->tool_id, tr, !ok);
                    baseline_log(ok ? "tool_result" : "tool_error",
                                 blk->tool_name ? blk->tool_name : "tool",
                                 tr, NULL);
                    free(tr);
                }
            }

            bool done = !has_tool_use ||
                        (sr.parsed.stop_reason &&
                         strcmp(sr.parsed.stop_reason, "end_turn") == 0);

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
            signal(SIGSEGV, crash_handler);
            signal(SIGBUS,  crash_handler);
            signal(SIGABRT, crash_handler);

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
                while (t2 < dsco_max_agent_turns() && !g_main_interrupted) {
                    t2++;
                    md_reset(&s_oneshot_md);
                    char *req2 = oneshot_provider
                        ? oneshot_provider->build_request(oneshot_provider, &conv, &oneshot_session, MAX_TOKENS)
                        : llm_build_request_ex(&conv, &oneshot_session, MAX_TOKENS);
                    if (!req2) { task_ok = false; break; }

                    stream_result_t sr2 = oneshot_provider
                        ? oneshot_provider->stream(oneshot_provider, oneshot_key, req2,
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
                            conv_add_tool_result(&conv, blk->tool_id, tr, !ok);
                            free(tr);
                        }
                    }

                    bool d2 = !has_tu || (sr2.parsed.stop_reason &&
                              strcmp(sr2.parsed.stop_reason, "end_turn") == 0);
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
    } else {
        agent_run(api_key, model, topology_name, topology_auto, g_provider_override);
    }

    curl_global_cleanup();
    baseline_stop();
    return 0;
}
