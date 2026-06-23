/* termios raw-mode helpers (cfmakeraw) and >38400 baud constants are BSD
 * extensions hidden by the build's -D_POSIX_C_SOURCE; re-enable them. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "connector.h"
#include "toolmgmt.h"
#include "json_util.h"
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sqlite3.h>

/* ── Shared helpers ───────────────────────────────────────────────────── */
static long conn_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static bool conn_is_json_scalar(const char *v) {
    if (!v || !v[0])
        return false;
    if (strcmp(v, "true") == 0 || strcmp(v, "false") == 0 || strcmp(v, "null") == 0)
        return true;
    if (v[0] == '{' || v[0] == '[' || v[0] == '"')
        return true;
    const char *p = v;
    if (*p == '-' || *p == '+')
        p++;
    bool digit = false, dot = false;
    for (; *p; p++) {
        if (isdigit((unsigned char)*p))
            digit = true;
        else if (*p == '.' && !dot)
            dot = true;
        else
            return false;
    }
    return digit;
}

/* Capability bitmask <- comma-separated names ("invoke,tx,stream,…"). */
static unsigned conn_caps_from_str(const char *s) {
    if (!s)
        return 0;
    const struct {
        const char *n;
        unsigned f;
    } tab[] = {
        {"invoke", CONN_CAP_INVOKE}, {"stream", CONN_CAP_STREAM}, {"subscribe", CONN_CAP_SUBSCRIBE},
        {"tx", CONN_CAP_TX},         {"sense", CONN_CAP_SENSE},   {"actuate", CONN_CAP_ACTUATE},
    };
    unsigned out = 0;
    char *dup = safe_strdup(s);
    for (char *tok = strtok(dup, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ')
            tok++;
        for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
            if (strcmp(tok, tab[i].n) == 0)
                out |= tab[i].f;
    }
    free(dup);
    return out;
}

/* Substitute feedback placeholders inside a params-template string. Recognized
 * (quoted) tokens, each replaced by the raw JSON of a prior result or a field
 * extracted from it:
 *   "{{seed}}"            the seed body passed into the flow
 *   "{{prev}}"            the most recent step's body
 *   "{{prevN}}" / "{{stepN}}"   the Nth step's body (1-based)
 *   "{{prev:key}}" etc.   json field `key` pulled from that body
 * Returns a malloc'd string (caller frees). */
static char *conn_subst(const char *tmpl, char **outputs, int ndone, const char *seed) {
    jbuf_t b;
    jbuf_init(&b, tmpl ? strlen(tmpl) + 64 : 64);
    const char *p = tmpl ? tmpl : "";
    while (*p) {
        if (p[0] == '"' && strncmp(p + 1, "{{", 2) == 0) {
            const char *end = strstr(p + 3, "}}\"");
            if (end) {
                char tok[64];
                size_t tlen = (size_t)(end - (p + 3));
                if (tlen < sizeof(tok)) {
                    memcpy(tok, p + 3, tlen);
                    tok[tlen] = '\0';
                    char *colon = strchr(tok, ':');
                    char field[48] = {0};
                    if (colon) {
                        *colon = '\0';
                        snprintf(field, sizeof(field), "%s", colon + 1);
                    }
                    const char *body = NULL;
                    if (strcmp(tok, "seed") == 0)
                        body = seed;
                    else if (strcmp(tok, "prev") == 0 && ndone > 0)
                        body = outputs[ndone - 1];
                    else {
                        int idx = -1;
                        if (strncmp(tok, "prev", 4) == 0)
                            idx = atoi(tok + 4);
                        else if (strncmp(tok, "step", 4) == 0)
                            idx = atoi(tok + 4);
                        if (idx >= 1 && idx <= ndone)
                            body = outputs[idx - 1];
                    }
                    if (body) {
                        char *picked = field[0] ? json_get_raw(body, field) : NULL;
                        const char *val = picked ? picked : body;
                        if (conn_is_json_scalar(val))
                            jbuf_append(&b, val);
                        else
                            jbuf_append_json_str(&b, val);
                        free(picked);
                    } else {
                        jbuf_append(&b, "null");
                    }
                    p = end + 3;
                    continue;
                }
            }
        }
        jbuf_append_char(&b, *p);
        p++;
    }
    return b.data;
}

/* ── Instance type ────────────────────────────────────────────────────── */
struct connector {
    const connector_vtable_t *vt;
    void *self;
};

void conn_result_free(conn_result_t *r) {
    if (!r)
        return;
    free(r->body);
    r->body = NULL;
    r->status = 0;
    r->error[0] = '\0';
}

/* ── Registry ─────────────────────────────────────────────────────────── */
#define CONN_MAX_KINDS 32
static const connector_vtable_t *g_kinds[CONN_MAX_KINDS];
static int g_nkinds = 0;

int connector_register(const connector_vtable_t *vt) {
    if (!vt || !vt->kind || !vt->kind[0])
        return -1;
    if (connector_find(vt->kind))
        return -1; /* no dup kinds */
    if (g_nkinds >= CONN_MAX_KINDS)
        return -1;
    g_kinds[g_nkinds++] = vt;
    return 0;
}

const connector_vtable_t *connector_find(const char *kind) {
    if (!kind)
        return NULL;
    for (int i = 0; i < g_nkinds; i++)
        if (strcmp(g_kinds[i]->kind, kind) == 0)
            return g_kinds[i];
    return NULL;
}

int connector_list(const connector_vtable_t **out, int max) {
    int n = g_nkinds < max ? g_nkinds : max;
    for (int i = 0; i < n; i++)
        out[i] = g_kinds[i];
    return g_nkinds;
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */
connector_t *connector_open(const char *kind, const char *config_json, char *err, size_t errlen) {
    const connector_vtable_t *vt = connector_find(kind);
    if (!vt) {
        if (err && errlen)
            snprintf(err, errlen, "unknown connector kind: %s", kind ? kind : "(null)");
        return NULL;
    }
    void *self = NULL;
    if (vt->open) {
        char oerr[256] = {0};
        self = vt->open(config_json, oerr, sizeof(oerr));
        if (!self) {
            if (err && errlen)
                snprintf(err, errlen, "%s: %s", vt->kind, oerr[0] ? oerr : "open failed");
            return NULL;
        }
    }
    connector_t *c = safe_malloc(sizeof(*c));
    c->vt = vt;
    c->self = self;
    return c;
}

static int g_validate = 0;
void connector_set_validate(int on) {
    g_validate = on;
}

char *connector_schema(connector_t *c, const char *method) {
    if (!c || !c->vt->schema)
        return NULL;
    return c->vt->schema(c->self, method);
}

int connector_validate(connector_t *c, const char *method, const char *params_json, char *err,
                       size_t errlen) {
    char *schema = connector_schema(c, method);
    if (!schema)
        return -1;
    json_validation_t v = json_validate_schema(params_json ? params_json : "{}", schema);
    free(schema);
    if (!v.valid) {
        if (err && errlen)
            snprintf(err, errlen, "%s", v.error);
        return 0;
    }
    return 1;
}

void connector_invoke(connector_t *c, const char *method, const char *params_json,
                      conn_result_t *out) {
    if (out) {
        out->status = -1;
        out->body = NULL;
        out->error[0] = '\0';
    }
    if (!c || !c->vt->invoke) {
        if (out)
            snprintf(out->error, sizeof(out->error), "connector does not support invoke");
        return;
    }
    if (g_validate) {
        char verr[256];
        if (connector_validate(c, method, params_json, verr, sizeof(verr)) == 0) {
            if (out) {
                out->status = 1;
                snprintf(out->error, sizeof(out->error), "schema: %s", verr);
            }
            if (getenv("DSCO_CONNECT_TRACE"))
                fprintf(stderr, "[connect] %s invoke %s REJECTED %s\n", c->vt->kind,
                        method ? method : "(none)", verr);
            return;
        }
    }
    long t0 = getenv("DSCO_CONNECT_TRACE") ? conn_now_ms() : 0;
    c->vt->invoke(c->self, method, params_json, out);
    if (t0)
        fprintf(stderr, "[connect] %s invoke %s status=%ld %ldms\n", c->vt->kind,
                method ? method : "(none)", out ? out->status : 0, conn_now_ms() - t0);
}

void connector_stream(connector_t *c, const char *method, const char *params_json,
                      conn_chunk_cb on_chunk, void *ctx, conn_result_t *out) {
    if (out) {
        out->status = -1;
        out->body = NULL;
        out->error[0] = '\0';
    }
    if (!c || !c->vt->stream) {
        if (out)
            snprintf(out->error, sizeof(out->error), "connector does not support stream");
        return;
    }
    long t0 = getenv("DSCO_CONNECT_TRACE") ? conn_now_ms() : 0;
    c->vt->stream(c->self, method, params_json, on_chunk, ctx, out);
    if (t0)
        fprintf(stderr, "[connect] %s stream %s status=%ld %ldms\n", c->vt->kind,
                method ? method : "(none)", out ? out->status : 0, conn_now_ms() - t0);
}

char *connector_describe(connector_t *c) {
    if (!c || !c->vt->describe)
        return NULL;
    return c->vt->describe(c->self);
}

unsigned connector_capabilities(const connector_t *c) {
    return c ? c->vt->capabilities : 0u;
}

void connector_close(connector_t *c) {
    if (!c)
        return;
    if (c->vt->close)
        c->vt->close(c->self);
    free(c);
}

/* ── Built-in adapter: "tool" (Tool Management API over HTTP) ─────────────
 * method = remote tool name, params = its inputs object. This is the first
 * concrete proof of the seam; future kinds register the same way. */
typedef struct {
    int _stub;
} tool_conn_t;

static void *tool_open(const char *config_json, char *err, size_t errlen) {
    (void)err;
    (void)errlen;
    if (config_json && config_json[0]) {
        char *url = json_get_str(config_json, "url");
        char *token = json_get_str(config_json, "token");
        if (url && url[0])
            toolmgmt_set_base_url(url);
        if (token && token[0])
            toolmgmt_set_token(token);
        free(url);
        free(token);
    }
    return safe_malloc(sizeof(tool_conn_t));
}

static void tool_invoke(void *self, const char *method, const char *params_json,
                        conn_result_t *out) {
    (void)self;
    if (!method || !method[0]) {
        if (out)
            snprintf(out->error, sizeof(out->error), "missing tool name");
        return;
    }
    char *body = toolmgmt_execute(method, params_json, 0);
    if (!out) {
        free(body);
        return;
    }
    if (!body) {
        out->status = -1;
        snprintf(out->error, sizeof(out->error), "tool '%s' returned no response", method);
        return;
    }
    char *st = json_get_str(body, "status");
    out->status = (st && strcmp(st, "success") == 0) ? 0 : 1;
    free(st);
    out->body = body;
}

static char *tool_describe(void *self) {
    (void)self;
    return toolmgmt_list_tools(1000);
}

static void tool_close(void *self) {
    free(self);
}

/* Derive a JSON-Schema for a tool's params from the catalog's typed `inputs`,
 * so the seam can type-check before dispatch. */
typedef struct {
    const char *want;
    jbuf_t *schema;
    int found;
} tool_schema_ctx_t;

static void tool_schema_cb(const char *el, void *ctx) {
    tool_schema_ctx_t *c = ctx;
    if (c->found)
        return;
    char *name = json_get_str(el, "name");
    if (!name || strcmp(name, c->want) != 0) {
        free(name);
        return;
    }
    free(name);
    c->found = 1;
    jbuf_t *b = c->schema;
    jbuf_append(b, "{\"type\":\"object\",\"properties\":{");
    jbuf_t reqbuf;
    jbuf_init(&reqbuf, 64);
    int nprop = 0, nreq = 0;
    char *inputs = json_get_raw(el, "inputs");
    if (inputs) {
        const char *p = strchr(inputs, '[');
        if (p)
            p++;
        while (p && *p) {
            while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t')
                p++;
            if (*p == ']' || *p == '\0')
                break;
            if (*p != '{') {
                p++;
                continue;
            }
            const char *start = p;
            int depth = 0;
            bool instr = false;
            for (; *p; p++) {
                if (instr) {
                    if (*p == '\\')
                        p++;
                    else if (*p == '"')
                        instr = false;
                    continue;
                }
                if (*p == '"')
                    instr = true;
                else if (*p == '{')
                    depth++;
                else if (*p == '}') {
                    depth--;
                    if (depth == 0) {
                        p++;
                        break;
                    }
                }
            }
            size_t elen = (size_t)(p - start);
            char *elem = safe_malloc(elen + 1);
            memcpy(elem, start, elen);
            elem[elen] = '\0';
            char *pn = json_get_str(elem, "name");
            char *pt = json_get_str(elem, "type");
            bool prequired = json_get_bool(elem, "required", false);
            if (pn && pn[0]) {
                if (nprop)
                    jbuf_append(b, ",");
                jbuf_append_json_str(b, pn);
                jbuf_append(b, ":{");
                if (pt && pt[0]) {
                    jbuf_append(b, "\"type\":");
                    jbuf_append_json_str(b, pt);
                }
                jbuf_append(b, "}");
                nprop++;
                if (prequired) {
                    if (nreq)
                        jbuf_append(&reqbuf, ",");
                    jbuf_append_json_str(&reqbuf, pn);
                    nreq++;
                }
            }
            free(pn);
            free(pt);
            free(elem);
        }
        free(inputs);
    }
    jbuf_append(b, "},\"required\":[");
    jbuf_append(b, reqbuf.data ? reqbuf.data : "");
    jbuf_append(b, "]}");
    jbuf_free(&reqbuf);
}

static char *tool_schema(void *self, const char *method) {
    (void)self;
    if (!method || !method[0])
        return NULL;
    char *cat = toolmgmt_list_tools(1000);
    if (!cat)
        return NULL;
    jbuf_t wrap;
    jbuf_init(&wrap, strlen(cat) + 32);
    jbuf_append(&wrap, "{\"__tm_items__\":");
    jbuf_append(&wrap, cat);
    jbuf_append(&wrap, "}");
    free(cat);
    jbuf_t schema;
    jbuf_init(&schema, 256);
    tool_schema_ctx_t ctx = {.want = method, .schema = &schema, .found = 0};
    json_array_foreach(wrap.data, "__tm_items__", tool_schema_cb, &ctx);
    jbuf_free(&wrap);
    if (!ctx.found) {
        jbuf_free(&schema);
        return NULL;
    }
    return schema.data;
}

static const connector_vtable_t TOOL_VT = {
    .kind = "tool",
    .description = "Tool Management API over HTTP — discover/execute remote tools",
    .capabilities = CONN_CAP_INVOKE,
    .osi_layers = OSI_L7_APPLICATION | OSI_L6_PRESENTATION,
    .open = tool_open,
    .invoke = tool_invoke,
    .stream = NULL,
    .describe = tool_describe,
    .schema = tool_schema,
    .close = tool_close,
};

/* ── Built-in adapter: "shell" (local process, non-HTTP transport) ────────
 * Proves the seam is transport-agnostic and exercises streaming + actuation.
 * method is advisory; params.cmd is run via /bin/sh -c, params.stdin is fed in.
 * invoke returns {"exit":N,"stdout":"…"}; stream emits stdout line-by-line. */
typedef struct {
    char *cwd;
} shell_conn_t;

static void *shell_open(const char *config_json, char *err, size_t errlen) {
    (void)err;
    (void)errlen;
    shell_conn_t *sc = safe_malloc(sizeof(*sc));
    sc->cwd = (config_json && config_json[0]) ? json_get_str(config_json, "cwd") : NULL;
    return sc;
}

/* Spawn `cmd`, capture stdout (+stderr). If on_line != NULL, deliver each line
 * as it arrives. Always accumulates full output into *out. Returns exit code,
 * or -1 on spawn failure. */
static int shell_run(const char *cwd, const char *cmd, const char *stdin_data,
                     conn_chunk_cb on_line, void *ctx, char **out) {
    int outfd[2], infd[2];
    if (pipe(outfd) != 0)
        return -1;
    if (pipe(infd) != 0) {
        close(outfd[0]);
        close(outfd[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(outfd[0]);
        close(outfd[1]);
        close(infd[0]);
        close(infd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(infd[0], STDIN_FILENO);
        dup2(outfd[1], STDOUT_FILENO);
        dup2(outfd[1], STDERR_FILENO);
        close(infd[0]);
        close(infd[1]);
        close(outfd[0]);
        close(outfd[1]);
        if (cwd && cwd[0] && chdir(cwd) != 0)
            _exit(126);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(infd[0]);
    close(outfd[1]);
    if (stdin_data && stdin_data[0]) {
        size_t n = strlen(stdin_data);
        ssize_t w = write(infd[1], stdin_data, n);
        (void)w;
    }
    close(infd[1]);
    jbuf_t acc;
    jbuf_init(&acc, 1024);
    jbuf_t line;
    jbuf_init(&line, 256);
    char buf[4096];
    ssize_t r;
    bool stop = false;
    while ((r = read(outfd[0], buf, sizeof(buf))) > 0) {
        jbuf_append_len(&acc, buf, (size_t)r);
        if (on_line && !stop) {
            for (ssize_t i = 0; i < r; i++) {
                if (buf[i] == '\n') {
                    if (!on_line(line.data ? line.data : "", ctx)) {
                        stop = true;
                    }
                    jbuf_reset(&line);
                    if (stop)
                        break;
                } else
                    jbuf_append_char(&line, buf[i]);
            }
        }
    }
    if (on_line && !stop && line.len > 0)
        on_line(line.data, ctx);
    close(outfd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    jbuf_free(&line);
    if (out)
        *out = acc.data;
    else
        jbuf_free(&acc);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void shell_invoke(void *self, const char *method, const char *params, conn_result_t *out) {
    (void)method;
    shell_conn_t *sc = self;
    char *cmd = params ? json_get_str(params, "cmd") : NULL;
    if (!cmd || !cmd[0]) {
        free(cmd);
        if (out)
            snprintf(out->error, sizeof(out->error), "shell: params.cmd required");
        return;
    }
    char *sin = params ? json_get_str(params, "stdin") : NULL;
    char *raw = NULL;
    int code = shell_run(sc->cwd, cmd, sin, NULL, NULL, &raw);
    free(cmd);
    free(sin);
    if (!out) {
        free(raw);
        return;
    }
    if (code < 0) {
        out->status = -1;
        snprintf(out->error, sizeof(out->error), "shell: spawn failed");
        free(raw);
        return;
    }
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{\"exit\":");
    jbuf_append_int(&b, code);
    jbuf_append(&b, ",\"stdout\":");
    jbuf_append_json_str(&b, raw ? raw : "");
    jbuf_append(&b, "}");
    free(raw);
    out->status = (code == 0) ? 0 : 1;
    out->body = b.data;
}

static void shell_stream(void *self, const char *method, const char *params, conn_chunk_cb on_chunk,
                         void *ctx, conn_result_t *out) {
    (void)method;
    shell_conn_t *sc = self;
    char *cmd = params ? json_get_str(params, "cmd") : NULL;
    if (!cmd || !cmd[0]) {
        free(cmd);
        if (out)
            snprintf(out->error, sizeof(out->error), "shell: params.cmd required");
        return;
    }
    char *sin = params ? json_get_str(params, "stdin") : NULL;
    char *raw = NULL;
    int code = shell_run(sc->cwd, cmd, sin, on_chunk, ctx, &raw);
    free(cmd);
    free(sin);
    free(raw);
    if (!out)
        return;
    if (code < 0) {
        out->status = -1;
        snprintf(out->error, sizeof(out->error), "shell: spawn failed");
        return;
    }
    out->status = (code == 0) ? 0 : 1;
    out->body = NULL;
}

static char *shell_describe(void *self) {
    (void)self;
    return safe_strdup("{\"kind\":\"shell\",\"methods\":[\"run\"],"
                       "\"params\":{\"cmd\":\"string (required)\",\"stdin\":\"string (optional)\"},"
                       "\"returns\":{\"exit\":\"int\",\"stdout\":\"string\"}}");
}

static void shell_close(void *self) {
    shell_conn_t *sc = self;
    if (sc) {
        free(sc->cwd);
        free(sc);
    }
}

static const connector_vtable_t SHELL_VT = {
    .kind = "shell",
    .description = "Local process over /bin/sh — non-HTTP transport, streams stdout",
    .capabilities = CONN_CAP_INVOKE | CONN_CAP_STREAM | CONN_CAP_ACTUATE,
    .osi_layers = OSI_L7_APPLICATION,
    .open = shell_open,
    .invoke = shell_invoke,
    .stream = shell_stream,
    .describe = shell_describe,
    .close = shell_close,
};

/* ── Built-in adapter: "flow" (recursive hierarchical feedback loops) ─────
 * A pipeline is itself a connector. Its config is a spec:
 *   {"steps":[{"kind":…,"method":…,"params":{…}}, …],
 *    "loop":{"max":N,"until_stable":true}}
 * Each step's params may contain feedback placeholders ("{{prev}}", "{{stepN}}",
 * "{{seed}}", "{{prev:key}}") substituted with prior results. Because "flow" is
 * itself a registered kind, a step's kind can be "flow" → flows nest inside
 * flows (hierarchy + recursion). The optional loop re-runs the whole sequence,
 * feeding the final body back as the seed, until the output stabilizes or max
 * iterations is reached (feedback). The seed for iteration 1 is invoke()'s
 * params, so a flow used as a step receives its parent's threaded output. */
typedef struct {
    char *spec;
} flow_conn_t;

typedef struct {
    char **out;
    int n;
    const char *seed;
    int rc;
    char err[256];
} flow_run_t;

static void *flow_open(const char *config_json, char *err, size_t errlen) {
    if (!config_json || !config_json[0]) {
        snprintf(err, errlen, "flow: config spec required (--config '{\"steps\":[…]}')");
        return NULL;
    }
    flow_conn_t *fc = safe_malloc(sizeof(*fc));
    fc->spec = safe_strdup(config_json);
    return fc;
}

/* One pass over the steps. Threads outputs; on first failed step sets rc and
 * stops. Returns a malloc'd copy of the final step's body (or NULL). */
static char *flow_pass(const char *spec, const char *seed, int *rc, char *errbuf, size_t errlen,
                       int trace, int iter) {
    char *steps = json_get_raw(spec, "steps");
    *rc = 0;
    if (!steps) {
        snprintf(errbuf, errlen, "flow: spec has no steps[]");
        *rc = 2;
        return NULL;
    }
    /* Iterate the array via a wrapper key (json_array_foreach needs a key). */
    jbuf_t wrap;
    jbuf_init(&wrap, strlen(steps) + 32);
    jbuf_append(&wrap, "{\"__steps__\":");
    jbuf_append(&wrap, steps);
    jbuf_append(&wrap, "}");
    free(steps);

    /* Collect step element pointers by scanning the array ourselves so we can
     * run them sequentially with threading (foreach callback can't easily carry
     * sequential state across heterogeneous opens). */
    char *outputs[64];
    int nout = 0;
    char *final = NULL;
    const char *arr = json_get_raw(wrap.data, "__steps__");
    /* `arr` begins at '['. Walk top-level object elements. */
    const char *p = arr ? strchr(arr, '[') : NULL;
    if (p)
        p++;
    int depth = 0;
    while (p && *p) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t')
            p++;
        if (*p == ']' || *p == '\0')
            break;
        if (*p != '{') {
            p++;
            continue;
        }
        /* find matching close brace */
        const char *start = p;
        depth = 0;
        bool instr = false;
        for (; *p; p++) {
            if (instr) {
                if (*p == '\\')
                    p++;
                else if (*p == '"')
                    instr = false;
                continue;
            }
            if (*p == '"')
                instr = true;
            else if (*p == '{')
                depth++;
            else if (*p == '}') {
                depth--;
                if (depth == 0) {
                    p++;
                    break;
                }
            }
        }
        size_t elen = (size_t)(p - start);
        char *elem = safe_malloc(elen + 1);
        memcpy(elem, start, elen);
        elem[elen] = '\0';

        char *kind = json_get_str(elem, "kind");
        char *method = json_get_str(elem, "method");
        char *praw = json_get_raw(elem, "params");
        char *craw = json_get_raw(elem, "config");
        char *params = conn_subst(praw ? praw : "{}", outputs, nout, seed);
        /* Per-step config (also feedback-substituted) lets a step be a nested
         * flow, a chain ledger, or any configured kind — not just default-open. */
        char *config = craw ? conn_subst(craw, outputs, nout, seed) : NULL;
        free(praw);
        free(craw);
        free(elem);

        char oerr[256];
        connector_t *c = connector_open(kind ? kind : "", config, oerr, sizeof(oerr));
        if (!c) {
            snprintf(errbuf, errlen, "%s", oerr);
            *rc = 1;
            free(kind);
            free(method);
            free(params);
            free(config);
            break;
        }
        conn_result_t res;
        connector_invoke(c, method, params, &res);
        connector_close(c);
        if (trace)
            fprintf(stderr, "[flow] iter %d step %d %s/%s status=%ld\n", iter, nout + 1,
                    kind ? kind : "?", method ? method : "?", res.status);
        free(kind);
        free(method);
        free(params);
        free(config);

        if (res.status < 0) {
            snprintf(errbuf, errlen, "%s", res.error[0] ? res.error : "step transport error");
            *rc = 1;
            conn_result_free(&res);
            break;
        }
        char *body = res.body ? res.body : safe_strdup("");
        res.body = NULL;
        conn_result_free(&res);
        if (nout < (int)(sizeof(outputs) / sizeof(outputs[0])))
            outputs[nout++] = body;
        else
            free(body);
        if (*rc != 0)
            break;
    }
    if (nout > 0)
        final = safe_strdup(outputs[nout - 1]);
    for (int i = 0; i < nout; i++)
        free(outputs[i]);
    jbuf_free(&wrap);
    return final;
}

static void flow_invoke(void *self, const char *method, const char *params, conn_result_t *out) {
    (void)method;
    flow_conn_t *fc = self;
    int trace = getenv("DSCO_CONNECT_TRACE") ? 1 : 0;

    int max = json_get_int(fc->spec, "loop.max", 0); /* dotted not supported → 0 */
    char *loop = json_get_raw(fc->spec, "loop");
    bool until_stable = false;
    if (loop) {
        max = json_get_int(loop, "max", max > 0 ? max : 1);
        until_stable = json_get_bool(loop, "until_stable", false);
        free(loop);
    }
    if (max < 1)
        max = 1;

    char *seed = params ? safe_strdup(params) : safe_strdup("{}");
    char *result = NULL;
    int rc = 0;
    char err[256] = {0};
    int iter = 0;
    for (iter = 1; iter <= max; iter++) {
        char *prev = result;
        result = flow_pass(fc->spec, seed, &rc, err, sizeof(err), trace, iter);
        if (rc != 0) {
            free(prev);
            break;
        }
        /* feedback: the final body becomes the next iteration's seed */
        free(seed);
        seed = result ? safe_strdup(result) : safe_strdup("{}");
        bool stable = (prev && result && strcmp(prev, result) == 0);
        free(prev);
        if (until_stable && stable) {
            if (trace)
                fprintf(stderr, "[flow] stable at iter %d\n", iter);
            break;
        }
    }
    free(seed);

    if (!out) {
        free(result);
        return;
    }
    if (rc != 0) {
        out->status = (rc == 2) ? 1 : 1;
        snprintf(out->error, sizeof(out->error), "%s", err[0] ? err : "flow failed");
        free(result);
        return;
    }
    out->status = 0;
    out->body = result;
}

static char *flow_describe(void *self) {
    flow_conn_t *fc = self;
    return safe_strdup(fc->spec);
}

static void flow_close(void *self) {
    flow_conn_t *fc = self;
    if (fc) {
        free(fc->spec);
        free(fc);
    }
}

static const connector_vtable_t FLOW_VT = {
    .kind = "flow",
    .description = "Recursive hierarchical feedback pipeline — nests flows, loops to convergence",
    .capabilities = CONN_CAP_INVOKE | CONN_CAP_SUBSCRIBE,
    .osi_layers = OSI_L7_APPLICATION, /* orchestrates across all underlying layers */
    .open = flow_open,
    .invoke = flow_invoke,
    .stream = NULL,
    .describe = flow_describe,
    .close = flow_close,
};

/* ── Built-in adapter: "net" (raw sockets — L3/L4/L5, sub-application) ─────
 * Proves the seam reaches below the application layer. config selects the
 * endpoint: {"proto":"tcp|udp|unix","host":…,"port":…,"path":…,"timeout_ms":…}.
 * invoke sends params.data bytes and reads the reply; stream emits each recv.
 * Returns {"sent":N,"received":M,"data":"…"} (text payloads). */
typedef struct {
    int proto; /* 0 tcp, 1 udp, 2 unix */
    char *host, *path;
    int port, timeout_ms;
} net_conn_t;

static void *net_open(const char *config_json, char *err, size_t errlen) {
    net_conn_t *nc = safe_malloc(sizeof(*nc));
    nc->proto = 0;
    nc->host = NULL;
    nc->path = NULL;
    nc->port = 0;
    nc->timeout_ms = 3000;
    if (config_json && config_json[0]) {
        char *proto = json_get_str(config_json, "proto");
        if (proto) {
            if (strcmp(proto, "udp") == 0)
                nc->proto = 1;
            else if (strcmp(proto, "unix") == 0)
                nc->proto = 2;
            free(proto);
        }
        nc->host = json_get_str(config_json, "host");
        nc->path = json_get_str(config_json, "path");
        nc->port = json_get_int(config_json, "port", 0);
        nc->timeout_ms = json_get_int(config_json, "timeout_ms", 3000);
    }
    if (nc->proto == 2 && (!nc->path || !nc->path[0])) {
        snprintf(err, errlen, "net: unix proto requires config.path");
        free(nc->host);
        free(nc->path);
        free(nc);
        return NULL;
    }
    if (nc->proto != 2 && (!nc->host || !nc->host[0] || nc->port <= 0)) {
        snprintf(err, errlen, "net: tcp/udp require config.host and config.port");
        free(nc->host);
        free(nc->path);
        free(nc);
        return NULL;
    }
    return nc;
}

static int net_connect(net_conn_t *nc, char *err, size_t errlen) {
    if (nc->proto == 2) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            snprintf(err, errlen, "net: socket: %s", strerror(errno));
            return -1;
        }
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", nc->path);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            snprintf(err, errlen, "net: connect %s: %s", nc->path, strerror(errno));
            close(fd);
            return -1;
        }
        return fd;
    }
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", nc->port);
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = (nc->proto == 1) ? SOCK_DGRAM : SOCK_STREAM;
    int gai = getaddrinfo(nc->host, portstr, &hints, &ai);
    if (gai != 0) {
        snprintf(err, errlen, "net: resolve %s: %s", nc->host, gai_strerror(gai));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *p = ai; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0)
        snprintf(err, errlen, "net: connect %s:%d failed", nc->host, nc->port);
    return fd;
}

/* Send data, read reply. If on_chunk != NULL deliver each recv as a chunk;
 * always accumulate into *recv_out. Returns 0 ok, -1 error. */
static int net_io(net_conn_t *nc, const char *data, size_t dlen, conn_chunk_cb on_chunk, void *ctx,
                  size_t *sent_out, char **recv_out, char *err, size_t errlen) {
    int fd = net_connect(nc, err, errlen);
    if (fd < 0)
        return -1;
    struct timeval tv = {nc->timeout_ms / 1000, (nc->timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    size_t sent = 0;
    if (data && dlen) {
        ssize_t w = send(fd, data, dlen, 0);
        if (w > 0)
            sent = (size_t)w;
    }
    jbuf_t acc;
    jbuf_init(&acc, 1024);
    char buf[4096];
    ssize_t r;
    bool stop = false;
    while ((r = recv(fd, buf, sizeof(buf), 0)) > 0) {
        jbuf_append_len(&acc, buf, (size_t)r);
        if (on_chunk && !stop) {
            buf[r < (ssize_t)sizeof(buf) ? r : (ssize_t)sizeof(buf) - 1] = '\0';
            if (!on_chunk(buf, ctx))
                stop = true;
        }
        if (nc->proto == 1)
            break; /* one datagram for udp */
        if (stop)
            break;
    }
    close(fd);
    if (sent_out)
        *sent_out = sent;
    if (recv_out)
        *recv_out = acc.data;
    else
        jbuf_free(&acc);
    return 0;
}

static void net_invoke(void *self, const char *method, const char *params, conn_result_t *out) {
    (void)method;
    net_conn_t *nc = self;
    char *data = params ? json_get_str(params, "data") : NULL;
    char err[256] = {0};
    size_t sent = 0;
    char *recv_buf = NULL;
    int rc =
        net_io(nc, data, data ? strlen(data) : 0, NULL, NULL, &sent, &recv_buf, err, sizeof(err));
    free(data);
    if (!out) {
        free(recv_buf);
        return;
    }
    if (rc != 0) {
        out->status = -1;
        snprintf(out->error, sizeof(out->error), "%s", err);
        free(recv_buf);
        return;
    }
    jbuf_t b;
    jbuf_init(&b, 512);
    jbuf_append(&b, "{\"sent\":");
    jbuf_append_int(&b, (int)sent);
    jbuf_append(&b, ",\"received\":");
    jbuf_append_int(&b, (int)(recv_buf ? strlen(recv_buf) : 0));
    jbuf_append(&b, ",\"data\":");
    jbuf_append_json_str(&b, recv_buf ? recv_buf : "");
    jbuf_append(&b, "}");
    free(recv_buf);
    out->status = 0;
    out->body = b.data;
}

static void net_stream(void *self, const char *method, const char *params, conn_chunk_cb on_chunk,
                       void *ctx, conn_result_t *out) {
    (void)method;
    net_conn_t *nc = self;
    char *data = params ? json_get_str(params, "data") : NULL;
    char err[256] = {0};
    size_t sent = 0;
    int rc =
        net_io(nc, data, data ? strlen(data) : 0, on_chunk, ctx, &sent, NULL, err, sizeof(err));
    free(data);
    if (!out)
        return;
    if (rc != 0) {
        out->status = -1;
        snprintf(out->error, sizeof(out->error), "%s", err);
        return;
    }
    out->status = 0;
    out->body = NULL;
}

static char *net_describe(void *self) {
    net_conn_t *nc = self;
    const char *proto = nc->proto == 1 ? "udp" : nc->proto == 2 ? "unix" : "tcp";
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"kind\":\"net\",\"proto\":");
    jbuf_append_json_str(&b, proto);
    jbuf_append(&b, ",\"osi\":[\"L3\",\"L4\",\"L5\"],");
    jbuf_append(&b, "\"params\":{\"data\":\"string\"},");
    jbuf_append(&b, "\"returns\":{\"sent\":\"int\",\"received\":\"int\",\"data\":\"string\"}}");
    return b.data;
}

static void net_close(void *self) {
    net_conn_t *nc = self;
    if (nc) {
        free(nc->host);
        free(nc->path);
        free(nc);
    }
}

static const connector_vtable_t NET_VT = {
    .kind = "net",
    .description = "Raw TCP/UDP/Unix sockets — sub-application transport (L3/L4/L5)",
    .capabilities = CONN_CAP_INVOKE | CONN_CAP_STREAM | CONN_CAP_SENSE | CONN_CAP_ACTUATE,
    .osi_layers = OSI_L3_NETWORK | OSI_L4_TRANSPORT | OSI_L5_SESSION,
    .open = net_open,
    .invoke = net_invoke,
    .stream = net_stream,
    .describe = net_describe,
    .close = net_close,
};

/* ── Built-in adapter: "serial" (termios device I/O — L1/L2 physical) ──────
 * Reaches the physical layer: config {"device":"/dev/tty…","baud":N,
 * "timeout_ms":N}. invoke writes params.data and reads the reply. */
typedef struct {
    char *device;
    int baud, timeout_ms;
} serial_conn_t;

static speed_t serial_speed(int baud) {
    switch (baud) {
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
        case 230400:
            return B230400;
        default:
            return B115200;
    }
}

static void *serial_open(const char *config_json, char *err, size_t errlen) {
    char *dev = (config_json && config_json[0]) ? json_get_str(config_json, "device") : NULL;
    if (!dev || !dev[0]) {
        snprintf(err, errlen, "serial: config.device required (e.g. /dev/tty.usbserial)");
        free(dev);
        return NULL;
    }
    serial_conn_t *sc = safe_malloc(sizeof(*sc));
    sc->device = dev;
    sc->baud = json_get_int(config_json, "baud", 115200);
    sc->timeout_ms = json_get_int(config_json, "timeout_ms", 2000);
    return sc;
}

static int serial_fd_open(serial_conn_t *sc, char *err, size_t errlen) {
    int fd = open(sc->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(err, errlen, "serial: open %s: %s", sc->device, strerror(errno));
        return -1;
    }
    struct termios t;
    if (tcgetattr(fd, &t) != 0) {
        snprintf(err, errlen, "serial: tcgetattr: %s", strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&t);
    speed_t sp = serial_speed(sc->baud);
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    t.c_cflag |= (CLOCAL | CREAD);
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        snprintf(err, errlen, "serial: tcsetattr: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void serial_invoke(void *self, const char *method, const char *params, conn_result_t *out) {
    (void)method;
    serial_conn_t *sc = self;
    char err[256] = {0};
    int fd = serial_fd_open(sc, err, sizeof(err));
    if (fd < 0) {
        if (out) {
            out->status = -1;
            snprintf(out->error, sizeof(out->error), "%s", err);
        }
        return;
    }
    char *data = params ? json_get_str(params, "data") : NULL;
    size_t sent = 0;
    if (data && data[0]) {
        ssize_t w = write(fd, data, strlen(data));
        if (w > 0)
            sent = (size_t)w;
    }
    free(data);
    jbuf_t acc;
    jbuf_init(&acc, 256);
    long deadline = conn_now_ms() + sc->timeout_ms;
    char buf[512];
    while (conn_now_ms() < deadline) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r > 0)
            jbuf_append_len(&acc, buf, (size_t)r);
        else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct timespec ts = {0, 20 * 1000000L};
            nanosleep(&ts, NULL);
        } else
            break;
    }
    close(fd);
    if (!out) {
        jbuf_free(&acc);
        return;
    }
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"sent\":");
    jbuf_append_int(&b, (int)sent);
    jbuf_append(&b, ",\"data\":");
    jbuf_append_json_str(&b, acc.data ? acc.data : "");
    jbuf_append(&b, "}");
    jbuf_free(&acc);
    out->status = 0;
    out->body = b.data;
}

static char *serial_describe(void *self) {
    serial_conn_t *sc = self;
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"kind\":\"serial\",\"device\":");
    jbuf_append_json_str(&b, sc->device);
    jbuf_append(&b, ",\"baud\":");
    jbuf_append_int(&b, sc->baud);
    jbuf_append(&b, ",\"osi\":[\"L1\",\"L2\"],\"params\":{\"data\":\"string\"}}");
    return b.data;
}

static void serial_close(void *self) {
    serial_conn_t *sc = self;
    if (sc) {
        free(sc->device);
        free(sc);
    }
}

static const connector_vtable_t SERIAL_VT = {
    .kind = "serial",
    .description = "Serial/termios device I/O — physical & data-link layers (L1/L2)",
    .capabilities = CONN_CAP_INVOKE | CONN_CAP_SENSE | CONN_CAP_ACTUATE,
    .osi_layers = OSI_L1_PHYSICAL | OSI_L2_DATALINK,
    .open = serial_open,
    .invoke = serial_invoke,
    .stream = NULL,
    .describe = serial_describe,
    .close = serial_close,
};

/* ── Built-in adapter: "agent" (persona seam) ─────────────────────────────
 * A named actor in a multi-agent workflow. Today it is a deterministic
 * decision function — it composes a role-tagged decision object from the
 * action (method) and the threaded params — and is the exact seam where an
 * LLM-, model- or human-backed agent plugs in later without touching callers.
 * Threading the whole body via {{stepN}} hands one agent's decision to the
 * next, so a `flow` of agents is a desk and a flow-of-flows is a firm.
 *   config:  {"name":"…","role":"…","firm":"…"}
 *   invoke:  method=<action>, params=<decision payload>
 *     -> {"agent","role","firm","action","decision":<params>} */
/* crd/individual are the human-backed seam: when a persona is grounded to a
 * real FINRA-registered individual (config {"crd":…,"individual":"…"}), every
 * trajectory decision it emits carries that verifiable individualId. */
typedef struct {
    char *name, *role, *firm, *crd, *individual;
} agent_conn_t;

static void *agent_open(const char *config_json, char *err, size_t errlen) {
    (void)err;
    (void)errlen;
    agent_conn_t *a = safe_malloc(sizeof(*a));
    int have = config_json && config_json[0];
    a->name = have ? json_get_str(config_json, "name") : NULL;
    a->role = have ? json_get_str(config_json, "role") : NULL;
    a->firm = have ? json_get_str(config_json, "firm") : NULL;
    a->crd = have ? json_get_str(config_json, "crd") : NULL;
    a->individual = have ? json_get_str(config_json, "individual") : NULL;
    if (!a->name)
        a->name = safe_strdup("agent");
    if (!a->role)
        a->role = safe_strdup("");
    if (!a->firm)
        a->firm = safe_strdup("");
    return a;
}

static void agent_invoke(void *self, const char *method, const char *params, conn_result_t *out) {
    agent_conn_t *a = self;
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"agent\":");
    jbuf_append_json_str(&b, a->name);
    jbuf_append(&b, ",\"role\":");
    jbuf_append_json_str(&b, a->role);
    jbuf_append(&b, ",\"firm\":");
    jbuf_append_json_str(&b, a->firm);
    if (a->crd && a->crd[0]) {
        jbuf_append(&b, ",\"crd\":");
        jbuf_append_json_str(&b, a->crd);
    }
    if (a->individual && a->individual[0]) {
        jbuf_append(&b, ",\"individual\":");
        jbuf_append_json_str(&b, a->individual);
    }
    jbuf_append(&b, ",\"action\":");
    jbuf_append_json_str(&b, method ? method : "");
    jbuf_append(&b, ",\"decision\":");
    jbuf_append(&b, (params && params[0]) ? params : "{}");
    jbuf_append(&b, "}");
    if (out) {
        out->status = 0;
        out->body = b.data;
    } else
        jbuf_free(&b);
}

static char *agent_describe(void *self) {
    agent_conn_t *a = self;
    jbuf_t b;
    jbuf_init(&b, 160);
    jbuf_append(&b, "{\"kind\":\"agent\",\"name\":");
    jbuf_append_json_str(&b, a->name);
    jbuf_append(&b, ",\"role\":");
    jbuf_append_json_str(&b, a->role);
    jbuf_append(&b, ",\"firm\":");
    jbuf_append_json_str(&b, a->firm);
    if (a->crd && a->crd[0]) {
        jbuf_append(&b, ",\"crd\":");
        jbuf_append_json_str(&b, a->crd);
    }
    if (a->individual && a->individual[0]) {
        jbuf_append(&b, ",\"individual\":");
        jbuf_append_json_str(&b, a->individual);
    }
    jbuf_append(&b, "}");
    return b.data;
}

static void agent_close(void *self) {
    agent_conn_t *a = self;
    if (a) {
        free(a->name);
        free(a->role);
        free(a->firm);
        free(a->crd);
        free(a->individual);
        free(a);
    }
}

static const connector_vtable_t AGENT_VT = {
    .kind = "agent",
    .description = "Agent persona seam — composes a role-tagged decision (LLM-backed later)",
    .capabilities = CONN_CAP_INVOKE,
    .osi_layers = OSI_L7_APPLICATION,
    .open = agent_open,
    .invoke = agent_invoke,
    .stream = NULL,
    .describe = agent_describe,
    .schema = NULL,
    .close = agent_close,
};

/* ── Built-in adapter: "chain" (hash-chained append-only ledger) ──────────
 * The concrete embodiment of CONN_CAP_TX: the seam every future blockchain,
 * credit/settlement rail or signed-event log plugs into. Records are linked
 * by sha256(prev_hash || data), so any tampering breaks `verify`. File-backed
 * for durability; methods: append/head/get/verify.
 *   config:  {"path":"<ledger file>"}        (default ".dsco_chain.log")
 *   append:  {"data": <any json>} -> {"index","prev","hash"}
 *   head:    {} -> {"index","hash"}            (index -1 when empty)
 *   get:     {"index": N} -> the record line
 *   verify:  {} -> {"ok":true,"length":N} | {"ok":false,"broken":i} */
typedef struct {
    char *path;
    long count;
    char last[65];
} chain_conn_t;

static const char CHAIN_ZERO[65] =
    "0000000000000000000000000000000000000000000000000000000000000000";

/* Minify JSON: drop insignificant whitespace outside strings so every ledger
 * record occupies exactly one physical line (the storage format is line-
 * delimited). Caller frees. */
static char *chain_compact(const char *s) {
    if (!s)
        return safe_strdup("null");
    jbuf_t b;
    jbuf_init(&b, strlen(s) + 1);
    bool instr = false;
    for (const char *p = s; *p; p++) {
        if (instr) {
            jbuf_append_char(&b, *p);
            if (*p == '\\' && p[1]) {
                jbuf_append_char(&b, p[1]);
                p++;
            } else if (*p == '"')
                instr = false;
            continue;
        }
        if (*p == '"') {
            instr = true;
            jbuf_append_char(&b, *p);
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            continue;
        jbuf_append_char(&b, *p);
    }
    return b.data;
}

/* Recompute a record's hash from its predecessor and data slice. */
static void chain_hash(const char *prev, const char *data, char out[65]) {
    jbuf_t b;
    jbuf_init(&b, (prev ? 64 : 0) + (data ? strlen(data) : 0) + 8);
    jbuf_append(&b, prev ? prev : CHAIN_ZERO);
    jbuf_append(&b, "\n");
    jbuf_append(&b, data ? data : "null");
    sha256_hex((const uint8_t *)b.data, b.data ? strlen(b.data) : 0, out);
    jbuf_free(&b);
}

/* Scan the ledger to recover count + last hash (also the verify workhorse).
 * When verify!=0, returns 1 if intact else 0 and writes the first bad index
 * to *broken; otherwise always returns 1. */
static int chain_scan(const char *path, long *count, char last[65], int verify, long *broken) {
    *count = 0;
    snprintf(last, 65, "%s", CHAIN_ZERO);
    FILE *f = fopen(path, "r");
    if (!f)
        return 1; /* absent ledger == empty, intact */
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    char prev[65];
    snprintf(prev, sizeof(prev), "%s", CHAIN_ZERO);
    long idx = 0;
    int ok = 1;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (n && line[n - 1] == '\n')
            line[n - 1] = '\0';
        if (!line[0])
            continue;
        char *data = json_get_raw(line, "data");
        char *hash = json_get_str(line, "hash");
        char *rprev = json_get_str(line, "prev");
        if (verify) {
            char calc[65];
            chain_hash(prev, data, calc);
            if (!hash || strcmp(hash, calc) != 0 || !rprev || strcmp(rprev, prev) != 0) {
                ok = 0;
                if (broken)
                    *broken = idx;
                free(data);
                free(hash);
                free(rprev);
                break;
            }
        }
        if (hash)
            snprintf(prev, sizeof(prev), "%s", hash);
        free(data);
        free(hash);
        free(rprev);
        idx++;
    }
    free(line);
    fclose(f);
    if (ok) {
        *count = idx;
        snprintf(last, 65, "%s", prev);
    }
    return ok;
}

static void *chain_open(const char *config_json, char *err, size_t errlen) {
    (void)err;
    (void)errlen;
    chain_conn_t *cc = safe_malloc(sizeof(*cc));
    char *p = (config_json && config_json[0]) ? json_get_str(config_json, "path") : NULL;
    cc->path = p ? p : safe_strdup(".dsco_chain.log");
    chain_scan(cc->path, &cc->count, cc->last, 0, NULL);
    return cc;
}

static void chain_invoke(void *self, const char *method, const char *params, conn_result_t *out) {
    chain_conn_t *cc = self;
    const char *m = method ? method : "head";

    if (strcmp(m, "append") == 0) {
        char *raw = params ? json_get_raw(params, "data") : NULL;
        if (!raw) {
            if (out) {
                out->status = 1;
                snprintf(out->error, sizeof(out->error), "append: 'data' required");
            }
            return;
        }
        char *data = chain_compact(raw); /* one record == one physical line */
        free(raw);
        char hash[65];
        chain_hash(cc->last, data, hash);
        FILE *f = fopen(cc->path, "a");
        if (!f) {
            if (out) {
                out->status = -1;
                snprintf(out->error, sizeof(out->error), "open %s: %s", cc->path, strerror(errno));
            }
            free(data);
            return;
        }
        fprintf(f, "{\"index\":%ld,\"prev\":\"%s\",\"data\":%s,\"hash\":\"%s\"}\n", cc->count,
                cc->last, data, hash);
        fclose(f);
        jbuf_t b;
        jbuf_init(&b, 160);
        jbuf_append(&b, "{\"index\":");
        jbuf_append_int(&b, cc->count);
        jbuf_append(&b, ",\"prev\":\"");
        jbuf_append(&b, cc->last);
        jbuf_append(&b, "\",\"hash\":\"");
        jbuf_append(&b, hash);
        jbuf_append(&b, "\"}");
        cc->count++;
        snprintf(cc->last, sizeof(cc->last), "%s", hash);
        free(data);
        if (out) {
            out->status = 0;
            out->body = b.data;
        } else
            jbuf_free(&b);
        return;
    }

    if (strcmp(m, "head") == 0) {
        jbuf_t b;
        jbuf_init(&b, 96);
        jbuf_append(&b, "{\"index\":");
        jbuf_append_int(&b, cc->count - 1);
        jbuf_append(&b, ",\"hash\":\"");
        jbuf_append(&b, cc->count > 0 ? cc->last : CHAIN_ZERO);
        jbuf_append(&b, "\"}");
        if (out) {
            out->status = 0;
            out->body = b.data;
        } else
            jbuf_free(&b);
        return;
    }

    if (strcmp(m, "verify") == 0) {
        long broken = -1, count = 0;
        char last[65];
        int ok = chain_scan(cc->path, &count, last, 1, &broken);
        jbuf_t b;
        jbuf_init(&b, 64);
        if (ok) {
            jbuf_append(&b, "{\"ok\":true,\"length\":");
            jbuf_append_int(&b, count);
            jbuf_append(&b, "}");
        } else {
            jbuf_append(&b, "{\"ok\":false,\"broken\":");
            jbuf_append_int(&b, broken);
            jbuf_append(&b, "}");
        }
        if (out) {
            out->status = ok ? 0 : 1;
            out->body = b.data;
        } else
            jbuf_free(&b);
        return;
    }

    if (strcmp(m, "get") == 0) {
        long want = params ? json_get_int(params, "index", -1) : -1;
        FILE *f = fopen(cc->path, "r");
        if (!f) {
            if (out) {
                out->status = 1;
                snprintf(out->error, sizeof(out->error), "get: empty ledger");
            }
            return;
        }
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;
        long i = 0;
        char *hit = NULL;
        while ((n = getline(&line, &cap, f)) > 0) {
            if (n && line[n - 1] == '\n')
                line[n - 1] = '\0';
            if (!line[0])
                continue;
            if (i == want) {
                hit = safe_strdup(line);
                break;
            }
            i++;
        }
        free(line);
        fclose(f);
        if (out) {
            if (hit) {
                out->status = 0;
                out->body = hit;
            } else {
                out->status = 1;
                snprintf(out->error, sizeof(out->error), "get: no index %ld", want);
            }
        } else
            free(hit);
        return;
    }

    if (out) {
        out->status = 1;
        snprintf(out->error, sizeof(out->error), "chain: unknown method '%s'", m);
    }
}

static char *chain_describe(void *self) {
    chain_conn_t *cc = self;
    jbuf_t b;
    jbuf_init(&b, 160);
    jbuf_append(&b, "{\"kind\":\"chain\",\"path\":");
    jbuf_append_json_str(&b, cc->path);
    jbuf_append(&b, ",\"length\":");
    jbuf_append_int(&b, cc->count);
    jbuf_append(&b, ",\"head\":\"");
    jbuf_append(&b, cc->count > 0 ? cc->last : CHAIN_ZERO);
    jbuf_append(&b, "\",\"methods\":[\"append\",\"head\",\"get\",\"verify\"]}");
    return b.data;
}

static char *chain_schema(void *self, const char *method) {
    (void)self;
    if (!method)
        return NULL;
    if (strcmp(method, "append") == 0)
        return safe_strdup("{\"type\":\"object\",\"required\":[\"data\"],"
                           "\"properties\":{\"data\":{}}}");
    if (strcmp(method, "get") == 0)
        return safe_strdup("{\"type\":\"object\",\"required\":[\"index\"],"
                           "\"properties\":{\"index\":{\"type\":\"integer\"}}}");
    return NULL;
}

static void chain_close(void *self) {
    chain_conn_t *cc = self;
    if (cc) {
        free(cc->path);
        free(cc);
    }
}

static const connector_vtable_t CHAIN_VT = {
    .kind = "chain",
    .description = "Hash-chained append-only ledger — transactional seam (CONN_CAP_TX)",
    .capabilities = CONN_CAP_INVOKE | CONN_CAP_TX,
    .osi_layers = OSI_L7_APPLICATION,
    .open = chain_open,
    .invoke = chain_invoke,
    .stream = NULL,
    .describe = chain_describe,
    .schema = chain_schema,
    .close = chain_close,
};

/* ── Built-in adapter: "sqlite" (read-only relational query seam) ─────────
 * Lets a flow persona pull live deal economics out of a SQLite database (the
 * collateral/deal book at data/cmo/cmo.db) and thread the rows into the next
 * step — e.g. a trader persona prices the pool the query returned, then the
 * decision is committed to the chain ledger. Opened READONLY: a query connector
 * is a read seam, not a mutation path, so the agent can never corrupt the book.
 *   config:  {"path":"<db file>"}             (default "data/cmo/cmo.db")
 *   query:   {"sql":"SELECT …","limit":N} -> {"columns":[…],"rows":[[…]],
 *                                              "count":N,"truncated":bool}
 *   tables:  {} -> {"tables":[{"name","type"}]}  (tables + views) */
typedef struct {
    sqlite3 *db;
    char *path;
} sqlite_conn_t;

/* Only statements that read are accepted; the handle is READONLY too, so this
 * is a guardrail for clearer errors rather than the sole defense. */
static bool sqlite_is_read_only(const char *sql) {
    while (*sql && isspace((unsigned char)*sql))
        sql++;
    return strncasecmp(sql, "SELECT", 6) == 0 || strncasecmp(sql, "WITH", 4) == 0 ||
           strncasecmp(sql, "PRAGMA", 6) == 0 || strncasecmp(sql, "EXPLAIN", 7) == 0;
}

/* Emit one column's value as JSON, typed from SQLite's dynamic column type. */
static void sqlite_emit_value(jbuf_t *b, sqlite3_stmt *st, int col) {
    switch (sqlite3_column_type(st, col)) {
        case SQLITE_INTEGER:
            jbuf_appendf(b, "%lld", (long long)sqlite3_column_int64(st, col));
            break;
        case SQLITE_FLOAT:
            jbuf_appendf(b, "%.10g", sqlite3_column_double(st, col));
            break;
        case SQLITE_NULL:
            jbuf_append(b, "null");
            break;
        default: {
            const char *t = (const char *)sqlite3_column_text(st, col);
            jbuf_append_json_str(b, t ? t : "");
        }
    }
}

static void *sqlite_open(const char *config_json, char *err, size_t errlen) {
    char *p = (config_json && config_json[0]) ? json_get_str(config_json, "path") : NULL;
    const char *path = p ? p : "data/cmo/cmo.db";
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        snprintf(err, errlen, "open %s: %s", path, db ? sqlite3_errmsg(db) : "out of memory");
        if (db)
            sqlite3_close(db);
        free(p);
        return NULL;
    }
    sqlite_conn_t *sc = safe_malloc(sizeof(*sc));
    sc->db = db;
    sc->path = p ? p : safe_strdup(path);
    return sc;
}

static void sqlite_invoke(void *self, const char *method, const char *params, conn_result_t *out) {
    sqlite_conn_t *sc = self;
    const char *m = method ? method : "query";

    if (strcmp(m, "tables") == 0) {
        sqlite3_stmt *st = NULL;
        const char *q = "SELECT name,type FROM sqlite_master "
                        "WHERE type IN ('table','view') ORDER BY name";
        if (sqlite3_prepare_v2(sc->db, q, -1, &st, NULL) != SQLITE_OK) {
            if (out) {
                out->status = 1;
                snprintf(out->error, sizeof(out->error), "tables: %s", sqlite3_errmsg(sc->db));
            }
            return;
        }
        jbuf_t b;
        jbuf_init(&b, 256);
        jbuf_append(&b, "{\"tables\":[");
        long n = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (n++)
                jbuf_append(&b, ",");
            jbuf_append(&b, "{\"name\":");
            jbuf_append_json_str(&b, (const char *)sqlite3_column_text(st, 0));
            jbuf_append(&b, ",\"type\":");
            jbuf_append_json_str(&b, (const char *)sqlite3_column_text(st, 1));
            jbuf_append(&b, "}");
        }
        sqlite3_finalize(st);
        jbuf_append(&b, "]}");
        if (out) {
            out->status = 0;
            out->body = b.data;
        } else
            jbuf_free(&b);
        return;
    }

    if (strcmp(m, "query") == 0) {
        char *sql = params ? json_get_str(params, "sql") : NULL;
        if (!sql) {
            if (out) {
                out->status = 1;
                snprintf(out->error, sizeof(out->error), "query: 'sql' required");
            }
            return;
        }
        if (!sqlite_is_read_only(sql)) {
            if (out) {
                out->status = 1;
                snprintf(out->error, sizeof(out->error),
                         "query: only SELECT/WITH/PRAGMA/EXPLAIN allowed");
            }
            free(sql);
            return;
        }
        long limit = params ? json_get_int(params, "limit", 1000) : 1000;
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(sc->db, sql, -1, &st, NULL) != SQLITE_OK) {
            if (out) {
                out->status = 1;
                snprintf(out->error, sizeof(out->error), "query: %s", sqlite3_errmsg(sc->db));
            }
            free(sql);
            return;
        }
        free(sql);
        int ncol = sqlite3_column_count(st);
        jbuf_t b;
        jbuf_init(&b, 512);
        jbuf_append(&b, "{\"columns\":[");
        for (int i = 0; i < ncol; i++) {
            if (i)
                jbuf_append(&b, ",");
            jbuf_append_json_str(&b, sqlite3_column_name(st, i));
        }
        jbuf_append(&b, "],\"rows\":[");
        long n = 0;
        int truncated = 0;
        for (;;) {
            int rc = sqlite3_step(st);
            if (rc != SQLITE_ROW)
                break;
            if (n >= limit) {
                truncated = 1;
                break;
            }
            if (n)
                jbuf_append(&b, ",");
            jbuf_append(&b, "[");
            for (int i = 0; i < ncol; i++) {
                if (i)
                    jbuf_append(&b, ",");
                sqlite_emit_value(&b, st, i);
            }
            jbuf_append(&b, "]");
            n++;
        }
        sqlite3_finalize(st);
        jbuf_appendf(&b, "],\"count\":%ld,\"truncated\":%s}", n, truncated ? "true" : "false");
        if (out) {
            out->status = 0;
            out->body = b.data;
        } else
            jbuf_free(&b);
        return;
    }

    if (out) {
        out->status = 1;
        snprintf(out->error, sizeof(out->error), "sqlite: unknown method '%s'", m);
    }
}

static char *sqlite_describe(void *self) {
    sqlite_conn_t *sc = self;
    jbuf_t b;
    jbuf_init(&b, 160);
    jbuf_append(&b, "{\"kind\":\"sqlite\",\"path\":");
    jbuf_append_json_str(&b, sc->path);
    jbuf_append(&b, ",\"readonly\":true,\"methods\":[\"query\",\"tables\"]}");
    return b.data;
}

static char *sqlite_schema(void *self, const char *method) {
    (void)self;
    if (method && strcmp(method, "query") == 0)
        return safe_strdup("{\"type\":\"object\",\"required\":[\"sql\"],"
                           "\"properties\":{\"sql\":{\"type\":\"string\"},"
                           "\"limit\":{\"type\":\"integer\"}}}");
    return NULL;
}

static void sqlite_close(void *self) {
    sqlite_conn_t *sc = self;
    if (sc) {
        if (sc->db)
            sqlite3_close(sc->db);
        free(sc->path);
        free(sc);
    }
}

static const connector_vtable_t SQLITE_VT = {
    .kind = "sqlite",
    .description = "Read-only SQLite query seam — deal/collateral book access for personas",
    .capabilities = CONN_CAP_INVOKE,
    .osi_layers = OSI_L7_APPLICATION,
    .open = sqlite_open,
    .invoke = sqlite_invoke,
    .stream = NULL,
    .describe = sqlite_describe,
    .schema = sqlite_schema,
    .close = sqlite_close,
};

void connector_register_builtins(void) {
    static int done = 0;
    if (done)
        return;
    done = 1;
    connector_register(&TOOL_VT);
    connector_register(&SHELL_VT);
    connector_register(&FLOW_VT);
    connector_register(&NET_VT);
    connector_register(&SERIAL_VT);
    connector_register(&CHAIN_VT);
    connector_register(&AGENT_VT);
    connector_register(&SQLITE_VT);
}

/* ── CLI ──────────────────────────────────────────────────────────────── */
static void conn_kv_to_json(jbuf_t *b, char **pairs, int n) {
    jbuf_append(b, "{");
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        char *eq = strchr(pairs[i], '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = pairs[i];
        const char *val = eq + 1;
        if (emitted)
            jbuf_append(b, ",");
        jbuf_append_json_str(b, key);
        jbuf_append(b, ":");
        if (conn_is_json_scalar(val))
            jbuf_append(b, val);
        else
            jbuf_append_json_str(b, val);
        *eq = '=';
        emitted++;
    }
    jbuf_append(b, "}");
}

/* Like conn_kv_to_json, but values may be feedback placeholders threaded from
 * prior step bodies (see conn_subst). Used by `pipe`/`loop`. */
static void conn_kv_to_json_threaded(jbuf_t *b, char **pairs, int n, char **outputs, int ndone,
                                     const char *seed) {
    jbuf_append(b, "{");
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        char *eq = strchr(pairs[i], '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = pairs[i];
        const char *val = eq + 1;
        if (emitted)
            jbuf_append(b, ",");
        jbuf_append_json_str(b, key);
        jbuf_append(b, ":");
        /* Reuse conn_subst by wrapping val as a quoted token if it is one. */
        if (val[0] == '{' && strncmp(val, "{{", 2) == 0) {
            char quoted[128];
            snprintf(quoted, sizeof(quoted), "\"%s\"", val);
            char *sub = conn_subst(quoted, outputs, ndone, seed);
            jbuf_append(b, sub);
            free(sub);
        } else if (conn_is_json_scalar(val)) {
            jbuf_append(b, val);
        } else {
            jbuf_append_json_str(b, val);
        }
        *eq = '=';
        emitted++;
    }
    jbuf_append(b, "}");
}

static bool conn_print_chunk(const char *chunk, void *ctx) {
    (void)ctx;
    printf("%s\n", chunk);
    fflush(stdout);
    return true;
}

/* Returns 1 if c advertises every capability named in `req` (comma list). */
static int conn_require_ok(connector_t *c, const char *req) {
    if (!req)
        return 1;
    unsigned need = conn_caps_from_str(req);
    unsigned have = connector_capabilities(c);
    if ((have & need) != need) {
        fprintf(stderr, "connector lacks required capabilities (%s)\n", req);
        return 0;
    }
    return 1;
}

static void conn_print_caps(unsigned c) {
    const struct {
        unsigned f;
        const char *n;
    } tab[] = {
        {CONN_CAP_INVOKE, "invoke"}, {CONN_CAP_STREAM, "stream"}, {CONN_CAP_SUBSCRIBE, "subscribe"},
        {CONN_CAP_TX, "tx"},         {CONN_CAP_SENSE, "sense"},   {CONN_CAP_ACTUATE, "actuate"},
    };
    int first = 1;
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (c & tab[i].f) {
            printf("%s%s", first ? "" : ",", tab[i].n);
            first = 0;
        }
    if (first)
        printf("-");
}

static void conn_print_osi(unsigned o) {
    const char *names[] = {"L1", "L2", "L3", "L4", "L5", "L6", "L7"};
    int first = 1;
    for (int i = 0; i < 7; i++)
        if (o & (1u << i)) {
            printf("%s%s", first ? "" : "/", names[i]);
            first = 0;
        }
    if (first)
        printf("-");
}

/* Sequential cross-kind pipeline: steps separated by "--", each step is
 * <kind> <method> [k=v…], with k=v values able to reference prior outputs via
 * {{prev}}/{{stepN}}/{{prev:key}}. Stops on the first failing step. */
static int conn_cli_pipe(char **rest, int nrest, const char *config, const char *require) {
    char *outputs[64] = {0};
    int nout = 0, rc = 0, i = 0;
    while (i < nrest) {
        int j = i;
        while (j < nrest && strcmp(rest[j], "--") != 0)
            j++;
        int len = j - i;
        if (len < 2) {
            fprintf(stderr, "pipe: each step needs <kind> <method>\n");
            rc = 2;
            break;
        }
        const char *kind = rest[i];
        const char *method = rest[i + 1];
        jbuf_t params;
        jbuf_init(&params, 256);
        conn_kv_to_json_threaded(&params, &rest[i + 2], len - 2, outputs, nout, NULL);
        char err[256];
        connector_t *c = connector_open(kind, config, err, sizeof(err));
        if (!c) {
            fprintf(stderr, "%s\n", err);
            jbuf_free(&params);
            rc = 1;
            break;
        }
        if (!conn_require_ok(c, require)) {
            connector_close(c);
            jbuf_free(&params);
            rc = 1;
            break;
        }
        conn_result_t res;
        connector_invoke(c, method, params.data, &res);
        jbuf_free(&params);
        connector_close(c);
        fprintf(stderr, "=== [step %d] %s/%s status=%ld ===\n", nout + 1, kind, method, res.status);
        if (res.body)
            printf("%s\n", res.body);
        if (res.status != 0) {
            if (res.status < 0)
                fprintf(stderr, "error: %s\n", res.error[0] ? res.error : "transport");
            conn_result_free(&res);
            rc = 1;
            break;
        }
        char *body = res.body ? res.body : safe_strdup("");
        res.body = NULL;
        conn_result_free(&res);
        if (nout < (int)(sizeof(outputs) / sizeof(outputs[0])))
            outputs[nout++] = body;
        else
            free(body);
        i = (j < nrest) ? j + 1 : j;
    }
    for (int k = 0; k < nout; k++)
        free(outputs[k]);
    return rc;
}

/* Single-connector feedback loop: re-invoke up to --max times, feeding each
 * result back as {{prev}} for the next iteration; stop early on --until-stable
 * when the output stops changing. */
static int conn_cli_loop(char **rest, int nrest, const char *config, const char *require, int max,
                         int until_stable, const char *seed) {
    if (nrest < 2) {
        fprintf(stderr, "loop: need <kind> <method>\n");
        return 2;
    }
    const char *kind = rest[0];
    const char *method = rest[1];
    if (max < 1)
        max = 1;
    char err[256];
    connector_t *c = connector_open(kind, config, err, sizeof(err));
    if (!c) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (!conn_require_ok(c, require)) {
        connector_close(c);
        return 1;
    }

    char *prev = seed ? safe_strdup(seed) : NULL;
    int rc = 0;
    for (int it = 1; it <= max; it++) {
        char *outs[1];
        int nout = prev ? 1 : 0;
        outs[0] = prev;
        jbuf_t params;
        jbuf_init(&params, 256);
        conn_kv_to_json_threaded(&params, &rest[2], nrest - 2, outs, nout, prev);
        conn_result_t res;
        connector_invoke(c, method, params.data, &res);
        jbuf_free(&params);
        fprintf(stderr, "=== [iter %d] %s/%s status=%ld ===\n", it, kind, method, res.status);
        if (res.body)
            printf("%s\n", res.body);
        if (res.status != 0) {
            if (res.status < 0)
                fprintf(stderr, "error: %s\n", res.error[0] ? res.error : "transport");
            conn_result_free(&res);
            rc = 1;
            break;
        }
        char *body = res.body ? res.body : safe_strdup("");
        res.body = NULL;
        conn_result_free(&res);
        int stable = (prev && strcmp(prev, body) == 0);
        free(prev);
        prev = body;
        if (until_stable && stable) {
            fprintf(stderr, "(stable at iter %d)\n", it);
            break;
        }
    }
    free(prev);
    connector_close(c);
    return rc;
}

static void conn_cli_usage(void) {
    printf("usage: dsco connect <command>\n"
           "\n"
           "  kinds                              list registered connector kinds\n"
           "  describe <kind>                    print a kind's capability/manifest doc\n"
           "  <kind> <method> [k=v ...]          open <kind> and invoke <method>\n"
           "  stream <kind> <method> [k=v ...]   invoke and print streamed chunks\n"
           "  pipe <k> <m> [kv] -- <k> <m> [kv]  sequential cross-kind pipeline\n"
           "  loop <kind> <method> [k=v ...]     re-invoke, feeding output back as {{prev}}\n"
           "  schema <kind> <method>             print the method's inferred JSON-Schema\n"
           "\n"
           "feedback placeholders in k=v values (pipe/loop) and flow specs:\n"
           "  {{prev}} {{stepN}} {{seed}} {{prev:key}}   prior result, or a field of it\n"
           "\n"
           "options:\n"
           "  --config '<json>'   config object passed to open() (or @path to load a file)\n"
           "  --require <caps>    fail unless the kind advertises these (invoke,stream,tx,…)\n"
           "  --validate          type-check params against the method schema at the seam\n"
           "  --seed '<json>'     loop: seed body for iteration 1's {{prev}}/{{seed}}\n"
           "  --max <n>           loop: max iterations (default 1)\n"
           "  --until-stable      loop: stop when the output stops changing\n"
           "\n"
           "recursive hierarchical feedback: the `flow` kind is itself a connector whose\n"
           "config is a {\"steps\":[…],\"loop\":{…}} spec; a step's kind may be `flow`, so\n"
           "flows nest inside flows and loop to convergence.\n"
           "\n"
           "The connector is the future-proof baseline seam: every external system\n"
           "(tools today; chains, credit, robotics, neural/haptic next) is one kind.\n");
}

/* Slurp a whole file into a malloc'd NUL-terminated buffer (caller frees). */
static char *conn_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = safe_malloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static int conn_cli_kinds(void) {
    const connector_vtable_t *kinds[CONN_MAX_KINDS];
    int n = connector_list(kinds, CONN_MAX_KINDS);
    if (n > CONN_MAX_KINDS)
        n = CONN_MAX_KINDS;
    for (int i = 0; i < n; i++) {
        printf("%-8s ", kinds[i]->kind);
        printf("osi=");
        conn_print_osi(kinds[i]->osi_layers);
        printf(" [");
        conn_print_caps(kinds[i]->capabilities);
        printf("]  %s\n", kinds[i]->description ? kinds[i]->description : "");
    }
    if (n == 0)
        printf("(no connector kinds registered)\n");
    return 0;
}

int connector_cli(int argc, char **argv) {
    connector_register_builtins();

    /* Pull options from anywhere in the argv tail; keep positionals in rest. */
    const char *config = NULL, *require = NULL, *seed = NULL;
    int max = 1, until_stable = 0;
    char *rest[128];
    int nrest = 0;
    for (int i = 2; i < argc && nrest < (int)(sizeof(rest) / sizeof(rest[0])); i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--require") == 0 && i + 1 < argc) {
            require = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            max = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--until-stable") == 0) {
            until_stable = 1;
            continue;
        }
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--validate") == 0) {
            connector_set_validate(1);
            continue;
        }
        rest[nrest++] = argv[i];
    }

    if (nrest == 0 || strcmp(rest[0], "-h") == 0 || strcmp(rest[0], "--help") == 0) {
        conn_cli_usage();
        return nrest == 0 ? 1 : 0;
    }

    /* --config @path loads the spec from a file (templates live on disk). */
    if (config && config[0] == '@') {
        char *loaded = conn_read_file(config + 1);
        if (!loaded) {
            fprintf(stderr, "config: cannot read file '%s'\n", config + 1);
            return 1;
        }
        config = loaded; /* leaked intentionally; process is one-shot */
    }

    if (strcmp(rest[0], "kinds") == 0)
        return conn_cli_kinds();

    if (strcmp(rest[0], "describe") == 0) {
        if (nrest < 2) {
            fprintf(stderr, "describe: need <kind>\n");
            return 2;
        }
        char err[256];
        connector_t *c = connector_open(rest[1], config, err, sizeof(err));
        if (!c) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        char *doc = connector_describe(c);
        connector_close(c);
        if (!doc) {
            fprintf(stderr, "%s: no manifest\n", rest[1]);
            return 1;
        }
        printf("%s\n", doc);
        free(doc);
        return 0;
    }

    if (strcmp(rest[0], "schema") == 0) {
        if (nrest < 3) {
            fprintf(stderr, "schema: need <kind> <method>\n");
            return 2;
        }
        char err[256];
        connector_t *c = connector_open(rest[1], config, err, sizeof(err));
        if (!c) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        char *sc = connector_schema(c, rest[2]);
        connector_close(c);
        if (!sc) {
            fprintf(stderr, "%s/%s: no schema available\n", rest[1], rest[2]);
            return 1;
        }
        printf("%s\n", sc);
        free(sc);
        return 0;
    }

    if (strcmp(rest[0], "pipe") == 0)
        return conn_cli_pipe(&rest[1], nrest - 1, config, require);

    if (strcmp(rest[0], "loop") == 0)
        return conn_cli_loop(&rest[1], nrest - 1, config, require, max, until_stable, seed);

    int streaming = 0, off = 0;
    if (strcmp(rest[0], "stream") == 0) {
        streaming = 1;
        off = 1;
    }

    /* [stream] <kind> <method> [k=v ...] */
    if (nrest - off < 2) {
        fprintf(stderr, "need <kind> <method>\n");
        return 2;
    }
    const char *kind = rest[off];
    const char *method = rest[off + 1];

    jbuf_t params;
    jbuf_init(&params, 256);
    conn_kv_to_json(&params, &rest[off + 2], nrest - off - 2);

    char err[256];
    connector_t *c = connector_open(kind, config, err, sizeof(err));
    if (!c) {
        fprintf(stderr, "%s\n", err);
        jbuf_free(&params);
        return 1;
    }
    if (!conn_require_ok(c, require)) {
        connector_close(c);
        jbuf_free(&params);
        return 1;
    }

    conn_result_t res;
    if (streaming)
        connector_stream(c, method, params.data, conn_print_chunk, NULL, &res);
    else
        connector_invoke(c, method, params.data, &res);
    jbuf_free(&params);
    connector_close(c);

    if (res.status < 0) {
        fprintf(stderr, "error: %s\n", res.error[0] ? res.error : "transport failure");
        conn_result_free(&res);
        return 1;
    }
    if (res.body)
        printf("%s\n", res.body);
    int rc = (res.status == 0) ? 0 : 1;
    conn_result_free(&res);
    return rc;
}
