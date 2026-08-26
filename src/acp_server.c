/* acp_server.c — dsco as an Agent Client Protocol agent (stdio JSON-RPC 2.0).
 *
 * The ACP surface is deliberately small and bounded:
 *   initialize, session/new, session/prompt, session/cancel.
 *
 * A prompt is executed by a fresh, governed headless dsco child. The adapter
 * owns the native router state and chooses the model for the next turn. This
 * keeps Buzz as the transport/harness while DSCO retains model-routing and
 * capability-gate authority.
 */

#include "acp_server.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "router.h"

#define ACP_LINE_MAX (4 * 1024 * 1024)
#define ACP_TEXT_MAX (1024 * 1024)
#define ACP_SESSION_MAX 64
#define ACP_MODEL_MAX 128
#define ACP_OUTPUT_MAX (1024 * 1024)

/* ACP can carry large prompts, but the process stack must stay small: Buzz
 * launches us under a harness with an implementation-defined stack limit. */
static char g_line[ACP_LINE_MAX];
static char g_params[ACP_TEXT_MAX + 4];
static char g_prompt[ACP_TEXT_MAX + 4];
static char g_output[ACP_OUTPUT_MAX];
static char g_json_scratch[ACP_TEXT_MAX + 4];
static char g_prompt_scratch[ACP_TEXT_MAX + 4];
static char g_child_prompt[ACP_TEXT_MAX + 9000];

typedef struct {
    bool used;
    char id[64];
    char cwd[1024];
    char system_prompt[8192];
    char model[ACP_MODEL_MAX];
    int turns;
} acp_session_t;

static router_t g_acp_router;
static acp_session_t g_sessions[ACP_SESSION_MAX];
static volatile sig_atomic_t g_cancelled = 0;
static pid_t g_child_pid = -1;
static unsigned long g_session_seq = 0;
static const char *g_dsco_bin = "dsco";

static void json_escape(FILE *out, const char *text) {
    if (!text) return;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        switch (*p) {
            case '"': fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (*p < 0x20) fprintf(out, "\\u%04x", (unsigned)*p);
                else fputc(*p, out);
        }
    }
}

static void rpc_result_prefix(const char *id) {
    fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":", id && id[0] ? id : "null");
}

static void rpc_error(const char *id, int code, const char *message) {
    fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"",
            id && id[0] ? id : "null", code);
    json_escape(stdout, message ? message : "error");
    fputs("\"}}\n", stdout);
    fflush(stdout);
}

static void rpc_update_text(const char *session_id, const char *text) {
    fputs("{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"", stdout);
    json_escape(stdout, session_id);
    fputs("\",\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"content\":{\"type\":\"text\",\"text\":\"", stdout);
    json_escape(stdout, text ? text : "");
    fputs("\"}}}}\n", stdout);
    fflush(stdout);
}

static bool json_raw_value(const char *json, const char *key, char *out, size_t cap) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return false;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    const char *start = p;
    bool quoted = *p == '"';
    bool escaped = false;
    int depth = 0;
    if (!quoted && (*p == '{' || *p == '[')) {
        char open = *p, close = open == '{' ? '}' : ']';
        for (; *p; ++p) {
            if (*p == '"' && !escaped) quoted = !quoted;
            if (!quoted) {
                if (*p == open) depth++;
                else if (*p == close && --depth == 0) { ++p; break; }
            }
            escaped = (*p == '\\' && !escaped);
            if (*p != '\\') escaped = false;
        }
    } else if (quoted) {
        ++p;
        for (; *p; ++p) {
            if (*p == '"' && !escaped) { ++p; break; }
            escaped = (*p == '\\' && !escaped);
            if (*p != '\\') escaped = false;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != '\n' && !isspace((unsigned char)*p)) ++p;
    }
    size_t n = (size_t)(p - start);
    if (n == 0 || n >= cap) return false;
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

static bool json_string_value(const char *json, const char *key, char *out, size_t cap) {
    if (!json_raw_value(json, key, g_json_scratch, sizeof(g_json_scratch))) return false;
    if (g_json_scratch[0] != '"') return false;
    size_t oi = 0;
    for (size_t i = 1; g_json_scratch[i] && g_json_scratch[i] != '"' && oi + 1 < cap; ++i) {
        if (g_json_scratch[i] == '\\' && g_json_scratch[i + 1]) {
            ++i;
            switch (g_json_scratch[i]) {
                case 'n': out[oi++] = '\n'; break;
                case 'r': out[oi++] = '\r'; break;
                case 't': out[oi++] = '\t'; break;
                default: out[oi++] = g_json_scratch[i]; break;
            }
        } else out[oi++] = g_json_scratch[i];
    }
    out[oi] = '\0';
    return true;
}

/* ACP prompt is an array of text blocks. Extract all text fields inside the
 * prompt raw payload and concatenate them with newlines. This is deliberately
 * constrained rather than a general JSON parser. */
static bool prompt_text_from_params(const char *params, char *out, size_t cap) {
    if (!json_raw_value(params, "prompt", g_prompt_scratch, sizeof(g_prompt_scratch))) return false;
    out[0] = '\0';
    const char *p = g_prompt_scratch;
    bool got = false;
    while ((p = strstr(p, "\"text\"")) != NULL) {
        if (!json_string_value(p, "text", g_json_scratch, sizeof(g_json_scratch))) { p += 6; continue; }
        size_t used = strlen(out), n = strlen(g_json_scratch);
        if (used + n + (got ? 1 : 0) + 1 >= cap) return false;
        if (got) out[used++] = '\n';
        memcpy(out + used, g_json_scratch, n + 1);
        got = true;
        p += 6;
    }
    return got;
}

static acp_session_t *find_session(const char *id) {
    for (int i = 0; i < ACP_SESSION_MAX; ++i)
        if (g_sessions[i].used && strcmp(g_sessions[i].id, id) == 0) return &g_sessions[i];
    return NULL;
}

static acp_session_t *create_session(void) {
    for (int i = 0; i < ACP_SESSION_MAX; ++i) {
        if (!g_sessions[i].used) {
            acp_session_t *s = &g_sessions[i];
            memset(s, 0, sizeof(*s));
            s->used = true;
            snprintf(s->id, sizeof(s->id), "dsco-%ld-%lu", (long)getpid(), ++g_session_seq);
            const char *initial = getenv("DSCO_ACP_MODEL");
            snprintf(s->model, sizeof(s->model), "%s", initial && initial[0] ? initial : "claude-sonnet-5");
            return s;
        }
    }
    return NULL;
}

static const char *model_for_prompt(acp_session_t *session, const char *prompt, router_decision_t *decision) {
    int ctx_pct = session->turns > 20 ? 30 : 5;
    task_complexity_t complexity = router_classify_task(prompt, session->turns, 0, ctx_pct);
    *decision = router_decide(&g_acp_router, session->model, complexity,
                              g_acp_router.session_cost_usd, 0.0,
                              g_acp_router.consecutive_failures);
    if (decision->should_switch && decision->model_id[0])
        snprintf(session->model, sizeof(session->model), "%s", decision->model_id);
    return session->model;
}

static bool append_text(char *out, size_t cap, const char *text) {
    size_t used = strlen(out), n = text ? strlen(text) : 0;
    if (used + n + 1 >= cap) return false;
    memcpy(out + used, text, n + 1);
    return true;
}

static bool run_dsco_prompt(const acp_session_t *session, const char *prompt,
                            char *out, size_t out_cap, double *latency_ms) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return false; }
    if (pid == 0) {
        const char *tier = getenv("DSCO_ACP_TRUST_TIER");
        const char *approval = getenv("DSCO_ACP_APPROVAL_MODE");
        const char *provider = getenv("DSCO_ACP_PROVIDER");
        const char *effort = getenv("DSCO_ACP_EFFORT");
        char sysarg[9000];
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (session->system_prompt[0]) {
            snprintf(sysarg, sizeof(sysarg), "System context from ACP harness:\n%s\n\n", session->system_prompt);
            snprintf(g_child_prompt, sizeof(g_child_prompt), "%s%s", sysarg, prompt);
        } else snprintf(g_child_prompt, sizeof(g_child_prompt), "%s", prompt);
        setenv("DSCO_ROUTER_POLICY", router_policy_name(g_acp_router.policy), 1);
        if (!getenv("DSCO_ROUTER_MAX_ESCALATIONS")) setenv("DSCO_ROUTER_MAX_ESCALATIONS", "2", 1);
        /* Do not force a provider by default: the selected model is the
         * router's output, so DSCO must retain its normal provider detection
         * and cross-provider failover. DSCO_ACP_PROVIDER is an explicit
         * operator pin (for example, "anthropic"). */
        char *child_argv[20];
        int ai = 0;
        child_argv[ai++] = (char *)g_dsco_bin;
        child_argv[ai++] = "--profile";
        child_argv[ai++] = "worker";
        child_argv[ai++] = "--model";
        child_argv[ai++] = (char *)session->model;
        if (provider && provider[0]) {
            child_argv[ai++] = "--provider";
            child_argv[ai++] = (char *)provider;
        }
        child_argv[ai++] = "--trust-tier";
        child_argv[ai++] = (char *)(tier && tier[0] ? tier : "trusted");
        child_argv[ai++] = "--approval-mode";
        child_argv[ai++] = (char *)(approval && approval[0] ? approval : "never");
        child_argv[ai++] = "--effort";
        child_argv[ai++] = (char *)(effort && effort[0] ? effort : "high");
        child_argv[ai++] = "--prompt";
        child_argv[ai++] = g_child_prompt;
        child_argv[ai] = NULL;
        execvp(g_dsco_bin, child_argv);
        _exit(127);
    }
    close(pipefd[1]);
    g_child_pid = pid;
    out[0] = '\0';
    char buf[8192];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        if (!append_text(out, out_cap, buf)) break;
    }
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    g_child_pid = -1;
    *latency_ms = 0.0; /* Native child does not yet emit a stable timing envelope. */
    return !g_cancelled && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void on_signal(int signo) {
    (void)signo;
    g_cancelled = 1;
    if (g_child_pid > 0) kill(g_child_pid, SIGTERM);
}

static void handle_initialize(const char *id) {
    rpc_result_prefix(id);
    fputs("{\"protocolVersion\":2,\"agentCapabilities\":{\"loadSession\":false,\"mcpCapabilities\":{\"http\":false,\"sse\":false,\"acp\":false},\"promptCapabilities\":{\"embeddedContext\":true,\"image\":false},\"sessionCapabilities\":{\"close\":{},\"delete\":{}}},\"agentInfo\":{\"name\":\"dsco-acp\",\"title\":\"DSCO Routed Runtime\",\"version\":\"1.0\"}}}\n", stdout);
    fflush(stdout);
}

static void handle_session_new(const char *id, const char *params) {
    acp_session_t *s = create_session();
    if (!s) { rpc_error(id, -32000, "session capacity exhausted"); return; }
    (void)json_string_value(params, "cwd", s->cwd, sizeof(s->cwd));
    (void)json_string_value(params, "systemPrompt", s->system_prompt, sizeof(s->system_prompt));
    rpc_result_prefix(id);
    fputs("{\"sessionId\":\"", stdout); json_escape(stdout, s->id);
    fputs("\",\"configOptions\":[{\"id\":\"model\",\"name\":\"DSCO routed model\",\"category\":\"model\",\"currentValue\":\"", stdout);
    json_escape(stdout, s->model);
    fputs("\"}]}}\n", stdout);
    fflush(stdout);
}

static void handle_session_prompt(const char *id, const char *params) {
    char session_id[64];
    if (!json_string_value(params, "sessionId", session_id, sizeof(session_id))) {
        rpc_error(id, -32602, "missing sessionId"); return;
    }
    acp_session_t *s = find_session(session_id);
    if (!s) { rpc_error(id, -32602, "unknown sessionId"); return; }
    if (!prompt_text_from_params(params, g_prompt, sizeof(g_prompt))) {
        rpc_error(id, -32602, "prompt must contain text content"); return;
    }
    g_cancelled = 0;
    router_decision_t decision;
    const char *model = model_for_prompt(s, g_prompt, &decision);
    char routing_note[640];
    snprintf(routing_note, sizeof(routing_note), "[DSCO router] model=%s policy=%s complexity=%s reason=%s confidence=%.0f%%\n",
             model, router_policy_name(g_acp_router.policy), task_complexity_name(decision.complexity),
             switch_reason_name(decision.reason), (double)decision.confidence * 100.0);
    rpc_update_text(s->id, routing_note);
    double latency_ms = 0.0;
    bool ok = run_dsco_prompt(s, g_prompt, g_output, sizeof(g_output), &latency_ms);
    if (g_output[0]) rpc_update_text(s->id, g_output);
    if (!ok && !g_cancelled && !g_output[0])
        rpc_update_text(s->id, "DSCO execution failed without a textual response. Inspect the DSCO runtime log.");
    router_record_turn(&g_acp_router, model, 0, 0, latency_ms, 0.0, 0.0, ok);
    g_acp_router.consecutive_failures = ok ? 0 : g_acp_router.consecutive_failures + 1;
    s->turns++;
    rpc_result_prefix(id);
    fputs(g_cancelled ? "{\"stopReason\":\"cancelled\"}}\n" : "{\"stopReason\":\"end_turn\"}}\n", stdout);
    fflush(stdout);
}

static void handle_session_cancel(const char *params) {
    (void)params;
    g_cancelled = 1;
    if (g_child_pid > 0) kill(g_child_pid, SIGTERM);
}

int acp_server_run(const char *dsco_argv0) {
    const char *override_bin = getenv("DSCO_ACP_DSCO_BIN");
    g_dsco_bin = override_bin && override_bin[0]
                     ? override_bin
                     : (dsco_argv0 && dsco_argv0[0] ? dsco_argv0 : "dsco");
    router_init(&g_acp_router, router_policy_parse(getenv("DSCO_ROUTER_POLICY")));
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (fgets(g_line, sizeof(g_line), stdin)) {
        size_t n = strlen(g_line);
        if (n && g_line[n - 1] == '\n') g_line[n - 1] = '\0';
        char method[128] = {0}, id[256] = {0};
        if (!json_string_value(g_line, "method", method, sizeof(method))) continue;
        (void)json_raw_value(g_line, "id", id, sizeof(id));
        (void)json_raw_value(g_line, "params", g_params, sizeof(g_params));
        if (strcmp(method, "initialize") == 0) handle_initialize(id);
        else if (strcmp(method, "session/new") == 0) handle_session_new(id, g_params);
        else if (strcmp(method, "session/prompt") == 0) handle_session_prompt(id, g_params);
        else if (strcmp(method, "session/cancel") == 0) handle_session_cancel(g_params);
        else if (strcmp(method, "session/close") == 0 || strcmp(method, "session/delete") == 0) {
            char sid[64];
            if (json_string_value(g_params, "sessionId", sid, sizeof(sid))) {
                acp_session_t *s = find_session(sid); if (s) s->used = false;
            }
            if (id[0]) { rpc_result_prefix(id); fputs("{}}\n", stdout); fflush(stdout); }
        } else if (strcmp(method, "ping") == 0) {
            rpc_result_prefix(id); fputs("{}}\n", stdout); fflush(stdout);
        } else if (id[0]) rpc_error(id, -32601, "method not found");
    }
    router_destroy(&g_acp_router);
    return 0;
}
