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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>

static md_renderer_t s_oneshot_md;

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
    TRACE_SHUTDOWN();
    ipc_shutdown();
}

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
        "  DSCO_BASELINE_DB    Override sqlite baseline path\n",
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
    (void)output_guard_init();
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
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            model = argv[++i];
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
        } else {
            size_t total = 0;
            for (int j = i; j < argc; j++) total += strlen(argv[j]) + 1;
            oneshot_prompt = safe_malloc(total + 1);
            oneshot_prompt[0] = '\0';
            for (int j = i; j < argc; j++) {
                if (j > i) strcat(oneshot_prompt, " ");
                strcat(oneshot_prompt, argv[j]);
            }
            break;
        }
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

    if (!api_key || api_key[0] == '\0') {
        const char *provider = provider_detect(model, NULL);
        api_key = provider_resolve_api_key(provider);
    }

    if (!api_key || api_key[0] == '\0') {
        const char *provider = provider_detect(model, NULL);
        fprintf(stderr, "error: API key not set for provider '%s'\n", provider);
        fprintf(stderr, "  use -k KEY or export the provider-specific env var\n");
        free(oneshot_prompt);
        return 1;
    }

    if (!baseline_start(model, oneshot_prompt ? "oneshot" : "interactive")) {
        fprintf(stderr, "warning: baseline disabled (sqlite unavailable)\n");
    }
    if (loaded_env_count > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "loaded %d key(s) from setup env", loaded_env_count);
        baseline_log("setup", "keys_loaded", msg, NULL);
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (oneshot_prompt) {
        tools_init();
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
        const char *oneshot_provider_name = provider_detect(oneshot_session.model, api_key);
        provider_t *oneshot_provider = provider_create(oneshot_provider_name);

        int turns = 0;
        while (turns < dsco_max_agent_turns()) {
            turns++;
            md_reset(&s_oneshot_md);

            char *req = oneshot_provider
                ? oneshot_provider->build_request(oneshot_provider, &conv, &oneshot_session, MAX_TOKENS)
                : llm_build_request_ex(&conv, &oneshot_session, MAX_TOKENS);
            if (!req) {
                fprintf(stderr, "error: failed to build request\n");
                baseline_log("error", "request_build_failed", NULL, NULL);
                break;
            }

            stream_result_t sr = oneshot_provider
                ? oneshot_provider->stream(oneshot_provider, api_key, req,
                                           oneshot_text_cb, oneshot_tool_cb,
                                           NULL, NULL)
                : llm_stream(api_key, req,
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
                        ? oneshot_provider->stream(oneshot_provider, api_key, req2,
                                                   oneshot_text_cb, oneshot_tool_cb,
                                                   NULL, NULL)
                        : llm_stream(api_key, req2,
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
    } else {
        agent_run(api_key, model, topology_name, topology_auto);
    }

    curl_global_cleanup();
    baseline_stop();
    return 0;
}
