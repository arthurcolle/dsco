#include "tools.h"
#include "llm.h"
#include "json_util.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SPINE_NAME "spine-dsco-slim"
#define LINE_CAP 65536u
#define RESULT_CAP (1024u * 1024u)

/* Legacy tool runtime linkage. The slim binary owns these process-wide roots
 * because it deliberately excludes main.c and agent.c. */
__attribute__((visibility("default"))) vm_t g_vm;
__attribute__((visibility("default"))) volatile int g_interrupted = 0;
__attribute__((visibility("default"))) int g_cheap_mode = 0;
__attribute__((visibility("default"))) double g_cost_budget = 0.0;

typedef struct {
    const char *tier;
    char *result;
    size_t result_cap;
    int last_status;
    bool stop_on_error;
    const char *model;
    const char *schema;
} spine_t;

#define EACH(n) for (size_t i = 0, n_ = (n); i < n_; ++i)
#define CLEANUP(p) do { free(p); (p) = NULL; } while (0)
#define FAIL_IF(c, ...) do { if (c) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); return 1; } } while (0)

typedef struct { const char *alias, *program; } alias_t;
static const alias_t aliases[] = {
    {"py", "python3"}, {"http", "curl --fail-with-body --show-error --location"},
    {"ftp", "curl --fail-with-body --show-error"}, {"remote", "ssh"},
    {"copy", "rsync -az"}, {"tcp", "nc"}, {"store", "sqlite3"},
    {"json", "jq"}, {"search", "rg"}, {"build", "make"}, {NULL, NULL}
};

static void usage(FILE *out) {
    fprintf(out, "usage: %s [-e] [-t tier] [-m model] [-S schema] [-c command] [-f script]\n", SPINE_NAME);
    fputs("  :tool JSON   governed DSCO tool\n"
          "  @name args   capability alias (py,http,ftp,remote,copy,tcp,store,json,search,build)\n"
          "  ? prompt     LLM completion\n  ?? prompt    structured LLM completion (-S schema)\n"
          "  .status      print previous status\n  .tier        print trust tier\n", out);
}

static char *json_quote(const char *s) {
    size_t n = 3;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        n += (*p == '"' || *p == '\\' || *p < 0x20) ? 6 : 1;
    char *z = malloc(n);
    if (!z) return NULL;
    char *q = z; *q++ = '"';
    static const char hex[] = "0123456789abcdef";
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') { *q++ = '\\'; *q++ = (char)*p; }
        else if (*p < 0x20) {
            *q++ = '\\'; *q++ = 'u'; *q++ = '0'; *q++ = '0';
            *q++ = hex[*p >> 4]; *q++ = hex[*p & 15];
        } else *q++ = (char)*p;
    }
    *q++ = '"'; *q = 0;
    return z;
}

static int invoke(spine_t *s, const char *name, const char *json) {
    s->result[0] = 0;
    bool ok = tools_execute_for_tier(name, json, s->tier, s->result, s->result_cap);
    FILE *out = ok ? stdout : stderr;
    if (s->result[0]) {
        fputs(s->result, out);
        size_t n = strlen(s->result);
        if (!n || s->result[n - 1] != '\n') fputc('\n', out);
    }
    return ok ? 0 : 1;
}

static void stream_out(const char *text, void *ctx) { (void)ctx; fputs(text, stdout); fflush(stdout); }

static int complete(spine_t *s, const char *prompt, bool structured) {
    const char *key = getenv("ANTHROPIC_API_KEY");
    if (!key || !*key) key = getenv("OPENROUTER_API_KEY");
    FAIL_IF(!key || !*key, "missing ANTHROPIC_API_KEY or OPENROUTER_API_KEY");
    conversation_t conv; conv_init(&conv); conv_add_user_text(&conv, prompt);
    session_state_t ss; session_state_init(&ss, s->model);
    ss.direct_answer_mode = true;
    if (structured) {
        FAIL_IF(!s->schema || !*s->schema, "structured completion requires -S schema");
        ss.structured_output = ss.structured_output_strict = true;
        snprintf(ss.structured_output_name, sizeof ss.structured_output_name, "spine_result");
        snprintf(ss.structured_output_schema, sizeof ss.structured_output_schema, "%s", s->schema);
    }
    char *request = llm_build_request_ex(&conv, &ss, 4096);
    if (!request) { conv_free(&conv); return 1; }
    stream_result_t r = llm_stream(key, request, stream_out, NULL, NULL, NULL, NULL);
    fputc('\n', stdout);
    if (structured && r.ok) {
        EACH((size_t)r.parsed.count) if (r.parsed.blocks[i].text) {
            json_validation_t v = json_validate_schema(r.parsed.blocks[i].text, s->schema);
            if (!v.valid) { fprintf(stderr, "schema: %s\n", v.error); r.ok = false; }
        }
    }
    free(r.actual_model); free(r.generation_id); json_free_response(&r.parsed);
    free(request); conv_free(&conv);
    return r.ok ? 0 : 1;
}

static int run_line(spine_t *s, char *line) {
    while (*line == ' ' || *line == '\t') ++line;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!*line) return 0;
    if (!strcmp(line, "exit")) return 1000;
    if (*line == '?') {
        bool structured = line[1] == '?';
        char *prompt = line + (structured ? 2 : 1);
        while (*prompt == ' ' || *prompt == '\t') ++prompt;
        return complete(s, prompt, structured);
    }
    if (!strcmp(line, ".status")) { printf("%d\n", s->last_status); return 0; }
    if (!strcmp(line, ".tier")) { puts(s->tier); return 0; }
    if (!strcmp(line, ".help")) { usage(stdout); return 0; }
    if (!strncmp(line, "cd ", 3)) {
        const char *path = line + 3;
        while (*path == ' ' || *path == '\t') ++path;
        if (chdir(path) == 0) return 0;
        fprintf(stderr, "cd: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (*line == '@') {
        char *name = line + 1, *args = strpbrk(name, " \t");
        if (args) { *args++ = 0; while (*args == ' ' || *args == '\t') ++args; }
        else args = "";
        const char *program = NULL;
        for (const alias_t *a = aliases; a->alias; ++a)
            if (!strcmp(name, a->alias)) { program = a->program; break; }
        if (!program) { fprintf(stderr, "unknown alias: %s\n", name); return 127; }
        size_t cap = strlen(program) + strlen(args) + 2;
        char *expanded = malloc(cap);
        if (!expanded) return 1;
        snprintf(expanded, cap, "%s%s%s", program, *args ? " " : "", args);
        int rc = run_line(s, expanded);
        free(expanded);
        return rc;
    }
    if (*line == ':') {
        char *name = line + 1;
        char *json = strpbrk(name, " \t");
        if (json) { *json++ = 0; while (*json == ' ' || *json == '\t') ++json; }
        else json = "{}";
        if (!*name) { fputs("empty tool name\n", stderr); return 2; }
        return invoke(s, name, *json ? json : "{}");
    }
    char *quoted = json_quote(line);
    if (!quoted) { fputs("out of memory\n", stderr); return 1; }
    size_t cap = strlen(quoted) + 16;
    char *json = malloc(cap);
    if (!json) { free(quoted); fputs("out of memory\n", stderr); return 1; }
    snprintf(json, cap, "{\"command\":%s}", quoted);
    int rc = invoke(s, "Bash", json);
    free(json); free(quoted);
    return rc;
}

int main(int argc, char **argv) {
    const char *command = NULL, *script = NULL, *schema = NULL;
    const char *model = getenv("DSCO_MODEL");
    if (!model || !*model) model = "claude-sonnet-4-5-20250929";
    bool stop_on_error = false;
    const char *tier = getenv("DSCO_TRUST_TIER");
    if (!tier || !*tier) tier = "trusted";
    int opt;
    while ((opt = getopt(argc, argv, "c:f:t:m:S:eh")) != -1) {
        if (opt == 'c') command = optarg;
        else if (opt == 'f') script = optarg;
        else if (opt == 'e') stop_on_error = true;
        else if (opt == 'm') model = optarg;
        else if (opt == 'S') schema = optarg;
        else if (opt == 't') tier = optarg;
        else { usage(opt == 'h' ? stdout : stderr); return opt == 'h' ? 0 : 2; }
    }
    if (optind != argc || (command && script)) { usage(stderr); return 2; }

    spine_t s = {.tier = tier, .result_cap = RESULT_CAP, .stop_on_error = stop_on_error,
                 .model = model, .schema = schema};
    s.result = malloc(s.result_cap);
    if (!s.result) { fputs("out of memory\n", stderr); return 1; }
    vm_init(&g_vm);
    tools_init_local_only();
    tools_register_vm_dispatch(&g_vm);

    int rc = 0;
    if (command) {
        char *copy = strdup(command);
        if (!copy) rc = 1;
        else { rc = run_line(&s, copy); free(copy); }
    } else {
        FILE *in = stdin;
        if (script && !(in = fopen(script, "r"))) {
            fprintf(stderr, "%s: %s\n", script, strerror(errno));
            free(s.result); return 1;
        }
        char *line = malloc(LINE_CAP);
        if (!line) rc = 1;
        else {
            bool tty = !script && isatty(STDIN_FILENO);
            while (tty ? (fputs("spine> ", stderr), fflush(stderr), true) : true) {
                if (!fgets(line, LINE_CAP, in)) break;
                int one = run_line(&s, line);
                if (one == 1000) break;
                s.last_status = rc = one;
                if (s.stop_on_error && rc) break;
            }
            free(line);
        }
        if (script) fclose(in);
    }
    free(s.result);
    return rc == 1000 ? 0 : rc;
}
