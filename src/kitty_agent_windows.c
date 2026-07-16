#include "kitty_agent_windows.h"

#include "pixel_tui.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define KITTY_AGENT_WINDOW_CAP 64

typedef struct {
    bool used;
    int log_fd;
    int kitty_window_id;
    pid_t kitty_pid;
    int escape_state;
    bool osc_escape;
    char task[128];
    char log_path[PATH_MAX];
} kitty_agent_window_t;

static kitty_agent_window_t s_windows[KITTY_AGENT_WINDOW_CAP];
static bool s_shutdown_registered;

static bool env_disabled(const char *value) {
    return value && (!strcmp(value, "0") || !strcasecmp(value, "false") ||
                     !strcasecmp(value, "no") || !strcasecmp(value, "off"));
}

static bool env_enabled(const char *value) {
    return value && value[0] && !env_disabled(value);
}

static bool windows_enabled(void) {
    const char *setting = getenv("DSCO_KITTY_AGENT_WINDOWS");
    if (setting)
        return env_enabled(setting);
    return pixel_tui_session_active();
}

static const char *find_kitty_tool(const char *name) {
    static char path[PATH_MAX];
    const char *candidates[] = {
        "/Applications/kitty.app/Contents/MacOS/kitten",
        "/Applications/kitty.app/Contents/MacOS/kitty",
        "/opt/homebrew/bin/kitten",
        "/opt/homebrew/bin/kitty",
        "/usr/local/bin/kitten",
        "/usr/local/bin/kitty",
        NULL,
    };
    bool want_kitten = !strcmp(name, "kitten");
    for (int i = want_kitten ? 0 : 1; candidates[i]; i += 2) {
        if (access(candidates[i], X_OK) == 0) {
            snprintf(path, sizeof(path), "%s", candidates[i]);
            return path;
        }
    }
    return NULL;
}

static void safe_title(char *dst, size_t cap, const char *prefix,
                       int child_id, const char *task) {
    int n = snprintf(dst, cap, "dsco agent %d %s ", child_id, prefix ? prefix : "");
    size_t off = n > 0 ? (size_t)n : 0;
    if (off >= cap)
        off = cap - 1;
    for (const unsigned char *p = (const unsigned char *)(task ? task : "worker");
         *p && off + 1 < cap; p++) {
        if (*p >= 0x20 && *p != 0x7f)
            dst[off++] = (char)*p;
    }
    dst[off] = '\0';
}

static int spawn_quiet(const char *path, char *const argv[], bool capture_stdout,
                       char *output, size_t output_cap, pid_t *running_pid) {
    int pipefd[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (capture_stdout) {
        if (pipe(pipefd) < 0) {
            posix_spawn_file_actions_destroy(&actions);
            return -1;
        }
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid = -1;
    int rc = posix_spawn(&pid, path, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (capture_stdout)
        close(pipefd[1]);
    if (rc != 0) {
        if (capture_stdout)
            close(pipefd[0]);
        return -1;
    }

    if (running_pid) {
        *running_pid = pid;
        if (capture_stdout)
            close(pipefd[0]);
        return 0;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (capture_stdout) {
        ssize_t total = 0;
        while ((size_t)total + 1 < output_cap) {
            ssize_t n = read(pipefd[0], output + total, output_cap - (size_t)total - 1);
            if (n <= 0)
                break;
            total += n;
        }
        output[total > 0 ? total : 0] = '\0';
        close(pipefd[0]);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int remote_command(char *argv[], char *output, size_t output_cap) {
    const char *kitten = find_kitty_tool("kitten");
    if (!kitten)
        return -1;
    argv[0] = (char *)kitten;
    return spawn_quiet(kitten, argv, output != NULL, output, output_cap, NULL);
}

static void write_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n > 0) {
            data += n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

/* Child output can contain model-controlled terminal sequences.  Preserve
 * text and UTF-8 while removing CSI/OSC controls before a Kitty sees it. */
static void append_terminal_safe(kitty_agent_window_t *window,
                                 const char *data, size_t len) {
    if (!window || window->log_fd < 0 || !data)
        return;
    char clean[4096];
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (window->escape_state == 2) { /* OSC */
            if (c == '\a') {
                window->escape_state = 0;
                window->osc_escape = false;
            } else if (window->osc_escape && c == '\\') {
                window->escape_state = 0;
                window->osc_escape = false;
            } else {
                window->osc_escape = c == 0x1b;
            }
            continue;
        }
        if (window->escape_state == 1) { /* CSI or short escape */
            if (c == ']') {
                window->escape_state = 2;
                window->osc_escape = false;
            } else if (c >= 0x40 && c <= 0x7e) {
                window->escape_state = 0;
            }
            continue;
        }
        if (c == 0x1b) {
            window->escape_state = 1;
            continue;
        }
        if (c < 0x20 && c != '\n' && c != '\t' && c != '\r')
            continue;
        clean[out++] = (char)c;
        if (out == sizeof(clean)) {
            write_all(window->log_fd, clean, out);
            out = 0;
        }
    }
    if (out)
        write_all(window->log_fd, clean, out);
}

static void close_window(kitty_agent_window_t *window) {
    if (!window || !window->used)
        return;
    if (window->log_fd >= 0) {
        close(window->log_fd);
        window->log_fd = -1;
    }
    const char *listen_on = getenv("KITTY_LISTEN_ON");
    if (window->kitty_window_id > 0 && listen_on && listen_on[0]) {
        char match[32];
        snprintf(match, sizeof(match), "id:%d", window->kitty_window_id);
        char *argv[] = {NULL, "@", "--to", (char *)listen_on,
                        "close-window", "--match", match, NULL};
        remote_command(argv, NULL, 0);
    } else if (window->kitty_pid > 0) {
        kill(window->kitty_pid, SIGTERM);
        waitpid(window->kitty_pid, NULL, WNOHANG);
    }
    memset(window, 0, sizeof(*window));
    window->log_fd = -1;
}

void kitty_agent_window_spawn(int child_id, pid_t child_pid,
                              const char *task, const char *model) {
    if (child_id < 0 || child_id >= KITTY_AGENT_WINDOW_CAP || !windows_enabled())
        return;
    kitty_agent_window_t *window = &s_windows[child_id];
    if (!s_shutdown_registered) {
        atexit(kitty_agent_windows_shutdown);
        s_shutdown_registered = true;
    }
    if (window->used)
        close_window(window);
    memset(window, 0, sizeof(*window));
    window->used = true;
    window->log_fd = -1;
    snprintf(window->task, sizeof(window->task), "%s", task ? task : "worker");

    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        window->used = false;
        return;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.dsco", home);
    mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.dsco/sessions", home);
    mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.dsco/sessions/swarm", home);
    mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/.dsco/sessions/swarm/%d", home, (int)getpid());
    mkdir(dir, 0700);
    chmod(dir, 0700);
    snprintf(window->log_path, sizeof(window->log_path), "%s/child-%d.live.log", dir, child_id);
    window->log_fd = open(window->log_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (window->log_fd < 0) {
        window->used = false;
        return;
    }

    char heading[512];
    int heading_len = snprintf(heading, sizeof(heading),
                               "DSCO SUBAGENT %d  PID %d\nTASK  %s\nMODEL %s\n\n",
                               child_id, (int)child_pid, task ? task : "worker",
                               model && model[0] ? model : "inherited");
    if (heading_len > 0)
        append_terminal_safe(window, heading, (size_t)heading_len);

    char title[256];
    safe_title(title, sizeof(title), "RUNNING", child_id, task);
    const char *listen_on = getenv("KITTY_LISTEN_ON");
    const char *source_id = getenv("KITTY_WINDOW_ID");
    if (listen_on && listen_on[0]) {
        char source[48];
        snprintf(source, sizeof(source), "id:%s", source_id && source_id[0] ? source_id : "0");
        char response[64] = {0};
        char *argv[32];
        int j = 0;
        argv[j++] = NULL;
        argv[j++] = "@";
        argv[j++] = "--to";
        argv[j++] = (char *)listen_on;
        argv[j++] = "launch";
        argv[j++] = "--type";
        argv[j++] = "os-window";
        argv[j++] = "--dont-take-focus";
        argv[j++] = "--copy-colors";
        argv[j++] = "--title";
        argv[j++] = title;
        if (source_id && source_id[0]) {
            argv[j++] = "--source-window";
            argv[j++] = source;
        }
        argv[j++] = "/usr/bin/tail";
        argv[j++] = "-n";
        argv[j++] = "+1";
        argv[j++] = "-F";
        argv[j++] = "--";
        argv[j++] = window->log_path;
        argv[j] = NULL;
        if (remote_command(argv, response, sizeof(response)) == 0)
            window->kitty_window_id = atoi(response);
    } else {
        const char *kitty = find_kitty_tool("kitty");
        if (kitty) {
            char *argv[] = {(char *)kitty, "--title", title,
                            "/usr/bin/tail", "-n", "+1", "-F", "--", window->log_path, NULL};
            spawn_quiet(kitty, argv, false, NULL, 0, &window->kitty_pid);
        }
    }
}

void kitty_agent_window_append(int child_id, const char *data, size_t len) {
    if (child_id < 0 || child_id >= KITTY_AGENT_WINDOW_CAP)
        return;
    kitty_agent_window_t *window = &s_windows[child_id];
    if (window->used)
        append_terminal_safe(window, data, len);
}

void kitty_agent_window_complete(int child_id, const char *status, int exit_code) {
    if (child_id < 0 || child_id >= KITTY_AGENT_WINDOW_CAP)
        return;
    kitty_agent_window_t *window = &s_windows[child_id];
    if (!window->used)
        return;
    char footer[160];
    int n = snprintf(footer, sizeof(footer), "\nDSCO AGENT %s  EXIT %d\n",
                     status ? status : "complete", exit_code);
    if (n > 0)
        append_terminal_safe(window, footer, (size_t)n);
    fsync(window->log_fd);

    const char *listen_on = getenv("KITTY_LISTEN_ON");
    if (window->kitty_window_id > 0 && listen_on && listen_on[0]) {
        char match[32], title[256];
        snprintf(match, sizeof(match), "id:%d", window->kitty_window_id);
        safe_title(title, sizeof(title), status ? status : "DONE", child_id, window->task);
        char *argv[] = {NULL, "@", "--to", (char *)listen_on,
                        "set-window-title", "--match", match, title, NULL};
        remote_command(argv, NULL, 0);
    }
}

void kitty_agent_windows_shutdown(void) {
    for (int i = 0; i < KITTY_AGENT_WINDOW_CAP; i++)
        close_window(&s_windows[i]);
}
