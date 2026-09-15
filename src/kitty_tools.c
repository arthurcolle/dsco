#include "kitty_tools.h"

#include "json_util.h"
#include "process_capture.h"

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

static char *decode_json_string(const char *p) {
    if (!p || *p != '"') return NULL;
    const char *end = p + 1;
    while (*end && *end != '"') {
        if (*end == '\\') { end++; if (!*end) return NULL; }
        end++;
    }
    if (*end != '"' || (size_t)(end - p) > 65536) return NULL;
    jbuf_t b; jbuf_init(&b, 64);
    jbuf_append(&b, "{\"value\":");
    jbuf_append_len(&b, p, (size_t)(end - p + 1));
    jbuf_append(&b, "}");
    /* argv cannot represent embedded NUL bytes. */
    char *out = NULL;
    if (!strstr(b.data, "\\u0000") && json_is_valid_container(b.data))
        out = json_get_str(b.data, "value");
    jbuf_free(&b);
    return out;
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

bool tool_kitty_remote(const char *input_json, char *result, size_t result_len) {
    if (!input_json || !json_is_valid_container(input_json)) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"invalid JSON input\"}");
        return false;
    }
    char *raw_args = json_get_raw(input_json, "args");
    bool bad_args = raw_args && raw_args[0] != '[';
    free(raw_args);
    if (bad_args) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"args must be an array\"}");
        return false;
    }
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
    /* Tool execution must never fall back to /dev/tty: the compositor owns
     * that input stream. Kitty's implicit terminal transport races its reader
     * and can stall until the subprocess deadline. Resolve a socket explicitly
     * before spawning, including when KITTY_LISTEN_ON supplies the address. */
    const char *address = to && *to ? to : getenv("KITTY_LISTEN_ON");
    if (!address || !*address) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"kitty_socket_required\",\"detail\":\"Provide to or KITTY_LISTEN_ON; terminal fallback is disabled to preserve compositor input.\"}");
        free_args(&args); free(command); free(to);
        return false;
    }
    if (!((!strncmp(address, "unix:", 5) && address[5]) ||
          (!strncmp(address, "tcp:", 4) && address[4]) ||
          (!strncmp(address, "tcp6:", 5) && address[5])) ||
        strpbrk(address, "\r\n\033")) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"invalid_kitty_socket\",\"detail\":\"Use a unix:, tcp:, or tcp6: socket address, not terminal transport.\"}");
        free_args(&args); free(command); free(to);
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
    argv[n++] = "--to";
    argv[n++] = (char *)address;
    argv[n++] = command;
    for (int i = 0; i < args.count; i++)
        argv[n++] = args.items[i];
    argv[n] = NULL;
    bool ok = process_capture_json(path, argv, timeout_s * 1000, command, result, result_len);
    free_args(&args);
    free(command);
    free(to);
    return ok;
}

bool tool_kitten(const char *input_json, char *result, size_t result_len) {
    if (!input_json || !json_is_valid_container(input_json)) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"invalid JSON input\"}");
        return false;
    }
    char *raw_args = json_get_raw(input_json, "args");
    bool bad_args = raw_args && raw_args[0] != '[';
    free(raw_args);
    if (bad_args) {
        snprintf(result, result_len, "{\"ok\":false,\"error\":\"args must be an array\"}");
        return false;
    }
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
    int timeout = json_get_int(input_json, "timeout_seconds", 15);
    if (timeout < 1) timeout = 1;
    if (timeout > 300) timeout = 300;
    bool ok = process_capture_json(path, argv, timeout * 1000, command, result, result_len);
    free_args(&args);
    free(command);
    return ok;
}
