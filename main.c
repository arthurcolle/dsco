#include "agent.h"
#include "config.h"
#include "llm.h"
#include "tools.h"
#include "json_util.h"
#include "baseline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "dsco v%s — thin agentic CLI (streaming + prompt caching)\n"
        "\n"
        "Usage: %s [options] [prompt]\n"
        "\n"
        "Options:\n"
        "  -m MODEL    Model name (default: %s)\n"
        "  -k KEY      API key (default: $ANTHROPIC_API_KEY)\n"
        "  --timeline-server        Run local timeline web server\n"
        "  --timeline-port PORT     Timeline webserver port (default: 8421)\n"
        "  --timeline-instance ID   Filter timeline to one instance ID\n"
        "  -h          Show this help\n"
        "\n"
        "Interactive mode: run without a prompt\n"
        "One-shot mode:    %s 'write a hello world in C and compile it'\n"
        "\n"
        "Environment:\n"
        "  ANTHROPIC_API_KEY   Your Anthropic API key\n"
        "  DSCO_MODEL          Default model override\n"
        "  DSCO_BASELINE_DB    Override sqlite baseline path\n",
    DSCO_VERSION, prog, DEFAULT_MODEL, prog);
}

/* One-shot mode: simple print callbacks */
static void oneshot_text_cb(const char *text, void *ctx) {
    (void)ctx;
    fputs(text, stdout);
    fflush(stdout);
}

static void oneshot_tool_cb(const char *name, const char *id, void *ctx) {
    (void)id; (void)ctx;
    fprintf(stderr, "\033[2m\033[36m► %s\033[0m\n", name);
    baseline_log("tool", name, "tool_use started", NULL);
}

int main(int argc, char **argv) {
    const char *api_key = getenv("ANTHROPIC_API_KEY");
    const char *model = getenv("DSCO_MODEL");
    if (!model) model = DEFAULT_MODEL;
    char *oneshot_prompt = NULL;
    bool timeline_server_mode = false;
    int timeline_port = 8421;
    const char *timeline_instance_filter = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            api_key = argv[++i];
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
        } else {
            size_t total = 0;
            for (int j = i; j < argc; j++) total += strlen(argv[j]) + 1;
            oneshot_prompt = malloc(total + 1);
            oneshot_prompt[0] = '\0';
            for (int j = i; j < argc; j++) {
                if (j > i) strcat(oneshot_prompt, " ");
                strcat(oneshot_prompt, argv[j]);
            }
            break;
        }
    }

    if (timeline_server_mode) {
        if (!baseline_start(model, "timeline-server")) {
            fprintf(stderr, "error: failed to start baseline sqlite storage\n");
            return 1;
        }
        int rc = baseline_serve_http(timeline_port, timeline_instance_filter);
        baseline_stop();
        return rc == 0 ? 0 : 1;
    }

    if (!api_key || api_key[0] == '\0') {
        fprintf(stderr, "error: ANTHROPIC_API_KEY not set\n");
        fprintf(stderr, "  export ANTHROPIC_API_KEY=sk-ant-...\n");
        free(oneshot_prompt);
        return 1;
    }

    if (!baseline_start(model, oneshot_prompt ? "oneshot" : "interactive")) {
        fprintf(stderr, "warning: baseline disabled (sqlite unavailable)\n");
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (oneshot_prompt) {
        tools_init();
        conversation_t conv;
        conv_init(&conv);
        conv_add_user_text(&conv, oneshot_prompt);
        baseline_log("user", "oneshot_prompt", oneshot_prompt, NULL);

        int turns = 0;
        while (turns < MAX_AGENT_TURNS) {
            turns++;

            char *req = llm_build_request(&conv, model, MAX_TOKENS);
            if (!req) {
                fprintf(stderr, "error: failed to build request\n");
                baseline_log("error", "request_build_failed", NULL, NULL);
                break;
            }

            stream_result_t sr = llm_stream(api_key, req,
                                             oneshot_text_cb,
                                             oneshot_tool_cb,
                                             NULL);
            free(req);

            if (!sr.ok) {
                fprintf(stderr, "error: stream failed (HTTP %d)\n", sr.http_status);
                char err[64];
                snprintf(err, sizeof(err), "HTTP %d", sr.http_status);
                baseline_log("error", "stream_failed", err, NULL);
                json_free_response(&sr.parsed);
                break;
            }

            /* Newline after streamed text */
            printf("\n");

            conv_add_assistant_raw(&conv, &sr.parsed);

            bool has_tool_use = false;
            for (int i = 0; i < sr.parsed.count; i++) {
                content_block_t *blk = &sr.parsed.blocks[i];
                if (blk->type && strcmp(blk->type, "tool_use") == 0) {
                    has_tool_use = true;
                    char *tr = malloc(MAX_TOOL_RESULT);
                    tr[0] = '\0';
                    bool ok = tools_execute(blk->tool_name, blk->tool_input,
                                            tr, MAX_TOOL_RESULT);
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

        conv_free(&conv);
        free(oneshot_prompt);
    } else {
        agent_run(api_key, model);
    }

    curl_global_cleanup();
    baseline_stop();
    return 0;
}
