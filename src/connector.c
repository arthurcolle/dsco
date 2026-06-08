#include "connector.h"
#include "toolmgmt.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Instance type ────────────────────────────────────────────────────── */
struct connector {
    const connector_vtable_t *vt;
    void                     *self;
};

void conn_result_free(conn_result_t *r) {
    if (!r) return;
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
    if (!vt || !vt->kind || !vt->kind[0]) return -1;
    if (connector_find(vt->kind)) return -1;          /* no dup kinds */
    if (g_nkinds >= CONN_MAX_KINDS) return -1;
    g_kinds[g_nkinds++] = vt;
    return 0;
}

const connector_vtable_t *connector_find(const char *kind) {
    if (!kind) return NULL;
    for (int i = 0; i < g_nkinds; i++)
        if (strcmp(g_kinds[i]->kind, kind) == 0) return g_kinds[i];
    return NULL;
}

int connector_list(const connector_vtable_t **out, int max) {
    int n = g_nkinds < max ? g_nkinds : max;
    for (int i = 0; i < n; i++) out[i] = g_kinds[i];
    return g_nkinds;
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */
connector_t *connector_open(const char *kind, const char *config_json,
                            char *err, size_t errlen) {
    const connector_vtable_t *vt = connector_find(kind);
    if (!vt) {
        if (err && errlen) snprintf(err, errlen, "unknown connector kind: %s",
                                    kind ? kind : "(null)");
        return NULL;
    }
    void *self = NULL;
    if (vt->open) {
        char oerr[256] = {0};
        self = vt->open(config_json, oerr, sizeof(oerr));
        if (!self) {
            if (err && errlen)
                snprintf(err, errlen, "%s: %s", vt->kind,
                         oerr[0] ? oerr : "open failed");
            return NULL;
        }
    }
    connector_t *c = safe_malloc(sizeof(*c));
    c->vt = vt;
    c->self = self;
    return c;
}

void connector_invoke(connector_t *c, const char *method,
                      const char *params_json, conn_result_t *out) {
    if (out) { out->status = -1; out->body = NULL; out->error[0] = '\0'; }
    if (!c || !c->vt->invoke) {
        if (out) snprintf(out->error, sizeof(out->error),
                          "connector does not support invoke");
        return;
    }
    c->vt->invoke(c->self, method, params_json, out);
}

void connector_stream(connector_t *c, const char *method,
                      const char *params_json,
                      conn_chunk_cb on_chunk, void *ctx, conn_result_t *out) {
    if (out) { out->status = -1; out->body = NULL; out->error[0] = '\0'; }
    if (!c || !c->vt->stream) {
        if (out) snprintf(out->error, sizeof(out->error),
                          "connector does not support stream");
        return;
    }
    c->vt->stream(c->self, method, params_json, on_chunk, ctx, out);
}

char *connector_describe(connector_t *c) {
    if (!c || !c->vt->describe) return NULL;
    return c->vt->describe(c->self);
}

unsigned connector_capabilities(const connector_t *c) {
    return c ? c->vt->capabilities : 0u;
}

void connector_close(connector_t *c) {
    if (!c) return;
    if (c->vt->close) c->vt->close(c->self);
    free(c);
}

/* ── Built-in adapter: "tool" (Tool Management API over HTTP) ─────────────
 * method = remote tool name, params = its inputs object. This is the first
 * concrete proof of the seam; future kinds register the same way. */
typedef struct { int _stub; } tool_conn_t;

static void *tool_open(const char *config_json, char *err, size_t errlen) {
    (void)err; (void)errlen;
    if (config_json && config_json[0]) {
        char *url   = json_get_str(config_json, "url");
        char *token = json_get_str(config_json, "token");
        if (url && url[0])     toolmgmt_set_base_url(url);
        if (token && token[0]) toolmgmt_set_token(token);
        free(url); free(token);
    }
    return safe_malloc(sizeof(tool_conn_t));
}

static void tool_invoke(void *self, const char *method,
                        const char *params_json, conn_result_t *out) {
    (void)self;
    if (!method || !method[0]) {
        if (out) snprintf(out->error, sizeof(out->error), "missing tool name");
        return;
    }
    char *body = toolmgmt_execute(method, params_json, 0);
    if (!out) { free(body); return; }
    if (!body) {
        out->status = -1;
        snprintf(out->error, sizeof(out->error),
                 "tool '%s' returned no response", method);
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

static void tool_close(void *self) { free(self); }

static const connector_vtable_t TOOL_VT = {
    .kind         = "tool",
    .description  = "Tool Management API over HTTP — discover/execute remote tools",
    .capabilities = CONN_CAP_INVOKE,
    .open         = tool_open,
    .invoke       = tool_invoke,
    .stream       = NULL,
    .describe     = tool_describe,
    .close        = tool_close,
};

void connector_register_builtins(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    connector_register(&TOOL_VT);
}

/* ── CLI ──────────────────────────────────────────────────────────────── */
static bool conn_is_json_scalar(const char *v) {
    if (!v || !v[0]) return false;
    if (strcmp(v, "true") == 0 || strcmp(v, "false") == 0 || strcmp(v, "null") == 0)
        return true;
    if (v[0] == '{' || v[0] == '[' || v[0] == '"') return true;
    const char *p = v;
    if (*p == '-' || *p == '+') p++;
    bool digit = false, dot = false;
    for (; *p; p++) {
        if (isdigit((unsigned char)*p)) digit = true;
        else if (*p == '.' && !dot) dot = true;
        else return false;
    }
    return digit;
}

static void conn_kv_to_json(jbuf_t *b, char **pairs, int n) {
    jbuf_append(b, "{");
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        char *eq = strchr(pairs[i], '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = pairs[i];
        const char *val = eq + 1;
        if (emitted) jbuf_append(b, ",");
        jbuf_append_json_str(b, key);
        jbuf_append(b, ":");
        if (conn_is_json_scalar(val)) jbuf_append(b, val);
        else jbuf_append_json_str(b, val);
        *eq = '=';
        emitted++;
    }
    jbuf_append(b, "}");
}

static void conn_print_caps(unsigned c) {
    const struct { unsigned f; const char *n; } tab[] = {
        {CONN_CAP_INVOKE,"invoke"}, {CONN_CAP_STREAM,"stream"},
        {CONN_CAP_SUBSCRIBE,"subscribe"}, {CONN_CAP_TX,"tx"},
        {CONN_CAP_SENSE,"sense"}, {CONN_CAP_ACTUATE,"actuate"},
    };
    int first = 1;
    for (size_t i = 0; i < sizeof(tab)/sizeof(tab[0]); i++)
        if (c & tab[i].f) { printf("%s%s", first ? "" : ",", tab[i].n); first = 0; }
    if (first) printf("-");
}

static void conn_cli_usage(void) {
    printf(
"usage: dsco connect <command>\n"
"\n"
"  kinds                          list registered connector kinds\n"
"  describe <kind> [config-json]  print a kind's capability/manifest doc\n"
"  <kind> <method> [k=v ...]      open <kind> and invoke <method>\n"
"                                 (values that look like JSON are passed raw)\n"
"\n"
"options:\n"
"  --config '<json>'              config object passed to open()\n"
"\n"
"The connector is the future-proof baseline seam: every external system\n"
"(tools today; chains, credit, robotics, neural/haptic next) is one kind.\n");
}

static int conn_cli_kinds(void) {
    const connector_vtable_t *kinds[CONN_MAX_KINDS];
    int n = connector_list(kinds, CONN_MAX_KINDS);
    if (n > CONN_MAX_KINDS) n = CONN_MAX_KINDS;
    for (int i = 0; i < n; i++) {
        printf("%-10s [", kinds[i]->kind);
        conn_print_caps(kinds[i]->capabilities);
        printf("]  %s\n", kinds[i]->description ? kinds[i]->description : "");
    }
    if (n == 0) printf("(no connector kinds registered)\n");
    return 0;
}

int connector_cli(int argc, char **argv) {
    connector_register_builtins();

    /* Pull an optional --config '<json>' from anywhere in the argv tail. */
    const char *config = NULL;
    char *rest[64];
    int nrest = 0;
    for (int i = 2; i < argc && nrest < (int)(sizeof(rest)/sizeof(rest[0])); i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) { config = argv[++i]; continue; }
        rest[nrest++] = argv[i];
    }

    if (nrest == 0 || strcmp(rest[0], "-h") == 0 || strcmp(rest[0], "--help") == 0) {
        conn_cli_usage();
        return nrest == 0 ? 1 : 0;
    }

    if (strcmp(rest[0], "kinds") == 0) return conn_cli_kinds();

    if (strcmp(rest[0], "describe") == 0) {
        if (nrest < 2) { fprintf(stderr, "describe: need <kind>\n"); return 2; }
        char err[256];
        connector_t *c = connector_open(rest[1], config, err, sizeof(err));
        if (!c) { fprintf(stderr, "%s\n", err); return 1; }
        char *doc = connector_describe(c);
        connector_close(c);
        if (!doc) { fprintf(stderr, "%s: no manifest\n", rest[1]); return 1; }
        printf("%s\n", doc);
        free(doc);
        return 0;
    }

    /* <kind> <method> [k=v ...] */
    if (nrest < 2) { fprintf(stderr, "need <kind> <method>\n"); return 2; }
    const char *kind = rest[0];
    const char *method = rest[1];

    jbuf_t params; jbuf_init(&params, 256);
    conn_kv_to_json(&params, &rest[2], nrest - 2);

    char err[256];
    connector_t *c = connector_open(kind, config, err, sizeof(err));
    if (!c) { fprintf(stderr, "%s\n", err); jbuf_free(&params); return 1; }

    conn_result_t res;
    connector_invoke(c, method, params.data, &res);
    jbuf_free(&params);
    connector_close(c);

    if (res.status < 0) {
        fprintf(stderr, "error: %s\n", res.error[0] ? res.error : "transport failure");
        conn_result_free(&res);
        return 1;
    }
    if (res.body) printf("%s\n", res.body);
    int rc = (res.status == 0) ? 0 : 1;
    conn_result_free(&res);
    return rc;
}
