/* eval_harness.c — `dsco eval run <set> [--json]` (Wave B #11)
 *
 * First user-facing eval harness. Loads JSONL eval sets from
 *   ${DSCO_EVAL_SET_DIR:-evals}/<set>.jsonl   (or a verbatim *.jsonl path),
 * executes each case with a bounded, deterministic verifier, and reports
 * pass/fail + latency_ms per case as a human table or JSON object:
 *   {"set":"<name>","cases":[{"id":..,"pass":true,"latency_ms":1.2}],
 *    "passed":n,"total":n}
 *
 * v1 verifiers (provider-free, deterministic):
 *   tool_dispatch_smoke  — in-process dispatch of a builtin tool through
 *                          tools_execute_for_tier() (the governance gate),
 *                          expect a substring in the tool result.
 *   gate_fs_write_denied — with DSCO_ALLOW_WRITE=0, expect dsco_capability_gate()
 *                          to return DENY, tools_execute_for_tier() to fail,
 *                          and the target file to remain absent.
 *   mcp_tools_list       — fork/exec the current dsco binary as
 *                          `mcp serve`, write initialize /
 *                          notifications/initialized / tools/list JSON-RPC
 *                          lines to its stdin, read framed responses from
 *                          stdout, and expect a non-empty result.tools
 *                          array (optionally containing expected names).
 *
 * Contract per .workspace/harness-parity/swarm-20260827/03-eval-harness-design.md:
 *  - the whole set is validated before the first case executes;
 *  - subprocesses use fork/exec with argv arrays, never system();
 *  - every executed tool call routes through tools_execute_for_tier();
 *  - exit 0 = all pass, 1 = a case failed/errored, 2 = usage or malformed set.
 *
 * This module is deliberately separate from src/eval.c (the expression
 * evaluator used by the builtin `eval` tool).
 */

#include "eval_harness.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "capability.h"
#include "json_util.h"
#include "tools.h"

/* ── limits ──────────────────────────────────────────────────────────── */

#define EVAL_MAX_CASES 256
#define EVAL_LINE_MAX (64 * 1024)
#define EVAL_JSON_MAX (48 * 1024)
#define EVAL_RESULT_MAX (64 * 1024)
#define EVAL_OUTBUF_MAX (4 * 1024 * 1024)
#define EVAL_DEFAULT_TIMEOUT_MS 10000
#define EVAL_ID_MAX 128
#define EVAL_VERIFIER_MAX 64
#define EVAL_MSG_MAX 256

/* ── case model ──────────────────────────────────────────────────────── */

typedef enum { EV_STATUS_PASS, EV_STATUS_FAIL, EV_STATUS_ERROR } eval_status_t;

typedef struct {
    char id[EVAL_ID_MAX];
    char verifier[EVAL_VERIFIER_MAX];
    char input_raw[EVAL_JSON_MAX];
    char expected_raw[EVAL_JSON_MAX];
} eval_case_def_t;

typedef struct {
    char id[EVAL_ID_MAX];
    char verifier[EVAL_VERIFIER_MAX];
    eval_status_t status;
    double latency_ms;
    char message[EVAL_MSG_MAX];
} eval_result_t;

/* ── small helpers ───────────────────────────────────────────────────── */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Fast-path init for direct tool execution. Idempotent in practice; called
 * lazily once before the first in-process case. */
static void eval_ensure_tools_init(void) {
    static bool initialized = false;
    if (!initialized) {
        tools_init_local_only();
        initialized = true;
    }
}

/* Resolve the JSONL path for a set name (worker contract: evals/<set>.jsonl). */
static int eval_resolve_set_path(const char *set, char *out, size_t out_len) {
    if (!set || !set[0])
        return -1;
    if (strstr(set, ".jsonl")) { /* explicit path: used verbatim */
        snprintf(out, out_len, "%s", set);
        return access(out, R_OK) == 0 ? 0 : -1;
    }
    const char *dir = getenv("DSCO_EVAL_SET_DIR");
    if (!dir || !dir[0])
        dir = "evals";
    snprintf(out, out_len, "%s/%s.jsonl", dir, set);
    if (access(out, R_OK) == 0)
        return 0;
    /* fall back to repo-root-relative evals/ when cwd differs */
    snprintf(out, out_len, "evals/%s.jsonl", set);
    return access(out, R_OK) == 0 ? 0 : -1;
}

/* Resolve the current executable for child spawns (no shell involved). */
static void eval_self_exe(char *out, size_t out_len, char *argv0) {
#ifdef __APPLE__
    {
        char buf[4096];
        uint32_t sz = sizeof(buf);
        if (_NSGetExecutablePath(buf, &sz) == 0 && access(buf, X_OK) == 0) {
            snprintf(out, out_len, "%s", buf);
            return;
        }
    }
#endif
    if (argv0 && argv0[0] && access(argv0, X_OK) == 0) {
        snprintf(out, out_len, "%s", argv0);
        return;
    }
    snprintf(out, out_len, "dsco"); /* last resort: execvp PATH lookup */
}

/* ── verifier: tool_dispatch_smoke ─────────────────────────────────────
 * input:   {"tool":"eval","arguments":{"expression":"2+2"},"tier":"trusted"}
 * expected:{"result_contains":"4"}
 * Dispatches the builtin through tools_execute_for_tier() so the governance
 * gate is exercised, and requires the result to contain a substring.
 */
static void eval_verify_tool_dispatch(const eval_case_def_t *c, eval_result_t *r) {
    char buf[EVAL_RESULT_MAX];

    char *args = json_get_raw(c->input_raw, "arguments");
    if (!args || !json_is_valid_container(args)) {
        free(args);
        args = strdup("{}");
        if (!args) {
            r->status = EV_STATUS_ERROR;
            snprintf(r->message, sizeof(r->message), "out of memory");
            return;
        }
    }
    char *tool = json_get_str(c->input_raw, "tool");
    if (!tool || !tool[0])
        tool = strdup("eval");
    char *tier = json_get_str(c->input_raw, "tier");
    if (!tier || !tier[0])
        tier = strdup("trusted");

    eval_ensure_tools_init();
    bool ok = tools_execute_for_tier(tool, args, tier, buf, sizeof(buf));

    if (!ok) {
        r->status = EV_STATUS_FAIL;
        snprintf(r->message, sizeof(r->message),
                 "tools_execute_for_tier(\"%s\") returned false", tool);
    } else {
        char *want = json_get_str(c->expected_raw, "result_contains");
        if (want && !strstr(buf, want)) {
            r->status = EV_STATUS_FAIL;
            snprintf(r->message, sizeof(r->message),
                     "tool result missing expected substring \"%s\" (got: %.64s)", want, buf);
        } else if (!want && buf[0] == '\0') {
            r->status = EV_STATUS_FAIL;
            snprintf(r->message, sizeof(r->message), "tool produced empty result");
        }
    }
    free(args);
    free(tool);
    free(tier);
}

/* ── verifier: gate_fs_write_denied ────────────────────────────────────
 * input:   {"tool":"write_file","arguments":{"file_path":P,"content":X}}
 * expected:{"decision":"deny"}
 * With DSCO_ALLOW_WRITE=0 (Deno-style hard opt-out) the gate must DENY the
 * fs_write capability, execution must not run the tool, and the file must
 * not exist afterwards. The caller's env value is restored afterwards.
 */
static void eval_verify_gate_fs_write_denied(const eval_case_def_t *c, eval_result_t *r) {
    char probe[1024] = "/tmp/dsco_eval_gate_probe";
    char buf[EVAL_RESULT_MAX];
    char reason[256] = "";

    char *args = json_get_raw(c->input_raw, "arguments");
    if (args && json_is_valid_container(args)) {
        char *p = json_get_str(args, "file_path");
        if (p && p[0]) {
            snprintf(probe, sizeof(probe), "%s", p);
            free(p);
        }
    } else {
        free(args);
        args = strdup("{\"file_path\":\"/tmp/dsco_eval_gate_probe\","
                      "\"content\":\"must not be written\"}");
        if (!args) {
            r->status = EV_STATUS_ERROR;
            snprintf(r->message, sizeof(r->message), "out of memory");
            return;
        }
    }

    /* Start clean: the probe must not pre-exist. */
    unlink(probe);

    /* Save + override the Deno-style grant, run, restore. */
    const char *saved = getenv("DSCO_ALLOW_WRITE");
    char saved_buf[512] = "";
    bool had_saved = saved != NULL;
    if (had_saved)
        snprintf(saved_buf, sizeof(saved_buf), "%s", saved);
    setenv("DSCO_ALLOW_WRITE", "0", 1);
    dsco_flow_reset();

    dsco_cap_decision_t decision = dsco_capability_gate("write_file", args, "trusted",
                                                        reason, sizeof(reason));
    bool exec_denied = !tools_execute_for_tier("write_file", args, "trusted", buf, sizeof(buf));

    if (had_saved)
        setenv("DSCO_ALLOW_WRITE", saved_buf, 1);
    else
        unsetenv("DSCO_ALLOW_WRITE");

    bool file_absent = access(probe, F_OK) != 0;
    if (!file_absent)
        unlink(probe); /* never leave a stray probe behind */

    if (decision != CAP_DECISION_DENY) {
        r->status = EV_STATUS_FAIL;
        snprintf(r->message, sizeof(r->message),
                 "capability gate decision=%d (want DENY=2) reason=%.80s", (int)decision, reason);
    } else if (!exec_denied) {
        r->status = EV_STATUS_FAIL;
        snprintf(r->message, sizeof(r->message), "tools_execute_for_tier ran a denied fs_write");
    } else if (!file_absent) {
        r->status = EV_STATUS_FAIL;
        snprintf(r->message, sizeof(r->message), "denied write left %s on disk", probe);
    }

    free(args);
}

/* ── verifier: mcp_tools_list ──────────────────────────────────────────
 * input:   {"mode":"cli","argv":["mcp","serve","--toolsets","core",
 *          "--tier","untrusted"],"timeout_ms":10000}
 * expected:{"tools_contains":["read_file","write_file"]}
 * Spawns the current binary via fork/exec (argv array, no shell), writes
 * three JSON-RPC lines to stdin, reads framed responses from stdout, and
 * requires: id=1 initialize result, id=2 result.tools non-empty array
 * containing each expected tool name, and no response for the notification.
 */
typedef struct {
    char names[64][64];
    int count;
} mcp_names_t;

static void mcp_tool_name_cb(const char *elem, void *ctx) {
    mcp_names_t *nctx = (mcp_names_t *)ctx;
    char *name = json_get_str(elem, "name");
    if (name && nctx->count < 64) {
        snprintf(nctx->names[nctx->count], sizeof(nctx->names[0]), "%s", name);
        nctx->count++;
    }
    free(name);
}

static bool mcp_names_contain(const mcp_names_t *n, const char *name) {
    for (int i = 0; i < n->count; i++)
        if (strcmp(n->names[i], name) == 0)
            return true;
    return false;
}

static void eval_verify_mcp_tools_list(const eval_case_def_t *c, eval_result_t *r,
                                       const char *self_exe) {
    int in_p[2] = {-1, -1}, out_p[2] = {-1, -1};
    char outbuf[EVAL_OUTBUF_MAX];
    size_t outlen = 0;
    int rc_pipe = pipe(in_p);
    if (rc_pipe == 0)
        rc_pipe = pipe(out_p);
    if (rc_pipe != 0) {
        r->status = EV_STATUS_ERROR;
        snprintf(r->message, sizeof(r->message), "pipe: %s", strerror(errno));
        return;
    }

    /* argv from the case, or the canonical default (excluding executable). */
    const char *argv_vec[64];
    int extra_argc = 0;
    {
        /* bounded manual scan: count string elements of input.argv */
        const char *p = strstr(c->input_raw, "\"argv\"");
        if (p) {
            p = strchr(p, '[');
            if (p) {
                p++;
                while (*p && *p != ']' && extra_argc < 60) {
                    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')
                        p++;
                    if (*p == '"') {
                        p++;
                        static char stored[8][256];
                        size_t n = 0;
                        while (*p && *p != '"' && n < sizeof(stored[0]) - 1) {
                            if (*p == '\\' && p[1])
                                p++;
                            stored[extra_argc][n++] = *p++;
                        }
                        stored[extra_argc][n] = '\0';
                        argv_vec[1 + extra_argc] = stored[extra_argc];
                        extra_argc++;
                        while (*p && *p != '"')
                            p++;
                        if (*p == '"')
                            p++;
                    } else if (*p && *p != ']') {
                        p++; /* skip non-string junk */
                    }
                }
            }
        }
    }
    if (extra_argc == 0) {
        static const char *def[] = {"mcp", "serve", "--toolsets", "core", "--tier", "untrusted"};
        for (int i = 0; i < 6; i++)
            argv_vec[1 + i] = def[i];
        extra_argc = 6;
    }
    argv_vec[0] = self_exe;
    argv_vec[1 + extra_argc] = NULL;

    int timeout_ms = json_get_int(c->input_raw, "timeout_ms", EVAL_DEFAULT_TIMEOUT_MS);
    if (timeout_ms <= 0 || timeout_ms > 60000)
        timeout_ms = EVAL_DEFAULT_TIMEOUT_MS;

    signal(SIGPIPE, SIG_IGN);
    pid_t pid = fork();
    if (pid < 0) {
        r->status = EV_STATUS_ERROR;
        snprintf(r->message, sizeof(r->message), "fork: %s", strerror(errno));
        close(in_p[0]);
        close(in_p[1]);
        close(out_p[0]);
        close(out_p[1]);
        return;
    }
    if (pid == 0) {
        /* child: stdin <- in_p[0], stdout -> out_p[1], stderr -> /dev/null */
        dup2(in_p[0], STDIN_FILENO);
        dup2(out_p[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        close(in_p[0]);
        close(in_p[1]);
        close(out_p[0]);
        close(out_p[1]);
        execvp(self_exe, (char *const *)argv_vec);
        _exit(127);
    }

    close(in_p[0]);
    close(out_p[1]);

    /* Protocol stdout must stay clean: keep stderr silenced in the child. */
    static const char *req_lines[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n",
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}\n",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n",
    };
    for (int i = 0; i < 3; i++) {
        size_t len = strlen(req_lines[i]);
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(in_p[1], req_lines[i] + off, len - off);
            if (w <= 0) {
                if (errno == EINTR)
                    continue;
                break; /* child died early; read loop will notice */
            }
            off += (size_t)w;
        }
    }
    close(in_p[1]);

    double deadline = now_ms() + (double)timeout_ms;
    bool timed_out = false;
    while (outlen < sizeof(outbuf) - 1) {
        double remain = deadline - now_ms();
        if (remain <= 0) {
            timed_out = true;
            break;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(out_p[0], &fds);
        struct timeval tv;
        tv.tv_sec = (long)(remain / 1000.0);
        tv.tv_usec = (long)((remain - tv.tv_sec * 1000.0) * 1000.0);
        int sel = select(out_p[0] + 1, &fds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (sel == 0) {
            timed_out = true;
            break;
        }
        ssize_t n = read(out_p[0], outbuf + outlen, sizeof(outbuf) - 1 - outlen);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break; /* EOF: server closed protocol stdout */
        outlen += (size_t)n;
    }
    outbuf[outlen] = '\0';
    close(out_p[0]);

    /* Reap the child: gentle first, hard if needed. */
    int st = 0;
    bool exited = false;
    for (int i = 0; i < 100; i++) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            exited = true;
            break;
        }
        usleep(10 * 1000);
    }
    if (!exited) {
        kill(pid, SIGTERM);
        usleep(50 * 1000);
        if (waitpid(pid, &st, WNOHANG) != pid) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
        }
    }

    if (timed_out) {
        r->status = EV_STATUS_ERROR;
        snprintf(r->message, sizeof(r->message), "mcp serve timed out after %d ms", timeout_ms);
        return;
    }
    if (outlen == 0) {
        r->status = EV_STATUS_ERROR;
        snprintf(r->message, sizeof(r->message), "no JSON-RPC output from mcp serve");
        return;
    }

    /* Split framed responses (one JSON object per line) and verify. */
    bool have_init = false, have_tools = false, bad_notification_response = false;
    mcp_names_t names = {0};

    char *save = NULL;
    for (char *line = strtok_r(outbuf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\r')
            line++;
        if (!line[0])
            continue;
        char *id = json_get_raw(line, "id");
        bool has_method = json_get_str(line, "method") != NULL;
        char *jsonrpc = json_get_str(line, "jsonrpc");
        bool framed = jsonrpc && strcmp(jsonrpc, "2.0") == 0;
        free(jsonrpc);

        if (!id) {
            if (has_method)
                continue; /* a server notification/request is fine */
            continue;
        }
        if (!framed) {
            bad_notification_response = true;
            free(id);
            continue;
        }
        if (strcmp(id, "1") == 0) {
            char *err = json_get_raw(line, "error");
            char *res = json_get_raw(line, "result");
            have_init = res != NULL && err == NULL;
            free(err);
            free(res);
        } else if (strcmp(id, "2") == 0) {
            char *err = json_get_raw(line, "error");
            char *res = json_get_raw(line, "result");
            if (res && !err) {
                int count = json_array_foreach(res, "tools", mcp_tool_name_cb, &names);
                if (count > 0)
                    have_tools = true;
            }
            free(err);
            free(res);
        } else {
            bad_notification_response = true; /* response to a notification */
        }
        free(id);
    }

    /* expected.tools_contains: every listed tool name must be exposed. */
    char missing[256] = "";
    int first_missing = 1;
    mcp_names_t wanted = {0};
    json_array_foreach(c->expected_raw, "tools_contains", mcp_tool_name_cb, &wanted);
    for (int i = 0; i < wanted.count; i++) {
        if (!mcp_names_contain(&names, wanted.names[i])) {
            snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), "%s%s",
                     first_missing ? "" : ",", wanted.names[i]);
            first_missing = 0;
        }
    }

    r->status = EV_STATUS_PASS;
    if (!have_init) {
        r->status = EV_STATUS_FAIL;
        snprintf(r->message, sizeof(r->message), "no successful initialize response (id=1)");
    } else if (!have_tools) {
        r->status = EV_STATUS_FAIL;
        snprintf(r->message, sizeof(r->message),
                 "no tools/list response with non-empty result.tools (id=2)");
    } else if (bad_notification_response) {
        r->status = EV_STATUS_FAIL;
        snprintf(r->message, sizeof(r->message), "server emitted a response for a notification");
    } else if (missing[0]) {
        r->status = EV_STATUS_FAIL;
        snprintf(r->message, sizeof(r->message), "tools missing from tools/list: %.120s", missing);
    }
}

/* ── set loading + validation (validate everything before executing) ──── */

static bool eval_line_is_blank(const char *s) {
    while (*s) {
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
            return false;
        s++;
    }
    return true;
}

static int eval_load_set(const char *path, eval_case_def_t *cases, int max, int *count_out,
                         char *err, size_t err_len) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, err_len, "cannot open eval set: %s", path);
        return 2;
    }
    char line[EVAL_LINE_MAX];
    int count = 0;
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (eval_line_is_blank(line))
            continue;
        if (count >= max) {
            snprintf(err, err_len, "%s:%d too many cases (max %d)", path, lineno, max);
            fclose(f);
            return 2;
        }
        eval_case_def_t *c = &cases[count];
        memset(c, 0, sizeof(*c));

        char *id = json_get_str(line, "id");
        char *verifier = json_get_str(line, "verifier");
        char *input = json_get_raw(line, "input");
        char *expected = json_get_raw(line, "expected");

        bool bad = false;
        if (!id || !id[0]) {
            snprintf(err, err_len, "%s:%d missing \"id\"", path, lineno);
            bad = true;
        } else if (!verifier || !verifier[0]) {
            snprintf(err, err_len, "%s:%d missing \"verifier\"", path, lineno);
            bad = true;
        } else if (strcmp(verifier, "tool_dispatch_smoke") != 0 &&
                   strcmp(verifier, "gate_fs_write_denied") != 0 &&
                   strcmp(verifier, "mcp_tools_list") != 0) {
            snprintf(err, err_len, "%s:%d unknown verifier \"%s\" (v1 supports "
                                   "tool_dispatch_smoke, gate_fs_write_denied, mcp_tools_list)",
                     path, lineno, verifier);
            bad = true;
        } else if (!input || !json_is_valid_container(input)) {
            snprintf(err, err_len, "%s:%d missing/invalid \"input\" object", path, lineno);
            bad = true;
        } else if (!expected || !json_is_valid_container(expected)) {
            snprintf(err, err_len, "%s:%d missing/invalid \"expected\" object", path, lineno);
            bad = true;
        } else {
            for (int i = 0; i < count; i++) {
                if (strcmp(cases[i].id, id) == 0) {
                    snprintf(err, err_len, "%s:%d duplicate case id \"%s\"", path, lineno, id);
                    bad = true;
                    break;
                }
            }
        }
        if (!bad && (strlen(input) >= sizeof(c->input_raw) ||
                     strlen(expected) >= sizeof(c->expected_raw))) {
            snprintf(err, err_len, "%s:%d input/expected object too large", path, lineno);
            bad = true;
        }
        if (!bad) {
            snprintf(c->id, sizeof(c->id), "%s", id);
            snprintf(c->verifier, sizeof(c->verifier), "%s", verifier);
            snprintf(c->input_raw, sizeof(c->input_raw), "%s", input);
            snprintf(c->expected_raw, sizeof(c->expected_raw), "%s", expected);
            count++;
        }
        free(id);
        free(verifier);
        free(input);
        free(expected);
        if (bad) {
            fclose(f);
            return 2;
        }
    }
    fclose(f);
    if (count == 0) {
        snprintf(err, err_len, "%s contains no cases", path);
        return 2;
    }
    *count_out = count;
    return 0;
}

/* ── reporting ───────────────────────────────────────────────────────── */

static void eval_report_human(const char *set, const eval_result_t *res, int count) {
    printf("ID                              VERIFIER                  STATUS  LATENCY\n");
    printf("%-32s%-26s%-8s%s\n", "----", "--------", "------", "-------");
    int passed = 0, failed = 0, errored = 0;
    for (int i = 0; i < count; i++) {
        const char *status = res[i].status == EV_STATUS_PASS  ? "PASS"
                             : res[i].status == EV_STATUS_FAIL ? "FAIL"
                                                               : "ERROR";
        if (res[i].status == EV_STATUS_PASS)
            passed++;
        else if (res[i].status == EV_STATUS_FAIL)
            failed++;
        else
            errored++;
        printf("%-32s%-26s%-8s%.1f ms\n", res[i].id, res[i].verifier, status, res[i].latency_ms);
    }
    printf("\n%d cases: %d passed, %d failed, %d errored  (set: %s)\n", count, passed, failed,
           errored, set);
}

static void eval_report_json(const char *set, const eval_result_t *res, int count) {
    int passed = 0;
    for (int i = 0; i < count; i++)
        if (res[i].status == EV_STATUS_PASS)
            passed++;

    jbuf_t b;
    jbuf_init(&b, 4096);
    jbuf_append(&b, "{\"set\":");
    jbuf_append_json_str(&b, set);
    jbuf_append(&b, ",\"cases\":[");
    for (int i = 0; i < count; i++) {
        if (i > 0)
            jbuf_append(&b, ",");
        jbuf_append(&b, "{\"id\":");
        jbuf_append_json_str(&b, res[i].id);
        jbuf_appendf(&b, ",\"pass\":%s,\"latency_ms\":%.2f}",
                     res[i].status == EV_STATUS_PASS ? "true" : "false", res[i].latency_ms);
    }
    jbuf_appendf(&b, "],\"passed\":%d,\"total\":%d}\n", passed, count);
    fwrite(b.data, 1, b.len, stdout);
    jbuf_free(&b);
}

/* ── CLI entry ───────────────────────────────────────────────────────── */

int eval_cli(int argc, char **argv) {
    const char *set = NULL;
    bool as_json = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            as_json = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr, "Usage: dsco eval run <set> [--json]\n"
                            "  <set>    name resolved as evals/<set>.jsonl "
                            "(or DSCO_EVAL_SET_DIR override), or a verbatim .jsonl path\n"
                            "  --json   emit {\"set\",\"cases\":[{id,pass,latency_ms}],"
                            "\"passed\",\"total\"} on stdout\n");
            return 0;
        } else if (argv[i][0] != '-') {
            /* dispatcher passes normalized argv with argv[0]=program and the
             * subcommand tokens ("eval", "run") still present — skip them */
            if (!set && strcmp(argv[i], "run") != 0 && strcmp(argv[i], "eval") != 0)
                set = argv[i];
        }
    }
    if (!set) {
        fprintf(stderr, "Usage: dsco eval run <set> [--json]\n");
        return 2;
    }

    char path[4096];
    if (eval_resolve_set_path(set, path, sizeof(path)) != 0) {
        fprintf(stderr, "eval: cannot resolve set \"%s\" (tried evals/%s.jsonl)\n", set, set);
        return 2;
    }

    /* eval_case_def_t embeds two 48K JSON buffers; 256 cases ≈ 25MB — heap,
     * not stack, or we overflow on entry. */
    eval_case_def_t *cases = calloc(EVAL_MAX_CASES, sizeof(eval_case_def_t));
    if (!cases) {
        fprintf(stderr, "eval: out of memory\n");
        return 2;
    }
    int count = 0;
    char err[EVAL_MSG_MAX];
    int lrc = eval_load_set(path, cases, EVAL_MAX_CASES, &count, err, sizeof(err));
    if (lrc != 0) {
        fprintf(stderr, "eval: %s\n", err);
        free(cases);
        return lrc;
    }

    char self_exe[4096];
    eval_self_exe(self_exe, sizeof(self_exe), argc > 0 ? argv[0] : NULL);

    eval_result_t results[EVAL_MAX_CASES];
    memset(results, 0, sizeof(results));

    for (int i = 0; i < count; i++) {
        eval_case_def_t *c = &cases[i];
        eval_result_t *r = &results[i];
        snprintf(r->id, sizeof(r->id), "%s", c->id);
        snprintf(r->verifier, sizeof(r->verifier), "%s", c->verifier);
        r->status = EV_STATUS_PASS;

        double t0 = now_ms();
        if (strcmp(c->verifier, "tool_dispatch_smoke") == 0) {
            eval_verify_tool_dispatch(c, r);
        } else if (strcmp(c->verifier, "gate_fs_write_denied") == 0) {
            eval_verify_gate_fs_write_denied(c, r);
        } else if (strcmp(c->verifier, "mcp_tools_list") == 0) {
            eval_verify_mcp_tools_list(c, r, self_exe);
        }
        r->latency_ms = now_ms() - t0;

        if (r->status != EV_STATUS_PASS && !as_json)
            fprintf(stderr, "eval: %s: %s\n", r->id,
                    r->message[0] ? r->message : "case failed");
    }

    if (as_json)
        eval_report_json(set, results, count);
    else
        eval_report_human(set, results, count);

    for (int i = 0; i < count; i++)
        if (results[i].status != EV_STATUS_PASS) {
            free(cases);
            return 1;
        }
    free(cases);
    return 0;
}
