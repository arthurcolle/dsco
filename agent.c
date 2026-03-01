#include "agent.h"
#include "llm.h"
#include "tools.h"
#include "config.h"
#include "json_util.h"
#include "tui.h"
#include "baseline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile int g_interrupted = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
}

/* Streaming callbacks */
static bool s_in_text_block = false;

static void on_stream_text(const char *text, void *ctx) {
    (void)ctx;
    if (!s_in_text_block) {
        s_in_text_block = true;
    }
    fputs(text, stdout);
    fflush(stdout);
}

static void on_stream_tool_start(const char *name, const char *id, void *ctx) {
    (void)ctx; (void)id;
    if (s_in_text_block) {
        printf("\n");
        s_in_text_block = false;
    }
    fprintf(stderr, "  %s%s⚡%s %s%s%s\n", TUI_BOLD, TUI_CYAN, TUI_RESET,
            TUI_DIM, name, TUI_RESET);
    baseline_log("tool", name, "tool_use started", NULL);
}

static void print_tool_result(const char *name, bool ok, const char *result) {
    const char *nl = strchr(result, '\n');
    int len = nl ? (int)(nl - result) : (int)strlen(result);
    if (len > 120) len = 120;
    fprintf(stderr, "    %s%s%s %s%.*s%s\n",
            ok ? TUI_GREEN : TUI_RED,
            ok ? "✓" : "✗",
            TUI_RESET,
            TUI_DIM, len, result, TUI_RESET);
    baseline_log(ok ? "tool_result" : "tool_error", name, result, NULL);
}

static void print_usage(usage_t *u) {
    fprintf(stderr, "%s  [in:%d out:%d", TUI_DIM, u->input_tokens, u->output_tokens);
    if (u->cache_read_input_tokens > 0)
        fprintf(stderr, " cache-read:%d", u->cache_read_input_tokens);
    if (u->cache_creation_input_tokens > 0)
        fprintf(stderr, " cache-write:%d", u->cache_creation_input_tokens);
    fprintf(stderr, "]%s\n", TUI_RESET);
}

void agent_run(const char *api_key, const char *model) {
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    tools_init();

    conversation_t conv;
    conv_init(&conv);

    char input_buf[MAX_INPUT_LINE];

    int tool_count;
    tools_get_all(&tool_count);

    /* ── Welcome banner ─────────────────────────────────────────────────── */
    tui_welcome(model, tool_count, DSCO_VERSION);

    fprintf(stderr, "  %stype your request, or 'quit' to exit%s\n\n", TUI_DIM, TUI_RESET);

    while (1) {
        g_interrupted = 0;
        fprintf(stderr, "%s%s❯%s ", TUI_BOLD, TUI_BGREEN, TUI_RESET);
        fflush(stderr);

        if (!fgets(input_buf, sizeof(input_buf), stdin)) break;

        size_t len = strlen(input_buf);
        while (len > 0 && (input_buf[len-1] == '\n' || input_buf[len-1] == '\r'))
            input_buf[--len] = '\0';

        if (len == 0) continue;
        if (strcmp(input_buf, "quit") == 0 || strcmp(input_buf, "exit") == 0) break;

        if (strcmp(input_buf, "/clear") == 0) {
            conv_free(&conv);
            conv_init(&conv);
            tui_success("conversation cleared");
            baseline_log("command", "/clear", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/tools") == 0) {
            int count;
            const tool_def_t *tools = tools_get_all(&count);
            fprintf(stderr, "\n");
            tui_header("Tools", TUI_BCYAN);

            /* Group by category */
            const char *last_prefix = "";
            for (int i = 0; i < count; i++) {
                /* Check for category breaks based on name patterns */
                const char *name = tools[i].name;
                const char *category = "misc";
                if (strstr(name, "file") || strstr(name, "read") || strstr(name, "write") ||
                    strstr(name, "edit") || strstr(name, "append") || strstr(name, "mkdir") ||
                    strstr(name, "tree") || strstr(name, "wc") || strstr(name, "head") ||
                    strstr(name, "tail") || strstr(name, "symlink") || strstr(name, "page") ||
                    strcmp(name, "list_directory") == 0 || strcmp(name, "find_files") == 0 ||
                    strcmp(name, "grep_files") == 0 || strcmp(name, "chmod") == 0 ||
                    strcmp(name, "move_file") == 0 || strcmp(name, "copy_file") == 0 ||
                    strcmp(name, "delete_file") == 0 || strcmp(name, "file_info") == 0)
                    category = "file";
                else if (strstr(name, "git"))   category = "git";
                else if (strstr(name, "agent") || strstr(name, "swarm") || strstr(name, "spawn"))
                    category = "swarm";
                else if (strstr(name, "self_") || strstr(name, "inspect") ||
                         strstr(name, "call_graph") || strstr(name, "dependency"))
                    category = "ast";
                else if (strstr(name, "http") || strstr(name, "curl") || strstr(name, "dns") ||
                         strstr(name, "ping") || strstr(name, "port") || strstr(name, "net") ||
                         strstr(name, "cert") || strstr(name, "whois") || strstr(name, "download") ||
                         strstr(name, "upload") || strstr(name, "websocket") || strstr(name, "traceroute"))
                    category = "network";
                else if (strstr(name, "docker")) category = "docker";
                else if (strstr(name, "ssh") || strstr(name, "scp")) category = "remote";
                else if (strstr(name, "compile") || strstr(name, "run_")) category = "exec";

                if (strcmp(category, last_prefix) != 0) {
                    last_prefix = category;
                    const char *cat_color = TUI_DIM;
                    if (strcmp(category, "swarm") == 0) cat_color = TUI_BYELLOW;
                    else if (strcmp(category, "ast") == 0) cat_color = TUI_BMAGENTA;
                    fprintf(stderr, "  %s%s%s\n", cat_color, category, TUI_RESET);
                }
                fprintf(stderr, "    %s%-22s%s %s%s%s\n",
                        TUI_CYAN, tools[i].name, TUI_RESET,
                        TUI_DIM, tools[i].description, TUI_RESET);
            }
            fprintf(stderr, "\n  %s%d tools total%s\n\n", TUI_DIM, count, TUI_RESET);
            baseline_log("command", "/tools", NULL, NULL);
            continue;
        }
        if (strcmp(input_buf, "/help") == 0) {
            fprintf(stderr, "\n");
            tui_header("Commands", TUI_BCYAN);
            fprintf(stderr, "  %s/clear%s   reset conversation\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/tools%s   list all tools\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %s/help%s    show this help\n", TUI_CYAN, TUI_RESET);
            fprintf(stderr, "  %squit%s     exit dsco\n\n", TUI_CYAN, TUI_RESET);
            tui_header("Swarm", TUI_BYELLOW);
            fprintf(stderr, "  %sAsk dsco to spawn sub-agents for parallel work:%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s\"spawn 3 agents to build a REST API, frontend, and tests\"%s\n\n", TUI_DIM, TUI_RESET);
            tui_header("AST Introspection", TUI_BMAGENTA);
            fprintf(stderr, "  %sAsk dsco to analyze its own code:%s\n", TUI_DIM, TUI_RESET);
            fprintf(stderr, "  %s\"inspect your own source code and find the most complex functions\"%s\n\n", TUI_DIM, TUI_RESET);
            baseline_log("command", "/help", NULL, NULL);
            continue;
        }

        conv_add_user_text(&conv, input_buf);
        baseline_log("user", "prompt", input_buf, NULL);

        int turns = 0;
        int total_input = 0, total_output = 0, total_cache_read = 0;

        while (turns < MAX_AGENT_TURNS && !g_interrupted) {
            turns++;
            s_in_text_block = false;

            char *req = llm_build_request(&conv, model, MAX_TOKENS);
            if (!req) {
                tui_error("failed to build request");
                baseline_log("error", "request_build_failed", NULL, NULL);
                break;
            }

            stream_result_t sr = llm_stream(api_key, req,
                                             on_stream_text,
                                             on_stream_tool_start,
                                             NULL);
            free(req);

            if (!sr.ok) {
                char err[128];
                snprintf(err, sizeof(err), "stream failed (HTTP %d)", sr.http_status);
                tui_error(err);
                baseline_log("error", "stream_failed", err, NULL);
                json_free_response(&sr.parsed);
                break;
            }

            /* Finish text line if we were streaming text */
            if (s_in_text_block) {
                printf("\n");
                s_in_text_block = false;
            }

            total_input += sr.usage.input_tokens;
            total_output += sr.usage.output_tokens;
            total_cache_read += sr.usage.cache_read_input_tokens;
            print_usage(&sr.usage);

            /* Add full response to conversation history */
            conv_add_assistant_raw(&conv, &sr.parsed);

            /* Execute tools */
            bool has_tool_use = false;
            for (int i = 0; i < sr.parsed.count; i++) {
                content_block_t *blk = &sr.parsed.blocks[i];

                if (blk->type && strcmp(blk->type, "tool_use") == 0) {
                    has_tool_use = true;

                    char *tool_result = malloc(MAX_TOOL_RESULT);
                    tool_result[0] = '\0';
                    bool ok = tools_execute(blk->tool_name, blk->tool_input,
                                            tool_result, MAX_TOOL_RESULT);

                    print_tool_result(blk->tool_name, ok, tool_result);
                    conv_add_tool_result(&conv, blk->tool_id, tool_result, !ok);
                    free(tool_result);
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

        if (g_interrupted) {
            fprintf(stderr, "\n");
            tui_warning("interrupted");
        }
        if (turns >= MAX_AGENT_TURNS) {
            char msg[64];
            snprintf(msg, sizeof(msg), "max turns reached (%d)", MAX_AGENT_TURNS);
            tui_warning(msg);
        }
        if (turns > 1) {
            fprintf(stderr, "%s  [%d turns | total in:%d out:%d cache-read:%d]%s\n",
                    TUI_DIM, turns, total_input, total_output, total_cache_read, TUI_RESET);
        }
        printf("\n");
    }

    conv_free(&conv);
    fprintf(stderr, "%s  goodbye%s\n", TUI_DIM, TUI_RESET);
}
