/* compute.c — adaptive modular compute fabric.
 *
 * Registers pluggable compute backends as connector "kinds" and provides an
 * adaptive router (`dsco compute`) that picks one per task. local/shell and
 * api/tool are already builtin connector kinds; this adds:
 *   fleet  — run on a ~/bridge/fleet peer over plain SSH (reuses remote_cli)
 *   modal  — run on Modal serverless compute over HTTPS
 * The router is local-first and composes with the `flow` kind like any other.
 */

#include "compute.h"
#include "connector.h"
#include "remote_cli.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <curl/curl.h>

/* ── Shared: fork+exec argv, capture stdout+stderr, return exit code ─────── */
static int capture_argv(char *const argv[], char **out) {
    int pfd[2];
    if (pipe(pfd) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(pfd[0]);
        int st;
        waitpid(pid, &st, 0);
        return -1;
    }
    ssize_t r;
    char tmp[4096];
    while ((r = read(pfd[0], tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)r + 1 > cap) {
            size_t ncap = (len + (size_t)r + 1) * 2;
            char *nb = realloc(buf, ncap);
            if (!nb)
                break;
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, tmp, (size_t)r);
        len += (size_t)r;
    }
    buf[len] = '\0';
    close(pfd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (out)
        *out = buf;
    else
        free(buf);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Wrap a raw command output as {"exit":N,"stdout":"…"} JSON. */
static char *wrap_exec_result(int code, const char *raw) {
    jbuf_t b;
    jbuf_init(&b, (raw ? strlen(raw) : 0) + 64);
    jbuf_append(&b, "{\"exit\":");
    jbuf_append_int(&b, code);
    jbuf_append(&b, ",\"stdout\":");
    jbuf_append_json_str(&b, raw ? raw : "");
    jbuf_append(&b, "}");
    return b.data;
}

/* ══════════════════════════════════════════════════════════════════════════
 * BACKEND: "fleet"  — run a command on a ~/bridge/fleet peer over SSH
 *   config:  {"peer":"matrix"}
 *   invoke:  method advisory, params.cmd → {"exit":N,"stdout":"…"}
 * ══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    char *peer;
    char *user;
    char *addr;
} fleet_conn_t;

static void *fleet_open(const char *config_json, char *err, size_t errlen) {
    char peer[128] = "";
    if (config_json && config_json[0]) {
        char *p = json_get_str(config_json, "peer");
        if (p) {
            snprintf(peer, sizeof(peer), "%s", p);
            free(p);
        }
    }
    if (!peer[0]) {
        snprintf(err, errlen, "fleet: config.peer required, e.g. {\"peer\":\"matrix\"}");
        return NULL;
    }
    char user[128] = "", addr[128] = "";
    if (!dsco_fleet_resolve(peer, user, sizeof(user), addr, sizeof(addr)) || !addr[0]) {
        snprintf(err, errlen, "fleet: unknown peer '%s' (no ~/bridge/fleet/%s.host)", peer, peer);
        return NULL;
    }
    fleet_conn_t *fc = calloc(1, sizeof(*fc));
    if (!fc) {
        snprintf(err, errlen, "fleet: out of memory");
        return NULL;
    }
    fc->peer = strdup(peer);
    fc->user = strdup(user[0] ? user : "agent");
    fc->addr = strdup(addr);
    return fc;
}

static void fleet_invoke(void *self, const char *method, const char *params, conn_result_t *out) {
    (void)method;
    fleet_conn_t *fc = self;
    char *cmd = params ? json_get_str(params, "cmd") : NULL;
    if (!cmd || !cmd[0]) {
        free(cmd);
        if (out)
            snprintf(out->error, sizeof(out->error), "fleet: params.cmd required");
        return;
    }
    char target[300];
    snprintf(target, sizeof(target), "%s@%s", fc->user, fc->addr);
    char *argv[] = {"ssh",
                    "-o",
                    "BatchMode=yes",
                    "-o",
                    "ConnectTimeout=8",
                    "-o",
                    "StrictHostKeyChecking=accept-new",
                    target,
                    cmd,
                    NULL};
    char *raw = NULL;
    int code = capture_argv(argv, &raw);
    free(cmd);
    if (!out) {
        free(raw);
        return;
    }
    if (code < 0) {
        out->status = -1;
        snprintf(out->error, sizeof(out->error), "fleet: ssh to %s failed", fc->peer);
        free(raw);
        return;
    }
    out->status = (code == 0) ? 0 : 1;
    out->body = wrap_exec_result(code, raw);
    free(raw);
}

static char *fleet_describe(void *self) {
    fleet_conn_t *fc = self;
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"kind\":\"fleet\",\"peer\":");
    jbuf_append_json_str(&b, fc->peer);
    jbuf_append(&b, ",\"target\":");
    char target[300];
    snprintf(target, sizeof(target), "%s@%s", fc->user, fc->addr);
    jbuf_append_json_str(&b, target);
    jbuf_append(&b, ",\"params\":{\"cmd\":\"string (required)\"},"
                    "\"returns\":{\"exit\":\"int\",\"stdout\":\"string\"}}");
    return b.data;
}

static void fleet_close(void *self) {
    fleet_conn_t *fc = self;
    if (fc) {
        free(fc->peer);
        free(fc->user);
        free(fc->addr);
        free(fc);
    }
}

static const connector_vtable_t FLEET_VT = {
    .kind = "fleet",
    .description = "Run on a ~/bridge/fleet peer over SSH — remote compute, no spool/daemon",
    .capabilities = CONN_CAP_INVOKE | CONN_CAP_ACTUATE,
    .osi_layers = OSI_L7_APPLICATION,
    .open = fleet_open,
    .invoke = fleet_invoke,
    .stream = NULL,
    .describe = fleet_describe,
    .schema = NULL,
    .close = fleet_close,
};

/* ══════════════════════════════════════════════════════════════════════════
 * BACKEND: "modal" — Modal serverless compute over HTTPS
 *   config:  {"url":"https://…modal.run/…","token":"…"}  (or env DSCO_MODAL_URL/TOKEN)
 *   invoke:  params posted as the JSON request body → response body
 * ══════════════════════════════════════════════════════════════════════════ */
typedef struct {
    char *url;
    char *token;
} modal_conn_t;

struct curlbuf {
    char *p;
    size_t n;
};

static size_t curl_wr(void *data, size_t s, size_t nm, void *u) {
    struct curlbuf *b = u;
    size_t add = s * nm;
    char *np = realloc(b->p, b->n + add + 1);
    if (!np)
        return 0;
    b->p = np;
    memcpy(b->p + b->n, data, add);
    b->n += add;
    b->p[b->n] = '\0';
    return add;
}

static int https_post(const char *url, const char *token, const char *body, char **out,
                      long *http_code) {
    CURL *c = curl_easy_init();
    if (!c)
        return -1;
    struct curlbuf buf = {0};
    struct curl_slist *hdr = NULL;
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    char auth[600];
    if (token && token[0]) {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);
        hdr = curl_slist_append(hdr, auth);
    }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "{}");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "dsco-compute/1.0");
    CURLcode rc = curl_easy_perform(c);
    if (http_code)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, http_code);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        free(buf.p);
        return -1;
    }
    if (out)
        *out = buf.p ? buf.p : strdup("");
    else
        free(buf.p);
    return 0;
}

static void *modal_open(const char *config_json, char *err, size_t errlen) {
    char *url = NULL, *token = NULL;
    if (config_json && config_json[0]) {
        url = json_get_str(config_json, "url");
        token = json_get_str(config_json, "token");
    }
    if (!url || !url[0]) {
        free(url);
        const char *env = getenv("DSCO_MODAL_URL");
        url = (env && env[0]) ? strdup(env) : NULL;
    }
    if (!token || !token[0]) {
        free(token);
        const char *env = getenv("DSCO_MODAL_TOKEN");
        token = (env && env[0]) ? strdup(env) : NULL;
    }
    if (!url) {
        snprintf(err, errlen, "modal: url required (config {\"url\":…} or env DSCO_MODAL_URL)");
        free(token);
        return NULL;
    }
    modal_conn_t *mc = calloc(1, sizeof(*mc));
    if (!mc) {
        free(url);
        free(token);
        snprintf(err, errlen, "modal: out of memory");
        return NULL;
    }
    mc->url = url;
    mc->token = token;
    return mc;
}

static void modal_invoke(void *self, const char *method, const char *params, conn_result_t *out) {
    (void)method;
    modal_conn_t *mc = self;
    char *resp = NULL;
    long code = 0;
    int rc = https_post(mc->url, mc->token, (params && params[0]) ? params : "{}", &resp, &code);
    if (!out) {
        free(resp);
        return;
    }
    if (rc != 0) {
        out->status = -1;
        snprintf(out->error, sizeof(out->error), "modal: POST %s failed", mc->url);
        free(resp);
        return;
    }
    out->status = (code >= 200 && code < 300) ? 0 : code;
    out->body = resp ? resp : strdup("{}");
}

static char *modal_describe(void *self) {
    modal_conn_t *mc = self;
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"kind\":\"modal\",\"url\":");
    jbuf_append_json_str(&b, mc->url);
    jbuf_append(&b, ",\"auth\":");
    jbuf_append_json_str(&b, mc->token ? "bearer" : "none");
    jbuf_append(&b, ",\"transport\":\"https\"}");
    return b.data;
}

static void modal_close(void *self) {
    modal_conn_t *mc = self;
    if (mc) {
        free(mc->url);
        free(mc->token);
        free(mc);
    }
}

static const connector_vtable_t MODAL_VT = {
    .kind = "modal",
    .description = "Modal serverless compute over HTTPS — burst/GPU offload",
    .capabilities = CONN_CAP_INVOKE,
    .osi_layers = OSI_L7_APPLICATION | OSI_L6_PRESENTATION,
    .open = modal_open,
    .invoke = modal_invoke,
    .stream = NULL,
    .describe = modal_describe,
    .schema = NULL,
    .close = modal_close,
};

/* ══════════════════════════════════════════════════════════════════════════
 * Registration
 * ══════════════════════════════════════════════════════════════════════════ */
void compute_register_backends(void) {
    static int done = 0;
    if (done)
        return;
    done = 1;
    connector_register(&FLEET_VT);
    connector_register(&MODAL_VT);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Adaptive router — pick a backend kind for a task. Local-first.
 * ══════════════════════════════════════════════════════════════════════════ */
static const char *compute_route(const char *on, const char *peer, const char *need) {
    if (on && on[0])
        return on; /* explicit override */
    if (peer && peer[0])
        return "fleet"; /* targeted offload to a machine */
    if (need && (strcmp(need, "gpu") == 0 || strcmp(need, "heavy") == 0) &&
        getenv("DSCO_MODAL_URL"))
        return "modal"; /* serverless burst for heavy/GPU */
    return "shell";     /* local-first default */
}

/* ══════════════════════════════════════════════════════════════════════════
 * CLI: `dsco compute …`
 * ══════════════════════════════════════════════════════════════════════════ */
static void compute_list(void) {
    connector_register_builtins();
    printf("\033[1madaptive compute fabric\033[0m — backends (connector kinds):\n\n");
    printf("  %-8s %-8s %-16s %s\n", "ROLE", "KIND", "STATUS", "DESCRIPTION");

    /* local */
    printf("  %-8s %-8s %-16s %s\n", "local", "shell", "ready", "Apple-Silicon native exec");
    /* fleet */
    {
        const char *home = getenv("HOME");
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/bridge/fleet", home ? home : "/tmp");
        int has = access(dir, F_OK) == 0;
        printf("  %-8s %-8s %-16s %s\n", "fleet", "fleet", has ? "peers (dsco fleet)" : "no peers",
               "SSH to ~/bridge/fleet peers");
    }
    /* modal */
    {
        const char *u = getenv("DSCO_MODAL_URL");
        printf("  %-8s %-8s %-16s %s\n", "modal", "modal",
               (u && u[0]) ? "configured" : "set DSCO_MODAL_URL", "Modal serverless (HTTPS)");
    }
    /* api */
    printf("  %-8s %-8s %-16s %s\n", "api", "tool", "ready", "Tool Management API (HTTP)");

    printf("\nauto-route: --on wins; else --peer→fleet; else --need gpu|heavy + "
           "DSCO_MODAL_URL→modal; else local.\n");
    printf("run: dsco compute [--on KIND] [--peer P] [--need gpu|heavy] <task…>\n");
}

int dsco_compute_cli(int argc, char **argv) {
    const char *on = NULL, *peer = NULL, *need = NULL;
    int ti = 2;
    for (; ti < argc; ti++) {
        if (strcmp(argv[ti], "--list") == 0 || strcmp(argv[ti], "list") == 0) {
            compute_list();
            return 0;
        } else if (strcmp(argv[ti], "--on") == 0 && ti + 1 < argc) {
            on = argv[++ti];
        } else if (strcmp(argv[ti], "--peer") == 0 && ti + 1 < argc) {
            peer = argv[++ti];
        } else if (strcmp(argv[ti], "--need") == 0 && ti + 1 < argc) {
            need = argv[++ti];
        } else if (strncmp(argv[ti], "--", 2) == 0) {
            continue; /* ignore unknown flags */
        } else {
            break; /* start of task */
        }
    }
    if (ti >= argc) {
        fprintf(stderr, "usage:\n"
                        "  dsco compute --list\n"
                        "  dsco compute [--on KIND] [--peer PEER] [--need gpu|heavy] <task…>\n"
                        "\nexamples:\n"
                        "  dsco compute uname -a                 (local)\n"
                        "  dsco compute --peer matrix nproc      (fleet)\n"
                        "  dsco compute --on modal '{\"x\":1}'     (serverless)\n");
        return 2;
    }

    /* Join the remaining args into the task string. */
    size_t need_len = 1;
    for (int i = ti; i < argc; i++)
        need_len += strlen(argv[i]) + 1;
    char *task = malloc(need_len);
    if (!task)
        return 1;
    task[0] = '\0';
    for (int i = ti; i < argc; i++) {
        strcat(task, argv[i]);
        if (i + 1 < argc)
            strcat(task, " ");
    }

    const char *kind = compute_route(on, peer, need);
    if (strcmp(kind, "fleet") == 0 && (!peer || !peer[0])) {
        fprintf(stderr, "dsco compute: fleet backend requires --peer <name>\n");
        free(task);
        return 2;
    }

    connector_register_builtins();

    /* Build config + params per backend. */
    char config[256] = "";
    char *params = NULL;
    const char *method = "run";
    if (strcmp(kind, "fleet") == 0) {
        snprintf(config, sizeof(config), "{\"peer\":\"%s\"}", peer);
        jbuf_t b;
        jbuf_init(&b, need_len + 16);
        jbuf_append(&b, "{\"cmd\":");
        jbuf_append_json_str(&b, task);
        jbuf_append(&b, "}");
        params = b.data;
    } else if (strcmp(kind, "shell") == 0) {
        jbuf_t b;
        jbuf_init(&b, need_len + 16);
        jbuf_append(&b, "{\"cmd\":");
        jbuf_append_json_str(&b, task);
        jbuf_append(&b, "}");
        params = b.data;
    } else if (strcmp(kind, "modal") == 0) {
        /* task is a JSON payload if it looks like one, else wrap as {"input":…} */
        if (task[0] == '{' || task[0] == '[') {
            params = strdup(task);
        } else {
            jbuf_t b;
            jbuf_init(&b, need_len + 16);
            jbuf_append(&b, "{\"input\":");
            jbuf_append_json_str(&b, task);
            jbuf_append(&b, "}");
            params = b.data;
        }
    } else if (strcmp(kind, "tool") == 0) {
        /* task = remote tool name; method carries it, params empty */
        method = task;
        params = strdup("{}");
    } else {
        params = strdup("{}");
    }

    fprintf(stderr, "\033[2m[compute] → %s%s%s\033[0m\n", kind, peer ? " peer=" : "",
            peer ? peer : "");

    char err[256] = "";
    connector_t *c = connector_open(kind, config[0] ? config : NULL, err, sizeof(err));
    if (!c) {
        fprintf(stderr, "dsco compute: %s\n", err[0] ? err : "open failed");
        free(task);
        free(params);
        return 1;
    }
    conn_result_t res;
    memset(&res, 0, sizeof(res));
    connector_invoke(c, method, params, &res);

    int rc = 0;
    if (res.status < 0) {
        fprintf(stderr, "dsco compute: %s\n", res.error[0] ? res.error : "invoke failed");
        rc = 1;
    } else {
        /* Pretty-print: for shell/fleet unwrap {"exit","stdout"}; else raw body. */
        if (res.body) {
            char *stdout_s = json_get_str(res.body, "stdout");
            if (stdout_s) {
                fputs(stdout_s, stdout);
                free(stdout_s);
                int ex = json_get_int(res.body, "exit", 0);
                rc = ex;
            } else {
                fputs(res.body, stdout);
                if (res.body[strlen(res.body) - 1] != '\n')
                    fputc('\n', stdout);
                rc = (res.status == 0) ? 0 : 1;
            }
        }
    }

    connector_close(c);
    conn_result_free(&res);
    free(task);
    free(params);
    return rc;
}
