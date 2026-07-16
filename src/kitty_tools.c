#include "kitty_tools.h"

#include "json_util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define KITTY_ARG_CAP 64
#define KITTY_OUTPUT_CAP (1024 * 1024)

typedef struct {
    char *items[KITTY_ARG_CAP];
    int count;
    bool invalid;
} arg_list_t;

static const char *const s_remote_commands[] = {
    "action", "close-tab", "close-window", "create-marker", "detach-tab",
    "detach-window", "disable-ligatures", "env", "focus-tab", "focus-window",
    "get-colors", "get-text", "goto-layout", "kitten", "last-used-layout",
    "launch", "load-config", "ls", "new-window", "remove-marker",
    "resize-os-window", "resize-window", "run", "scroll-window", "select-window",
    "send-key", "send-text", "set-background-image", "set-background-opacity",
    "set-colors", "set-enabled-layouts", "set-font-size", "set-spacing",
    "set-tab-color", "set-tab-title", "set-user-vars", "set-window-logo",
    "set-window-title", "signal-child", NULL,
};

static const char *const s_kittens[] = {
    "update-self", "edit-in-kitty", "clipboard", "dnd", "icat", "ssh", "transfer",
    "panel", "quick-access-terminal", "unicode-input", "show-key", "desktop-ui",
    "mouse-demo", "hyperlinked-grep", "ask", "hints", "diff", "notify", "themes",
    "run-shell", "choose-fonts", "choose-files", "command-palette", "query-terminal", NULL,
};

static bool allowed(const char *value, const char *const *values) {
    if (!value)
        return false;
    for (int i = 0; values[i]; i++)
        if (!strcmp(value, values[i]))
            return true;
    return false;
}

static const char *kitty_binary(bool remote) {
    static const char *const kitten_paths[] = {
        "/Applications/kitty.app/Contents/MacOS/kitten", "/opt/homebrew/bin/kitten",
        "/usr/local/bin/kitten", "/usr/bin/kitten", NULL,
    };
    (void)remote;
    for (int i = 0; kitten_paths[i]; i++)
        if (access(kitten_paths[i], X_OK) == 0)
            return kitten_paths[i];
    return NULL;
}

static void append_utf8(jbuf_t *b, unsigned codepoint) {
    char out[4];
    size_t n = 0;
    if (codepoint <= 0x7f) {
        out[n++] = (char)codepoint;
    } else if (codepoint <= 0x7ff) {
        out[n++] = (char)(0xc0 | (codepoint >> 6));
        out[n++] = (char)(0x80 | (codepoint & 0x3f));
    } else {
        out[n++] = (char)(0xe0 | (codepoint >> 12));
        out[n++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        out[n++] = (char)(0x80 | (codepoint & 0x3f));
    }
    jbuf_append_len(b, out, n);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *decode_json_string(const char *p) {
    if (!p || *p != '"')
        return NULL;
    p++;
    jbuf_t out;
    jbuf_init(&out, 64);
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c != '\\') {
            if (c < 0x20) {
                jbuf_free(&out);
                return NULL;
            }
            jbuf_append_len(&out, (const char *)&c, 1);
            continue;
        }
        char esc = *p++;
        switch (esc) {
            case '"': jbuf_append_char(&out, '"'); break;
            case '\\': jbuf_append_char(&out, '\\'); break;
            case '/': jbuf_append_char(&out, '/'); break;
            case 'b': jbuf_append_char(&out, '\b'); break;
            case 'f': jbuf_append_char(&out, '\f'); break;
            case 'n': jbuf_append_char(&out, '\n'); break;
            case 'r': jbuf_append_char(&out, '\r'); break;
            case 't': jbuf_append_char(&out, '\t'); break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    int h = hex_value(*p++);
                    if (h < 0) {
                        jbuf_free(&out);
                        return NULL;
                    }
                    cp = (cp << 4) | (unsigned)h;
                }
                if (cp >= 0xd800 && cp <= 0xdfff)
                    cp = 0xfffd;
                append_utf8(&out, cp);
                break;
            }
            default:
                jbuf_free(&out);
                return NULL;
        }
    }
    if (*p != '"') {
        jbuf_free(&out);
        return NULL;
    }
    return out.data;
}

static void collect_arg(const char *element, void *ctx) {
    arg_list_t *args = ctx;
    if (args->count >= KITTY_ARG_CAP) {
        args->invalid = true;
        return;
    }
    char *decoded = decode_json_string(element);
    if (!decoded || strlen(decoded) > 16384) {
        free(decoded);
        args->invalid = true;
        return;
    }
    args->items[args->count++] = decoded;
}

static void free_args(arg_list_t *args) {
    for (int i = 0; i < args->count; i++)
        free(args->items[i]);
    memset(args, 0, sizeof(*args));
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool run_capture(const char *path, char *const argv[], int timeout_s,
                        const char *command, char *result, size_t result_len) {
    int pipes[2];
    if (pipe(pipes) < 0) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"pipe failed\"}");
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    pid_t pid = -1;
    int rc = posix_spawn(&pid, path, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (rc != 0) {
        close(pipes[0]);
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"spawn failed: %s\"}",
                 strerror(rc));
        return false;
    }
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    jbuf_t output;
    jbuf_init(&output, 4096);
    bool truncated = false, timed_out = false, child_done = false;
    int status = 0;
    double deadline = monotonic_seconds() + timeout_s;
    while (!child_done) {
        struct pollfd pfd = {.fd = pipes[0], .events = POLLIN | POLLHUP};
        poll(&pfd, 1, 50);
        char buf[8192];
        ssize_t n;
        while ((n = read(pipes[0], buf, sizeof(buf))) > 0) {
            size_t room = output.len < KITTY_OUTPUT_CAP ? KITTY_OUTPUT_CAP - output.len : 0;
            if ((size_t)n > room) {
                n = (ssize_t)room;
                truncated = true;
            }
            if (n > 0)
                jbuf_append_len(&output, buf, (size_t)n);
        }
        pid_t w = waitpid(pid, &status, WNOHANG);
        child_done = w == pid;
        if (!child_done && monotonic_seconds() >= deadline) {
            timed_out = true;
            kill(pid, SIGTERM);
            usleep(50000);
            if (waitpid(pid, &status, WNOHANG) == 0) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            child_done = true;
        }
    }
    char buf[8192];
    ssize_t n;
    while ((n = read(pipes[0], buf, sizeof(buf))) > 0) {
        size_t room = output.len < KITTY_OUTPUT_CAP ? KITTY_OUTPUT_CAP - output.len : 0;
        if ((size_t)n > room) {
            n = (ssize_t)room;
            truncated = true;
        }
        if (n > 0)
            jbuf_append_len(&output, buf, (size_t)n);
    }
    close(pipes[0]);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    bool ok = !timed_out && exit_code == 0;
    jbuf_t response;
    jbuf_init(&response, output.len + 256);
    jbuf_append(&response, "{\"ok\":");
    jbuf_append(&response, ok ? "true" : "false");
    jbuf_append(&response, ",\"command\":");
    jbuf_append_json_str(&response, command);
    jbuf_appendf(&response, ",\"exit_code\":%d,\"timed_out\":%s,\"truncated\":%s,\"output\":",
                 exit_code, timed_out ? "true" : "false", truncated ? "true" : "false");
    jbuf_append_json_str(&response, output.data ? output.data : "");
    jbuf_append(&response, "}");
    snprintf(result, result_len, "%s", response.data);
    jbuf_free(&response);
    jbuf_free(&output);
    return ok;
}

static bool run_interactive(const char *path, char *const argv[], const char *command,
                            char *result, size_t result_len) {
    pid_t pid = -1;
    int rc = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    if (rc != 0) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"spawn failed: %s\"}",
                 strerror(rc));
        return false;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    snprintf(result, result_len, "{\"ok\":%s,\"command\":\"%s\",\"exit_code\":%d}",
             exit_code == 0 ? "true" : "false", command, exit_code);
    return exit_code == 0;
}

bool tool_kitty_remote(const char *input_json, char *result, size_t result_len) {
    char *command = json_get_str(input_json ? input_json : "{}", "command");
    char *to = json_get_str(input_json ? input_json : "{}", "to");
    int timeout_s = json_get_int(input_json ? input_json : "{}", "timeout_seconds", 15);
    if (timeout_s < 1) timeout_s = 1;
    if (timeout_s > 300) timeout_s = 300;
    if (!allowed(command, s_remote_commands)) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"unknown Kitty remote command\"}");
        free(command);
        free(to);
        return false;
    }
    arg_list_t args = {0};
    json_array_foreach(input_json ? input_json : "{}", "args", collect_arg, &args);
    if (args.invalid) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"args must be at most 64 JSON strings\"}");
        free_args(&args);
        free(command);
        free(to);
        return false;
    }
    const char *path = kitty_binary(true);
    if (!path) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"kitten executable not found\"}");
        free_args(&args);
        free(command);
        free(to);
        return false;
    }
    char *argv[KITTY_ARG_CAP + 12];
    int n = 0;
    argv[n++] = (char *)path;
    argv[n++] = "@";
    if (to && to[0]) {
        argv[n++] = "--to";
        argv[n++] = to;
    }
    argv[n++] = command;
    for (int i = 0; i < args.count; i++)
        argv[n++] = args.items[i];
    argv[n] = NULL;
    bool ok = run_capture(path, argv, timeout_s + 1, command, result, result_len);
    free_args(&args);
    free(command);
    free(to);
    return ok;
}

bool tool_kitten(const char *input_json, char *result, size_t result_len) {
    char *command = json_get_str(input_json ? input_json : "{}", "command");
    if (!allowed(command, s_kittens)) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"unknown kitten command\"}");
        free(command);
        return false;
    }
    arg_list_t args = {0};
    json_array_foreach(input_json ? input_json : "{}", "args", collect_arg, &args);
    if (args.invalid) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"args must be at most 64 JSON strings\"}");
        free_args(&args);
        free(command);
        return false;
    }
    const char *path = kitty_binary(false);
    if (!path) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"kitten executable not found\"}");
        free_args(&args);
        free(command);
        return false;
    }
    char *argv[KITTY_ARG_CAP + 4];
    int n = 0;
    argv[n++] = (char *)path;
    argv[n++] = command;
    for (int i = 0; i < args.count; i++)
        argv[n++] = args.items[i];
    argv[n] = NULL;
    bool ok = run_interactive(path, argv, command, result, result_len);
    free_args(&args);
    free(command);
    return ok;
}
