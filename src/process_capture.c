#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#include "process_capture.h"
#include "json_util.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;

static double capture_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000 + ts.tv_nsec / 1e6;
}

void process_capture_free(process_capture_t *out) {
    if (out) { free(out->output); memset(out, 0, sizeof(*out)); }
}

static void signal_capture(pid_t pid, bool reaped, bool group_gone, int signal_number) {
    if (!group_gone) kill(-pid, signal_number);
    /* A pipe child can move into another process group. The unreaped direct
     * PID is still ours, but its new group is not: never signal that group.
     * Always target the owned root at hard kill, including a group-move race. */
    if (!reaped && (signal_number == SIGKILL || group_gone || getpgid(pid) != pid))
        kill(pid, signal_number);
}

static bool capture_with_fd(const char *path, char *const argv[], int timeout_ms,
                            size_t max_output, process_capture_t *out, int input_fd) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;
    if (!path || !argv || timeout_ms < 1 || max_output > 16 * 1024 * 1024) {
        out->spawn_error = EINVAL; return false;
    }
    int fds[2];
    if (pipe(fds)) { out->spawn_error = errno; return false; }
    for (int i = 0; i < 2; i++) {
        int safe_fd = fcntl(fds[i], F_DUPFD_CLOEXEC, 3);
        if (safe_fd < 0) {
            out->spawn_error = errno; close(fds[0]); close(fds[1]); return false;
        }
        close(fds[i]); fds[i] = safe_fd;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (input_fd >= 0) {
        posix_spawn_file_actions_adddup2(&fa, input_fd, STDIN_FILENO);
        if (input_fd != STDIN_FILENO)
            posix_spawn_file_actions_addclose(&fa, input_fd);
    } else {
        posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#endif
#endif
    /* The caller may ignore or block termination signals (for example while
     * running the agent's own signal loop). Helpers need normal Unix signal
     * behavior, independent of whichever worker thread launched them. */
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigfillset(&defaults);
    sigdelset(&defaults, SIGKILL);
    sigdelset(&defaults, SIGSTOP);
    int rc = posix_spawnattr_setflags(&attr, flags);
    if (!rc) rc = posix_spawnattr_setpgroup(&attr, 0);
    if (!rc) rc = posix_spawnattr_setsigmask(&attr, &empty);
    if (!rc) rc = posix_spawnattr_setsigdefault(&attr, &defaults);
    pid_t pid;
    if (!rc) rc = posix_spawn(&pid, path, &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(fds[1]);
    if (rc) { close(fds[0]); out->spawn_error = rc; return false; }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    out->output = calloc(max_output + 1, 1);
    if (!out->output) {
        signal_capture(pid, false, false, SIGKILL); close(fds[0]);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        out->spawn_error = ENOMEM; return false;
    }
    bool done = false, eof = false, closing = false, killed = false, group_gone = false;
    int status = -1;
    double deadline = capture_ms() + timeout_ms;
    double kill_at = 0, drain_deadline = 0;
    for (;;) {
        char chunk[8192];
        ssize_t n;
        /* Bound each drain so an endless writer cannot evade the deadline. */
        for (int i = 0; i < 64; i++) {
            n = read(fds[0], chunk, sizeof(chunk));
            if (n == 0) { eof = true; break; }
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) eof = true;
                break;
            }
            size_t keep = (size_t)n;
            if (keep > max_output - out->length) {
                keep = max_output - out->length; out->truncated = true;
            }
            memcpy(out->output + out->length, chunk, keep);
            out->length += keep;
        }
        if (!done) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) done = true;
            else if (w < 0 && errno != EINTR) {
                done = true; out->spawn_error = errno;
            }
        }
        /* A descendant can close stdout/stderr and outlive a successful root.
         * One-shot captures own their private group until cleanup completes.
         * Latch disappearance so a retired group ID is never signalled again. */
        if (!group_gone && kill(-pid, 0) < 0 && errno == ESRCH) group_gone = true;
        if (done && eof && group_gone) break;
        double now = capture_ms();
        if (!closing && ((done && eof) || now >= deadline)) {
            closing = true;
            out->timed_out = !(done && eof);
            signal_capture(pid, done, group_gone, SIGTERM);
            kill_at = now + 250.0;
        }
        if (closing && !killed && now >= kill_at) {
            signal_capture(pid, done, group_gone, SIGKILL);
            killed = true;
            drain_deadline = now + 100.0;
        }
        /* Keep draining during both stages: a TERM handler can flush more
         * than a pipe buffer. Bound the final drain if an escaped descendant
         * retains the write end, and keep the same output-size limit. */
        if (killed && now >= drain_deadline) break;
        struct pollfd pfd = {.fd = fds[0], .events = POLLIN | POLLHUP};
        if (eof) { struct timespec pause = {.tv_nsec = 1000000}; nanosleep(&pause, NULL); }
        else poll(&pfd, 1, 10);
    }
    close(fds[0]);
    if (!done) {
        pid_t waited;
        do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
        if (waited < 0) out->spawn_error = errno;
    }
    out->output[out->length] = 0;
    if (!out->spawn_error && status >= 0) {
        if (WIFEXITED(status)) out->exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) out->exit_code = 128 + WTERMSIG(status);
    }
    return !out->spawn_error && !out->timed_out && out->exit_code == 0;
}

bool process_capture(const char *path, char *const argv[], int timeout_ms,
                     size_t max_output, process_capture_t *out) {
    return capture_with_fd(path, argv, timeout_ms, max_output, out, -1);
}

bool process_capture_input(const char *path, char *const argv[], const char *input,
                           size_t input_len, int timeout_ms, size_t max_output,
                           process_capture_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;
    if (!input || input_len > 64 * 1024) {
        out->spawn_error = EINVAL;
        return false;
    }
    FILE *request = tmpfile();
    if (!request) { out->spawn_error = errno; return false; }
    fcntl(fileno(request), F_SETFD, FD_CLOEXEC);
    if (fwrite(input, 1, input_len, request) != input_len || fflush(request) ||
        fseek(request, 0, SEEK_SET)) {
        out->spawn_error = errno ? errno : EIO;
        fclose(request);
        return false;
    }
    int input_fd = fcntl(fileno(request), F_DUPFD_CLOEXEC, 3);
    if (input_fd < 0) { out->spawn_error = errno; fclose(request); return false; }
    fclose(request);
    bool ok = capture_with_fd(path, argv, timeout_ms, max_output, out, input_fd);
    close(input_fd);
    return ok;
}

/* Text remains valid JSON/UTF-8 even for binary child output. When replacement
 * is needed, an exact base64 representation accompanies it. */
static bool capture_json_bytes(jbuf_t *b, const unsigned char *s, size_t n) {
    bool binary = false;
    jbuf_append_char(b, '"');
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        if (c < 0x80) {
            if (c == '"' || c == '\\') { jbuf_append_char(b, '\\'); jbuf_append_char(b, (char)c); }
            else if (c < 0x20) { jbuf_appendf(b, "\\u%04x", c); if (!c) binary = true; }
            else jbuf_append_char(b, (char)c);
            i++; continue;
        }
        size_t width = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        bool valid = width && i + width <= n;
        for (size_t j = 1; valid && j < width; j++) valid = (s[i + j] & 0xc0) == 0x80;
        if (valid && width == 3) valid = !(c == 0xe0 && s[i + 1] < 0xa0) && !(c == 0xed && s[i + 1] >= 0xa0);
        if (valid && width == 4) valid = !(c == 0xf0 && s[i + 1] < 0x90) && !(c == 0xf4 && s[i + 1] > 0x8f);
        if (valid) { jbuf_append_len(b, (const char *)s + i, width); i += width; }
        else { jbuf_append(b, "\\ufffd"); binary = true; i++; }
    }
    jbuf_append_char(b, '"'); return binary;
}
static void capture_base64(jbuf_t *b, const unsigned char *s, size_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    jbuf_append_char(b, '"');
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)s[i] << 16;
        if (i + 1 < n) v |= (unsigned)s[i + 1] << 8;
        if (i + 2 < n) v |= s[i + 2];
        char enc[4] = {t[v >> 18], t[(v >> 12) & 63], i + 1 < n ? t[(v >> 6) & 63] : '=', i + 2 < n ? t[v & 63] : '='};
        jbuf_append_len(b, enc, 4);
    }
    jbuf_append_char(b, '"');
}

void process_capture_append_json_output(jbuf_t *b,
                                        const process_capture_t *capture) {
    const unsigned char *output =
        (const unsigned char *)(capture && capture->output ? capture->output : "");
    size_t length = capture ? capture->length : 0;
    if (capture_json_bytes(b, output, length)) {
        jbuf_append(b, ",\"output_base64\":");
        capture_base64(b, output, length);
    }
}

bool process_capture_json(const char *path, char *const argv[], int timeout_ms,
                          const char *command, char *result, size_t result_len) {
    if (!result || result_len < 256) {
        if (result && result_len) snprintf(result, result_len, "{}");
        return false;
    }
    size_t cap = (result_len - 256) / 8;
    if (cap > 1024 * 1024) cap = 1024 * 1024;
    process_capture_t capture;
    bool ok = process_capture(path, argv, timeout_ms, cap, &capture);
    jbuf_t b; jbuf_init(&b, 512);
    jbuf_appendf(&b, "{\"ok\":%s,\"command\":", ok ? "true" : "false");
    jbuf_append_json_str(&b, command ? command : "");
    jbuf_appendf(&b, ",\"exit_code\":%d,\"timed_out\":%s,\"truncated\":%s,\"output\":",
                 capture.exit_code, capture.timed_out ? "true" : "false",
                 capture.truncated ? "true" : "false");
    process_capture_append_json_output(&b, &capture);
    if (capture.spawn_error) {
        jbuf_append(&b, ",\"error\":");
        jbuf_append_json_str(&b, strerror(capture.spawn_error));
    }
    jbuf_append(&b, "}");
    if (b.len >= result_len) {
        snprintf(result, result_len, "{\"ok\":false,\"truncated\":true,\"error\":\"result buffer too small\"}");
        ok = false;
    } else memcpy(result, b.data, b.len + 1);
    jbuf_free(&b); process_capture_free(&capture);
    return ok;
}
