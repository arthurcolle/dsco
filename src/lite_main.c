#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void lite_usage(const char *prog) {
    fprintf(stderr,
            "dsco-lite v%s\n\n"
            "Usage: %s [--version] [--help]\n"
            "       %s --models-json\n"
            "       %s --tools-json\n"
            "       %s --tool-exec cwd '{}'\n"
            "       %s [dsco provider options] PROMPT\n",
            DSCO_VERSION, prog, prog, prog, prog, prog);
}

static void json_escape(FILE *out, const char *s) {
    if (!s) return;
    for (const char *p = s; *p; p++) {
        if (*p == '"') fputs("\\\"", out);
        else if (*p == '\\') fputs("\\\\", out);
        else if (*p == '\n') fputs("\\n", out);
        else if (*p == '\r') fputs("\\r", out);
        else if (*p == '\t') fputs("\\t", out);
        else fputc(*p, out);
    }
}

static bool json_string_needs_escape(const char *s) {
    if (!s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == '"' || *p == '\\') return true;
    }
    return false;
}

static void print_tool_result_json(bool ok, const char *result) {
    const char *prefix = ok ? "{\"ok\":true,\"result\":\""
                            : "{\"ok\":false,\"result\":\"";
    if (!json_string_needs_escape(result)) {
        (void)write(STDOUT_FILENO, prefix, strlen(prefix));
        if (result) (void)write(STDOUT_FILENO, result, strlen(result));
        (void)write(STDOUT_FILENO, "\"}\n", 3);
        return;
    }

    fputs(prefix, stdout);
    json_escape(stdout, result);
    fputs("\"}\n", stdout);
}

static int print_models_json(void) {
    printf("[");
    for (int j = 0; MODEL_REGISTRY[j].alias; j++) {
        const model_info_t *m = &MODEL_REGISTRY[j];
        if (j > 0) printf(",");
        printf("{\"alias\":\"%s\",\"model_id\":\"%s\","
               "\"context_window\":%d,\"max_output\":%d,"
               "\"input_price\":%.2f,\"output_price\":%.2f,"
               "\"cache_read_price\":%.2f,\"cache_write_price\":%.2f,"
               "\"supports_thinking\":%d}",
               m->alias, m->model_id, m->context_window, m->max_output,
               m->input_price, m->output_price,
               m->cache_read_price, m->cache_write_price,
               m->supports_thinking);
    }
    printf("]\n");
    return 0;
}

static int print_tools_json(void) {
    puts("[{\"name\":\"cwd\",\"description\":\"Get current working directory.\","
         "\"input_schema\":{\"type\":\"object\",\"properties\":{}}}]");
    return 0;
}

static int run_tool_exec(const char *name, const char *input_json) {
    (void)input_json;
    if (!name || strcmp(name, "cwd") != 0) {
        print_tool_result_json(false, "tool not available in lite core profile");
        return 1;
    }
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        print_tool_result_json(false, strerror(errno));
        return 1;
    }
    print_tool_result_json(true, cwd);
    return 0;
}

static char *full_binary_path(const char *argv0) {
    const char *slash = argv0 ? strrchr(argv0, '/') : NULL;
    if (!slash) return strdup("dsco");
    size_t dir_len = (size_t)(slash - argv0);
    char *path = malloc(dir_len + strlen("/dsco") + 1);
    if (!path) return NULL;
    memcpy(path, argv0, dir_len);
    strcpy(path + dir_len, "/dsco");
    return path;
}

static int delegate_to_full(int argc, char **argv) {
    char *dsco = full_binary_path(argv[0]);
    if (!dsco) {
        fprintf(stderr, "dsco-lite: out of memory\n");
        return 1;
    }

    char **child = calloc((size_t)argc + 3, sizeof(char *));
    if (!child) {
        free(dsco);
        fprintf(stderr, "dsco-lite: out of memory\n");
        return 1;
    }
    int n = 0;
    child[n++] = dsco;
    child[n++] = "--profile";
    child[n++] = "lite";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            i++;
            continue;
        }
        child[n++] = argv[i];
    }
    child[n] = NULL;

    execv(dsco, child);
    execvp("dsco", child);
    fprintf(stderr, "dsco-lite: failed to delegate to dsco: %s\n", strerror(errno));
    free(child);
    free(dsco);
    return 127;
}

int main(int argc, char **argv) {
    if (argc <= 1) {
        lite_usage(argv[0]);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("dsco-lite v%s (built %s, %s)\n",
                   DSCO_VERSION, BUILD_DATE, GIT_HASH);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            lite_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--models-json") == 0) {
            return print_models_json();
        }
        if (strcmp(argv[i], "--tools-json") == 0) {
            return print_tools_json();
        }
        if (strcmp(argv[i], "--tool-exec") == 0) {
            if (i + 2 >= argc) {
                fprintf(stderr, "error: --tool-exec requires <name> <json>\n");
                return 1;
            }
            return run_tool_exec(argv[i + 1], argv[i + 2]);
        }
    }

    return delegate_to_full(argc, argv);
}
