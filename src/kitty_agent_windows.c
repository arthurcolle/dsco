#include "kitty_agent_windows.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
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

#define KITTY_AGENT_WINDOW_CAP 1024

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
    return env_enabled(setting);
}

static const char *window_glyph(int child_id) {
    static const char *glyphs[] = {"⍼", "⧉", "⌬", "⦇", "🝮", "⧖"};
    return glyphs[(unsigned)child_id % 6];
}

static const char *window_location(void) {
    const char *setting = getenv("DSCO_KITTY_AGENT_WINDOW_LOCATION");
    if (setting && (!strcmp(setting, "vsplit") || !strcmp(setting, "hsplit")))
        return setting;
    return "split";
}

static const char *window_type(void) {
    const char *setting = getenv("DSCO_KITTY_AGENT_WINDOW_TYPE");
    return setting && !strcmp(setting, "os-window") ? "os-window" : "window";
}

static bool windows_keep_open(void) {
    return env_enabled(getenv("DSCO_KITTY_AGENT_WINDOWS_KEEP_OPEN"));
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
    if (!cap)
        return;
    int n = snprintf(dst, cap, "%s dsco · %02d ", window_glyph(child_id), child_id);
    size_t off = n > 0 ? (size_t)n : 0;
    if (off >= cap)
        off = cap - 1;
    const char *signature = getenv("DSCO_KITTY_SIGNATURE");
    const char *parts[] = {signature && signature[0] ? signature : "dsco",
                           " · ", prefix ? prefix : "", " · ", task ? task : "worker"};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        for (const unsigned char *p = (const unsigned char *)parts[i];
             *p && off + 1 < cap; p++) {
            if (*p >= 0x20 && *p != 0x7f)
                dst[off++] = (char)*p;
        }
    }
    /* snprintf and byte-oriented clipping may stop inside the final glyph. */
    if (off) {
        size_t start = off - 1;
        while (start && ((unsigned char)dst[start] & 0xc0) == 0x80)
            start--;
        unsigned char lead = (unsigned char)dst[start];
        size_t width = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
        if (off - start < width)
            off = start;
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
    bool done = false, timed_out = false;
    size_t total = 0;
    if (capture_stdout) fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double deadline = (double)ts.tv_sec + ts.tv_nsec / 1e9 + 0.75;
    while (!done) {
        if (capture_stdout) {
            char chunk[4096];
            for (int i = 0; i < 16; i++) {
                ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
                if (n <= 0) break;
                size_t keep = (size_t)n;
                size_t room = output_cap > total + 1 ? output_cap - total - 1 : 0;
                if (keep > room) keep = room;
                if (keep) { memcpy(output + total, chunk, keep); total += keep; }
            }
        }
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) { done = true; break; }
        if (w < 0 && errno != EINTR) { timed_out = true; break; }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if ((double)ts.tv_sec + ts.tv_nsec / 1e9 >= deadline) { timed_out = true; break; }
        struct timespec pause = {.tv_nsec = 1000000}; nanosleep(&pause, NULL);
    }
    if (timed_out && !done) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    if (capture_stdout) {
        char chunk[4096]; ssize_t n;
        for (int i = 0; i < 16 && (n = read(pipefd[0], chunk, sizeof(chunk))) > 0; i++) {
            size_t keep = (size_t)n, room = output_cap > total + 1 ? output_cap - total - 1 : 0;
            if (keep > room) keep = room;
            if (keep) { memcpy(output + total, chunk, keep); total += keep; }
        }
        if (output_cap) output[total] = 0;
        close(pipefd[0]);
    }
    return !timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
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
        if (window->escape_state == 2 || window->escape_state == 4) { /* OSC / control string */
            if (c == '\a' && window->escape_state == 2) {
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
        if (window->escape_state == 1) { /* ESC introducer */
            if (c == '[') {
                window->escape_state = 3;
            } else if (c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_') {
                window->escape_state = c == ']' ? 2 : 4;
                window->osc_escape = false;
            } else if (c >= 0x20 && c <= 0x2f) {
                window->escape_state = 5;
            } else {
                window->escape_state = 0;
            }
            continue;
        }
        if (window->escape_state == 3 || window->escape_state == 5) {
            if (c == 0x1b)
                window->escape_state = 1;
            else if (c >= (window->escape_state == 3 ? 0x40 : 0x30) && c <= 0x7e)
                window->escape_state = 0;
            continue;
        }
        if (c == 0x1b) {
            window->escape_state = 1;
            continue;
        }
        if (c == 0x7f || (c < 0x20 && c != '\n' && c != '\t' && c != '\r'))
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

/* Decorations are trusted; metadata passes through the same escape filter as
 * streamed model output. Incomplete escapes must not eat the next label. */
static void write_label(kitty_agent_window_t *window, const char *label,
                        const char *value) {
    window->escape_state = 0;
    window->osc_escape = false;
    write_all(window->log_fd, label, strlen(label));
    append_terminal_safe(window, value, strlen(value));
    window->escape_state = 0;
    window->osc_escape = false;
    write_all(window->log_fd, "\033[0m\n", 5);
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
        bool reaped = false;
        for (int i = 0; i < 20; i++) {
            if (waitpid(window->kitty_pid, NULL, WNOHANG) == window->kitty_pid) {
                reaped = true;
                break;
            }
            usleep(10000);
        }
        if (!reaped) {
            kill(window->kitty_pid, SIGKILL);
            waitpid(window->kitty_pid, NULL, 0);
        }
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
    snprintf(window->log_path, sizeof(window->log_path), "%s/child-%d-XXXXXX", dir, child_id);
    window->log_fd = mkstemp(window->log_path);
    if (window->log_fd >= 0) fcntl(window->log_fd, F_SETFD, FD_CLOEXEC);
    if (window->log_fd < 0) {
        window->used = false;
        return;
    }

    const char *signature = getenv("DSCO_KITTY_SIGNATURE");
    char heading[192];
    snprintf(heading, sizeof(heading),
             "\033[38;2;104;211;225m  %s  \033[1mSWARM / %02d\033[0m"
             "\033[38;2;115;127;155m  ·  pid %d\033[0m\n",
             window_glyph(child_id), child_id, (int)child_pid);
    write_all(window->log_fd, heading, strlen(heading));
    write_label(window, "\033[38;2;197;168;255m  ",
                signature && signature[0] ? signature : "dsco");
    write_label(window, "\033[38;2;115;127;155m  task   \033[0m", task ? task : "worker");
    write_label(window, "\033[38;2;115;127;155m  model  \033[0m",
                model && model[0] ? model : "inherited");
    const char *rule = "\033[38;2;58;75;100m  ────────────────────────────────\033[0m\n\n";
    write_all(window->log_fd, rule, strlen(rule));

    char title[256];
    safe_title(title, sizeof(title), "RUNNING", child_id, task);
    const char *listen_on = getenv("KITTY_LISTEN_ON");
    const char *source_id = getenv("KITTY_WINDOW_ID");
    if (listen_on && listen_on[0]) {
        char source[48], tab_match[48];
        snprintf(source, sizeof(source), "id:%s", source_id && source_id[0] ? source_id : "0");
        snprintf(tab_match, sizeof(tab_match), "window_id:%s", source_id && source_id[0] ? source_id : "0");
        char response[64] = {0};
        char *argv[40];
        int j = 0;
        argv[j++] = NULL;
        argv[j++] = "@";
        argv[j++] = "--to";
        argv[j++] = (char *)listen_on;
        argv[j++] = "launch";
        argv[j++] = "--type";
        argv[j++] = (char *)window_type();
        argv[j++] = "--location";
        argv[j++] = (char *)window_location();
        argv[j++] = "--spacing";
        argv[j++] = "padding=12";
        argv[j++] = "--dont-take-focus";
        argv[j++] = "--copy-colors";
        argv[j++] = "--title";
        argv[j++] = title;
        if (source_id && source_id[0]) {
            argv[j++] = "--source-window";
            argv[j++] = source;
            argv[j++] = "--match";
            argv[j++] = tab_match;
            argv[j++] = "--next-to";
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
    snprintf(footer, sizeof(footer),
             "\n\033[38;2;%sm  %s  EXIT %d  ·  ",
             exit_code == 0 ? "142;218;172" : "244;137;153",
             window_glyph(child_id), exit_code);
    write_label(window, footer, status ? status : "complete");
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
    if (!windows_keep_open())
        close_window(window);
}

void kitty_agent_windows_shutdown(void) {
    for (int i = 0; i < KITTY_AGENT_WINDOW_CAP; i++) {
        if (windows_keep_open()) {
            if (s_windows[i].used && s_windows[i].log_fd >= 0) close(s_windows[i].log_fd);
            memset(&s_windows[i], 0, sizeof(s_windows[i]));
            s_windows[i].log_fd = -1;
        } else close_window(&s_windows[i]);
    }
}
