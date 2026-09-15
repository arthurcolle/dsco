/* Prompt programs: deterministic DAG compilation and first-class prompt constructors.
 * Deliberately no provider/tool dispatch. Artifacts are proposals, never authority. */
#include "prompt_program.h"
#include "json_util.h"
#include "crypto.h"
#include "../vendor/yyjson.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PP_SOURCE (256U * 1024U)
#define PP_TEXT (64U * 1024U)
#define PP_TOTAL (1024U * 1024U)
#define PP_NODES 64

typedef struct {
    const char *id, *kind;
    yyjson_val *value;
    char *output;
    unsigned char state;
    bool deps[PP_NODES];
} pp_node_t;
typedef struct {
    pp_node_t nodes[PP_NODES];
    int count, order[PP_NODES], ordered;
    size_t total;
    yyjson_val *inputs;
    char *error;
    size_t error_size;
} pp_t;

static bool fail(pp_t *p, const char *fmt, ...) {
    if (p->error && p->error_size && !p->error[0]) {
        va_list ap; va_start(ap, fmt); vsnprintf(p->error, p->error_size, fmt, ap); va_end(ap);
    }
    return false;
}
static bool name_ok(const char *s) {
    if (!s || !*s || strlen(s) > 63) return false;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                           (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return false;
    return true;
}
static const char *str(yyjson_val *v) {
    const char *s = yyjson_get_str(v);
    return s && strlen(s) == yyjson_get_len(v) ? s : NULL; /* embedded NUL forbidden */
}
static int find(pp_t *p, const char *id) {
    for (int i = 0; i < p->count; i++) if (!strcmp(id, p->nodes[i].id)) return i;
    return -1;
}
static bool fields(pp_t *p, yyjson_val *v, const char *allowed) {
    if (!yyjson_is_obj(v)) return fail(p, "expected an object");
    size_t i, n; yyjson_val *key, *value;
    yyjson_obj_foreach(v, i, n, key, value) {
        (void)value;
        const char *k = str(key); char token[80];
        if (!name_ok(k)) return fail(p, "invalid field name");
        snprintf(token, sizeof(token), "|%s|", k);
        if (!strstr(allowed, token)) return fail(p, "unknown field: %s", k);
        yyjson_obj_iter iter = yyjson_obj_iter_with(v); yyjson_val *other; int matches = 0;
        while ((other = yyjson_obj_iter_next(&iter))) if (yyjson_equals_str(other, k)) matches++;
        if (matches != 1) return fail(p, "duplicate field: %s", k);
    }
    return true;
}
static bool bindings(pp_t *p, yyjson_val *v) {
    if (!yyjson_is_obj(v) || yyjson_obj_size(v) > 64)
        return fail(p, "inputs/args must be string-valued objects with at most 64 bindings");
    size_t i, n; yyjson_val *k, *val;
    yyjson_obj_foreach(v, i, n, k, val) {
        if (!name_ok(str(k)) || !str(val)) return fail(p, "invalid binding name or non-string value");
        yyjson_obj_iter iter = yyjson_obj_iter_with(v); yyjson_val *other; int matches = 0;
        while ((other = yyjson_obj_iter_next(&iter))) if (yyjson_equals_str(other, str(k))) matches++;
        if (matches != 1) return fail(p, "duplicate binding: %s", str(k));
    }
    return true;
}
static bool evaluate(pp_t *p, int index);
/* Substitutions are single-pass. Inserted text is never parsed as more code. */
static char *expand(pp_t *p, int owner, const char *text, yyjson_val *args,
                    bool meta_validate) {
    if (!text) { fail(p, "node %s: template/text must be a string", p->nodes[owner].id); return NULL; }
    jbuf_t out; jbuf_init(&out, 256);
    const char *pos = text;
    while (*pos) {
        const char *open = strstr(pos, "{{");
        size_t literal = open ? (size_t)(open - pos) : strlen(pos);
        if (out.len + literal > PP_TEXT) goto oversized;
        jbuf_append_len(&out, pos, literal);
        if (!open) break;
        const char *end = strstr(open + 2, "}}"); char ref[80];
        if (!end || end - (open + 2) >= (int)sizeof(ref)) {
            fail(p, "node %s: malformed reference", p->nodes[owner].id); goto bad;
        }
        size_t length = (size_t)(end - open - 2);
        memcpy(ref, open + 2, length); ref[length] = 0;
        char *dot = strchr(ref, '.');
        if (!dot || !name_ok(dot + 1)) { fail(p, "invalid reference in node %s", p->nodes[owner].id); goto bad; }
        *dot++ = 0;
        if (!name_ok(ref)) { fail(p, "invalid reference scope"); goto bad; }
        const char *replacement = NULL;
        if (meta_validate) {
            if (strcmp(ref, "arg")) { fail(p, "meta templates accept only {{arg.name}} references"); goto bad; }
            if (out.len + (size_t)(end + 2 - open) > PP_TEXT) goto oversized;
            jbuf_append_len(&out, open, (size_t)(end + 2 - open)); pos = end + 2; continue;
        }
        if (args) {
            if (strcmp(ref, "arg")) { fail(p, "constructor expected arg reference"); goto bad; }
            replacement = str(yyjson_obj_get(args, dot));
        } else if (!strcmp(ref, "input")) replacement = str(yyjson_obj_get(p->inputs, dot));
        else if (!strcmp(ref, "node")) {
            int dep = find(p, dot);
            if (dep < 0) { fail(p, "missing node: %s", dot); goto bad; }
            p->nodes[owner].deps[dep] = true;
            if (!evaluate(p, dep)) goto bad;
            if (!strcmp(p->nodes[dep].kind, "meta")) { fail(p, "meta node %s must be applied, not interpolated", dot); goto bad; }
            replacement = p->nodes[dep].output;
        }
        if (!replacement) { fail(p, "unbound reference: %s.%s", ref, dot); goto bad; }
        if (out.len + strlen(replacement) > PP_TEXT) goto oversized;
        jbuf_append(&out, replacement); pos = end + 2;
    }
    return out.data;
oversized:
    fail(p, "node %s exceeds 64 KiB output limit", p->nodes[owner].id);
bad:
    jbuf_free(&out); return NULL;
}

static bool evaluate(pp_t *p, int index) {
    pp_node_t *node = &p->nodes[index];
    if (node->state == 2) return true;
    if (node->state == 1) return fail(p, "dependency cycle at %s", node->id);
    node->state = 1;
    yyjson_val *v = node->value;
    if (!strcmp(node->kind, "text")) {
        const char *s = str(yyjson_obj_get(v, "text"));
        if (!s || strlen(s) > PP_TEXT) return fail(p, "invalid/oversized text node: %s", node->id);
        node->output = safe_strdup(s);
    } else if (!strcmp(node->kind, "meta")) {
        node->output = expand(p, index, str(yyjson_obj_get(v, "template")), NULL, true);
    } else if (!strcmp(node->kind, "apply")) {
        const char *source = str(yyjson_obj_get(v, "source"));
        int dep = source ? find(p, source) : -1;
        if (dep < 0 || strcmp(p->nodes[dep].kind, "meta")) return fail(p, "apply %s requires a meta source", node->id);
        node->deps[dep] = true;
        if (!evaluate(p, dep)) return false;
        yyjson_val *args = yyjson_obj_get(v, "args");
        if (!bindings(p, args)) return false;
        /* Compile each argument in the caller scope, then substitute into constructor. */
        jbuf_t json; jbuf_init(&json, 256); jbuf_append(&json, "{");
        size_t i, n; yyjson_val *key, *value;
        yyjson_obj_foreach(args, i, n, key, value) {
            char token[96]; snprintf(token, sizeof(token), "{{arg.%s}}", str(key));
            if (!strstr(p->nodes[dep].output, token)) {
                jbuf_free(&json); return fail(p, "unused argument %s in %s", str(key), node->id);
            }
            char *expanded = expand(p, index, str(value), NULL, false);
            if (!expanded) { jbuf_free(&json); return false; }
            if (json.len > PP_TOTAL) { free(expanded); jbuf_free(&json); return fail(p, "argument expansion budget exceeded"); }
            if (i) jbuf_append_char(&json, ',');
            jbuf_append_json_str(&json, str(key)); jbuf_append_char(&json, ':');
            jbuf_append_json_str(&json, expanded); free(expanded);
        }
        jbuf_append_char(&json, '}');
        yyjson_doc *doc = yyjson_read(json.data, json.len, 0);
        if (doc) node->output = expand(p, index, p->nodes[dep].output, yyjson_doc_get_root(doc), false);
        else fail(p, "could not parse expanded arguments");
        yyjson_doc_free(doc); jbuf_free(&json);
    } else {
        node->output = expand(p, index, str(yyjson_obj_get(v, "template")), NULL, false);
        if (node->output && !strcmp(node->kind, "check")) {
            bool any = false;
            const char *keys[] = {"contains", "not_contains"};
            for (int k = 0; k < 2; k++) {
                yyjson_val *a = yyjson_obj_get(v, keys[k]);
                if (!a) continue;
                if (!yyjson_is_arr(a)) return fail(p, "check %s: %s must be an array", node->id, keys[k]);
                size_t i, n; yyjson_val *value;
                yyjson_arr_foreach(a, i, n, value) {
                    const char *needle = str(value);
                    if (!needle || !*needle) return fail(p, "check patterns must be nonempty strings");
                    any = true; bool found = strstr(node->output, needle) != NULL;
                    if (found != (k == 0)) return fail(p, "check failed: %s (%s item %zu)", node->id, keys[k], i);
                }
            }
            if (!any) return fail(p, "check %s needs at least one assertion", node->id);
        }
    }
    if (!node->output) return false;
    p->total += strlen(node->output);
    if (p->total > PP_TOTAL) return fail(p, "program output exceeds 1 MiB limit");
    node->state = 2; p->order[p->ordered++] = index;
    return true;
}

bool prompt_program_compile(const char *source, size_t length, char **compiled_json,
                            char *error, size_t error_size) {
    if (error && error_size) error[0] = 0;
    if (!compiled_json) return false;
    *compiled_json = NULL;
    pp_t p = {.error = error, .error_size = error_size};
    if (!source || !length || length > PP_SOURCE || memchr(source, 0, length)) return fail(&p, "source must be 1..262144 bytes without NUL");
    yyjson_read_err parse_error;
    yyjson_doc *doc = yyjson_read_opts((char *)source, length, 0, NULL, &parse_error);
    if (!doc) return fail(&p, "invalid JSON at byte %zu", parse_error.pos);
    bool ok = false;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!fields(&p, root, "|version||name||inputs||nodes||outputs|")) goto done;
    if (!yyjson_equals_str(yyjson_obj_get(root, "version"), "dsco.prompt.v1")) { fail(&p, "expected version dsco.prompt.v1"); goto done; }
    const char *name = str(yyjson_obj_get(root, "name"));
    if (!name_ok(name)) { fail(&p, "invalid program name"); goto done; }
    p.inputs = yyjson_obj_get(root, "inputs");
    if (!bindings(&p, p.inputs)) goto done;
    yyjson_val *nodes = yyjson_obj_get(root, "nodes");
    if (!yyjson_is_arr(nodes) || !yyjson_arr_size(nodes) || yyjson_arr_size(nodes) > PP_NODES) { fail(&p, "expected 1..64 nodes"); goto done; }
    size_t i, n; yyjson_val *v;
    yyjson_arr_foreach(nodes, i, n, v) {
        if (!yyjson_is_obj(v)) { fail(&p, "node must be an object"); goto done; }
        const char *id = str(yyjson_obj_get(v, "id")), *kind = str(yyjson_obj_get(v, "kind"));
        if (!name_ok(id) || !name_ok(kind) || find(&p, id) >= 0) { fail(&p, "invalid or duplicate node id/kind"); goto done; }
        const char *allowed = NULL;
        if (!strcmp(kind, "text")) allowed = "|id||kind||text|";
        if (!strcmp(kind, "prompt") || !strcmp(kind, "meta")) allowed = "|id||kind||template|";
        if (!strcmp(kind, "apply")) allowed = "|id||kind||source||args|";
        if (!strcmp(kind, "check")) allowed = "|id||kind||template||contains||not_contains|";
        if (!allowed) { fail(&p, "unknown kind: %s", kind); goto done; }
        if (!fields(&p, v, allowed)) goto done;
        p.nodes[p.count++] = (pp_node_t){.id = id, .kind = kind, .value = v};
    }
    yyjson_val *outputs = yyjson_obj_get(root, "outputs");
    if (!yyjson_is_arr(outputs) || !yyjson_arr_size(outputs) || yyjson_arr_size(outputs) > PP_NODES) { fail(&p, "outputs must select 1..64 node ids"); goto done; }
    bool selected[PP_NODES] = {0};
    yyjson_arr_foreach(outputs, i, n, v) {
        const char *id = str(v); int index = id ? find(&p, id) : -1;
        if (index < 0 || selected[index]) { fail(&p, "invalid/duplicate output selection"); goto done; }
        selected[index] = true;
    }
    for (int j = 0; j < p.count; j++) if (!evaluate(&p, j)) goto done;
    jbuf_t out; jbuf_init(&out, 1024); char hash[65];
    sha256_hex((const unsigned char *)source, length, hash);
    jbuf_append(&out, "{\"version\":\"dsco.prompt.compiled.v1\",\"name\":"); jbuf_append_json_str(&out, name);
    jbuf_append(&out, ",\"source_sha256\":"); jbuf_append_json_str(&out, hash);
    jbuf_append(&out, ",\"execution\":\"not_run\",\"nodes\":[");
    for (int j = 0; j < p.ordered; j++) {
        int index = p.order[j]; pp_node_t *node = &p.nodes[index];
        if (j) jbuf_append_char(&out, ',');
        jbuf_append(&out, "{\"id\":"); jbuf_append_json_str(&out, node->id);
        jbuf_append(&out, ",\"kind\":"); jbuf_append_json_str(&out, node->kind);
        jbuf_append(&out, ",\"selected\":"); jbuf_append(&out, selected[index] ? "true" : "false");
        jbuf_append(&out, ",\"dependencies\":["); bool comma = false;
        for (int k = 0; k < p.count; k++) if (node->deps[k]) {
            if (comma) jbuf_append_char(&out, ','); comma = true; jbuf_append_json_str(&out, p.nodes[k].id);
        }
        jbuf_append(&out, "],\"status\":"); jbuf_append_json_str(&out, !strcmp(node->kind, "check") ? "check_passed" : "compiled");
        sha256_hex((const unsigned char *)node->output, strlen(node->output), hash);
        jbuf_append(&out, ",\"output_sha256\":"); jbuf_append_json_str(&out, hash);
        jbuf_append(&out, ",\"output\":"); jbuf_append_json_str(&out, node->output); jbuf_append_char(&out, '}');
    }
    jbuf_append(&out, "]}"); *compiled_json = out.data; ok = true;
done:
    for (int j = 0; j < p.count; j++) free(p.nodes[j].output);
    yyjson_doc_free(doc); return ok;
}

int prompt_program_cli(int argc, char **argv) {
    if (argc == 3 && (!strcmp(argv[2], "--help") || !strcmp(argv[2], "help"))) {
        puts("usage: dsco prompt <check|compile|graph> program.json\nLocal deterministic compilation; no model or tool calls."); return 0;
    }
    if (argc != 4 || (strcmp(argv[2], "check") && strcmp(argv[2], "compile") && strcmp(argv[2], "graph"))) {
        fputs("usage: dsco prompt <check|compile|graph> program.json\n", stderr); return 2;
    }
    FILE *file = fopen(argv[3], "rb");
    if (!file) { perror("prompt: open"); return 1; }
    char *source = safe_malloc(PP_SOURCE + 1); size_t len = fread(source, 1, PP_SOURCE + 1, file);
    bool read_error = ferror(file); fclose(file);
    char *compiled = NULL, error[256];
    if (read_error || !prompt_program_compile(source, len, &compiled, error, sizeof(error))) {
        fprintf(stderr, "prompt: %s\n", read_error ? "read error" : error); free(source); return 1;
    }
    free(source);
    if (!strcmp(argv[2], "compile")) puts(compiled);
    else {
        yyjson_doc *doc = yyjson_read(compiled, strlen(compiled), 0);
        yyjson_val *root = yyjson_doc_get_root(doc), *nodes = yyjson_obj_get(root, "nodes");
        if (!strcmp(argv[2], "check")) printf("PASS %s: %zu nodes compiled; execution not run\n", str(yyjson_obj_get(root, "name")), yyjson_arr_size(nodes));
        else {
            size_t i, n; yyjson_val *v;
            yyjson_arr_foreach(nodes, i, n, v) {
                printf("%-16s [%-6s] <-", str(yyjson_obj_get(v, "id")), str(yyjson_obj_get(v, "kind")));
                size_t j, m; yyjson_val *dep;
                yyjson_arr_foreach(yyjson_obj_get(v, "dependencies"), j, m, dep) printf(" %s", str(dep));
                puts(yyjson_get_bool(yyjson_obj_get(v, "selected")) ? "  * output" : "");
            }
        }
        yyjson_doc_free(doc);
    }
    free(compiled); return ferror(stdout) ? 1 : 0;
}
