#include "pty_session.h"
#include "json_util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#else
#include <pty.h>
#endif

extern char **environ;

#define SESSION_CAP 32
#define OUTPUT_CAP (256 * 1024)
#define INPUT_CAP (64 * 1024)
#define REQUEST_CAP (256 * 1024)
#define ARG_CAP 64

typedef struct {
    bool used, eof, reaped, group_gone, foreground_gone, closing, killed, expired, retire, command_truncated;
    unsigned users;
    int master, status, cols, rows;
    pid_t pid, foreground;
    char id[33], command[192];
    unsigned char *output;
    uint64_t start, end, cursor;
    double deadline, kill_at;
} pty_session_t;

static pty_session_t sessions[SESSION_CAP];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t collector;
static _Atomic pid_t owner_pid;
static bool collector_started, stopping, shutdown_registered;

static double monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* The condition variable uses its default realtime clock. Recompute each short
 * wait; caller deadlines always use monotonic time. mutex is held on entry/exit. */
static void pause_locked(int milliseconds) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)milliseconds * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&changed, &mutex, &ts);
}

static bool error_result(char *result, size_t cap, const char *message) {
    jbuf_t b;
    jbuf_init(&b, 256);
    jbuf_append(&b, "{\"ok\":false,\"error\":");
    jbuf_append_json_str(&b, message);
    jbuf_append(&b, "}");
    if (b.len < cap)
        memcpy(result, b.data, b.len + 1);
    else if (cap >= 13)
        snprintf(result, cap, "{\"ok\":false}");
    else if (cap)
        result[0] = '\0';
    jbuf_free(&b);
    return false;
}

static bool finish_result(jbuf_t *b, char *result, size_t cap, bool ok) {
    if (b->len >= cap) {
        jbuf_free(b);
        return error_result(result, cap, "response buffer too small");
    }
    memcpy(result, b->data, b->len + 1);
    jbuf_free(b);
    return ok;
}

static bool groups_gone(const pty_session_t *s) {
    return s->group_gone && (s->foreground <= 1 || s->foreground_gone);
}

static void signal_owned_groups(pty_session_t *s, int signal_number) {
    if (s->pid > 1 && !s->group_gone)
        if (kill(-s->pid, signal_number) < 0 && errno == ESRCH) s->group_gone = true;
    if (s->foreground > 1 && !s->foreground_gone)
        if (kill(-s->foreground, signal_number) < 0 && errno == ESRCH) s->foreground_gone = true;
}

static void begin_close(pty_session_t *s, bool expired) {
    if (s->closing) return;
    s->closing = true;
    s->expired = expired;
    s->kill_at = monotonic_ms() + 250.0;
    /* A shell gives foreground jobs their own group. Capture the kernel's
     * foreground group before terminating the shell or losing the PTY. Merely
     * closing the terminal leaks jobs that ignore SIGHUP. The terminal only
     * accepts foreground groups in its own session; caller IDs are not used. */
    pid_t foreground = s->master >= 0 ? tcgetpgrp(s->master) : -1;
    if (foreground > 1 && foreground != s->pid && foreground != getpgrp())
        s->foreground = foreground;
    signal_owned_groups(s, SIGTERM);
}

static void ring_append(pty_session_t *s, const unsigned char *data, size_t n) {
    size_t first = OUTPUT_CAP - (size_t)(s->end % OUTPUT_CAP);
    if (first > n) first = n;
    memcpy(s->output + s->end % OUTPUT_CAP, data, first);
    memcpy(s->output, data + first, n - first);
    s->end += n;
    if (s->end - s->start > OUTPUT_CAP) s->start = s->end - OUTPUT_CAP;
}

static void collect_locked(pty_session_t *s) {
    if (!s->used) return;
    if (!s->group_gone && kill(-s->pid, 0) < 0 && errno == ESRCH) s->group_gone = true;
    if (s->foreground > 1 && !s->foreground_gone && kill(-s->foreground, 0) < 0 && errno == ESRCH)
        s->foreground_gone = true;
    if (!s->closing && monotonic_ms() >= s->deadline && (!s->reaped || !s->eof || !groups_gone(s)))
        begin_close(s, true);
    if (s->closing && !s->killed && monotonic_ms() >= s->kill_at) {
        signal_owned_groups(s, SIGKILL);
        s->killed = true;
    }
    if (s->master >= 0) {
        unsigned char data[8192];
        /* Bound work per session so a flood cannot starve peers or control. */
        for (int i = 0; i < 32; i++) {
            ssize_t n = read(s->master, data, sizeof(data));
            if (n > 0) {
                ring_append(s, data, (size_t)n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                s->eof = true;
                close(s->master);
                s->master = -1;
            }
            break;
        }
    }
    if (!s->reaped) {
        pid_t got = waitpid(s->pid, &s->status, WNOHANG);
        if (got == s->pid) s->reaped = true;
        else if (got < 0 && errno == ECHILD) {
            /* Another process-wide reaper consumed this child. Do not invent
             * a successful exit code or spin forever. */
            s->reaped = true;
            s->status = -1;
        }
    }
    if (s->closing && s->killed && s->master >= 0) {
        /* After signalling both owned groups, bound terminal lifetime even if
         * another process escaped those groups and retained a slave fd. */
        close(s->master);
        s->master = -1;
        s->eof = true;
    }
    if (s->retire && !s->users && s->reaped && (groups_gone(s) || s->killed)) {
        if (s->master >= 0) close(s->master);
        free(s->output);
        memset(s, 0, sizeof(*s));
        s->master = -1;
    }
}

static void *collect_main(void *unused) {
    (void)unused;
    pthread_mutex_lock(&mutex);
    while (!stopping) {
        bool active = false;
        struct pollfd fds[SESSION_CAP];
        nfds_t count = 0;
        for (int i = 0; i < SESSION_CAP; i++) {
            collect_locked(&sessions[i]);
            if (sessions[i].used && (!sessions[i].reaped || !sessions[i].eof || !groups_gone(&sessions[i]))) active = true;
            if (sessions[i].used && sessions[i].master >= 0)
                fds[count++] = (struct pollfd){.fd = sessions[i].master, .events = POLLIN | POLLHUP};
        }
        pthread_cond_broadcast(&changed);
        if (count) {
            /* PTY kernel buffers can be small. Wake on readability, rather
             * than limiting producers to one kernel buffer per timer tick.
             * A concurrent close can invalidate an fd snapshot; readiness is
             * only a wake hint and the next pass rechecks current ownership. */
            pthread_mutex_unlock(&mutex);
            poll(fds, count, 20);
            pthread_mutex_lock(&mutex);
        } else if (active) pause_locked(20);
        else pthread_cond_wait(&changed, &mutex);
    }
    pthread_mutex_unlock(&mutex);
    return NULL;
}

void pty_sessions_shutdown(void) {
    if (!owner_pid || owner_pid != getpid()) return;
    pthread_mutex_lock(&mutex);
    if (stopping) { pthread_mutex_unlock(&mutex); return; }
    stopping = true;
    for (int i = 0; i < SESSION_CAP; i++) if (sessions[i].used) begin_close(&sessions[i], false);
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    if (collector_started) pthread_join(collector, NULL);
    pthread_mutex_lock(&mutex);
    double deadline = monotonic_ms() + 1500.0;
    bool active;
    do {
        active = false;
        for (int i = 0; i < SESSION_CAP; i++) {
            collect_locked(&sessions[i]);
            if (sessions[i].used && (!sessions[i].reaped || !groups_gone(&sessions[i]))) active = true;
        }
        if (active) pause_locked(20);
    } while (active && monotonic_ms() < deadline);
    for (int i = 0; i < SESSION_CAP; i++) {
        pty_session_t *s = &sessions[i];
        if (!s->used) continue;
        signal_owned_groups(s, SIGKILL);
        if (s->master >= 0) close(s->master);
        s->master = -1;
        if (!s->users) { free(s->output); s->output = NULL; s->used = false; }
    }
    pthread_mutex_unlock(&mutex);
}

static pty_session_t *find_session(const char *id) {
    if (!id) return NULL;
    if (!strncmp(id, "pty:", 4)) id += 4;
    for (int i = 0; i < SESSION_CAP; i++)
        if (sessions[i].used && !sessions[i].retire && !strcmp(sessions[i].id, id)) return &sessions[i];
    return NULL;
}

static void output_text(jbuf_t *b, const unsigned char *p, size_t n);

static void metadata(jbuf_t *b, const pty_session_t *s) {
    jbuf_appendf(b, "\"session_id\":\"%s\",\"surface_id\":\"pty:%s\",\"pid\":%d,", s->id, s->id, (int)s->pid);
    jbuf_append(b, "\"state\":");
    jbuf_append_json_str(b, s->retire ? "closed" : s->expired ? "expired" :
        s->closing ? "closing" : s->reaped ? "exited" : "running");
    jbuf_appendf(b, ",\"eof\":%s,\"reaped\":%s,\"group_alive\":%s,\"exit_code\":", s->eof ? "true" : "false", s->reaped ? "true" : "false", groups_gone(s) ? "false" : "true");
    if (s->reaped && s->status >= 0 && WIFEXITED(s->status)) jbuf_append_int(b, WEXITSTATUS(s->status));
    else jbuf_append(b, "null");
    jbuf_append(b, ",\"signal\":");
    if (s->reaped && s->status >= 0 && WIFSIGNALED(s->status)) jbuf_append_int(b, WTERMSIG(s->status));
    else jbuf_append(b, "null");
    jbuf_appendf(b, ",\"cols\":%d,\"rows\":%d,\"buffer_start\":%llu,\"buffer_end\":%llu,\"dropped_bytes\":%llu,\"expires_in_ms\":%.0f,\"command\":",
        s->cols, s->rows, (unsigned long long)s->start, (unsigned long long)s->end,
        (unsigned long long)s->start, s->deadline > monotonic_ms() ? s->deadline - monotonic_ms() : 0.0);
    output_text(b, (const unsigned char *)s->command, strlen(s->command));
    jbuf_appendf(b, ",\"command_truncated\":%s", s->command_truncated ? "true" : "false");
    jbuf_appendf(b, ",\"forced_close\":%s", s->killed ? "true" : "false");
}

/* JSON text is valid UTF-8 even for binary terminal output; base64 preserves
 * every original byte. Incomplete trailing characters wait for the next read. */
static size_t utf8_width(const unsigned char *p, size_t n) {
    unsigned c = p[0];
    if (c < 0x80) return 1;
    size_t w = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 1;
    if (w == 1 || w > n) return 0;
    for (size_t i = 1; i < w; i++) if ((p[i] & 0xc0) != 0x80) return 0;
    if ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] >= 0xa0) ||
        (c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] >= 0x90)) return 0;
    return w;
}

static void output_text(jbuf_t *b, const unsigned char *p, size_t n) {
    jbuf_append_char(b, '"');
    for (size_t i = 0; i < n;) {
        unsigned c = p[i];
        size_t w = utf8_width(p + i, n - i);
        if (!w) { jbuf_append(b, "\\ufffd"); i++; }
        else if (c < 0x20) { jbuf_appendf(b, "\\u%04x", c); i++; }
        else if (c == '"' || c == '\\') { jbuf_append_char(b, '\\'); jbuf_append_char(b, (char)c); i++; }
        else { jbuf_append_len(b, (const char *)p + i, w); i += w; }
    }
    jbuf_append_char(b, '"');
}

static void output_base64(jbuf_t *b, const unsigned char *p, size_t n) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    jbuf_append_char(b, '"');
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16;
        if (i + 1 < n) v |= (unsigned)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        char out[] = {alphabet[v >> 18], alphabet[(v >> 12) & 63],
            i + 1 < n ? alphabet[(v >> 6) & 63] : '=', i + 2 < n ? alphabet[v & 63] : '='};
        jbuf_append_len(b, out, 4);
    }
    jbuf_append_char(b, '"');
}

static size_t read_output(jbuf_t *b, pty_session_t *s, uint64_t offset, size_t max_bytes, bool advance) {
    uint64_t actual = offset < s->start ? s->start : offset;
    if (actual > s->end) actual = s->end;
    size_t n = (size_t)(s->end - actual);
    if (n > max_bytes) n = max_bytes;
    unsigned char *data = safe_malloc(n + 1);
    for (size_t i = 0; i < n; i++) data[i] = s->output[(actual + i) % OUTPUT_CAP];
    if (n && (!s->eof || actual + n < s->end)) {
        size_t last = n - 1;
        while (last && n - last < 4 && (data[last] & 0xc0) == 0x80) last--;
        unsigned c = data[last];
        size_t width = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 1;
        if (width > n - last) n = last;
    }
    jbuf_appendf(b, ",\"offset\":%llu,\"next_offset\":%llu,\"lost_bytes\":%llu,\"more\":%s,\"output\":",
        (unsigned long long)actual, (unsigned long long)(actual + n),
        (unsigned long long)(s->start > offset ? s->start - offset : 0), actual + n < s->end ? "true" : "false");
    output_text(b, data, n);
    jbuf_append(b, ",\"output_base64\":");
    output_base64(b, data, n);
    free(data);
    if (advance) s->cursor = actual + n;
    return n;
}

typedef struct { char *argv[ARG_CAP + 2]; size_t bytes; int count; bool invalid; } arguments_t;

static void collect_arg(const char *element, void *context) {
    arguments_t *a = context;
    if (a->invalid) return;
    if (a->count >= ARG_CAP + 1 || *element != '"') { a->invalid = true; return; }
    const char *end = element + 1;
    while (*end && *end != '"') { if (*end == '\\' && end[1]) end++; end++; }
    if (*end != '"' || (size_t)(end - element) > REQUEST_CAP) { a->invalid = true; return; }
    jbuf_t wrapper;
    jbuf_init(&wrapper, (size_t)(end - element) + 16);
    jbuf_append(&wrapper, "{\"v\":");
    jbuf_append_len(&wrapper, element, (size_t)(end - element) + 1);
    jbuf_append_char(&wrapper, '}');
    char *value = json_get_str(wrapper.data, "v");
    jbuf_free(&wrapper);
    if (!value || strlen(value) > 16384 || a->bytes + strlen(value) > 128 * 1024) {
        free(value); a->invalid = true; return;
    }
    a->bytes += strlen(value);
    a->argv[a->count++] = value;
}

static void free_arguments(arguments_t *a) {
    for (int i = 0; i < a->count; i++) free(a->argv[i]);
}

/* Reject embedded NUL before shared string helpers turn it into a truncated C
 * string. Escaped literal backslashes are skipped, so "\\u0000" stays literal. */
static bool has_json_nul(const char *p) {
    while (*p) {
        if (*p++ != '\\') continue;
        if (*p == 'u' && !strncmp(p + 1, "0000", 4)) return true;
        if (*p) p++;
    }
    return false;
}

static bool executable_path(const char *command, const char *cwd, char *out, size_t cap) {
    if (strchr(command, '/')) {
        int n = command[0] == '/' ? snprintf(out, cap, "%s", command) : snprintf(out, cap, "%s/%s", cwd, command);
        return n >= 0 && (size_t)n < cap && access(out, X_OK) == 0;
    }
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    for (const char *p = path;;) {
        const char *end = strchr(p, ':');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        int written;
        if (!n) written = snprintf(out, cap, "%s/%s", cwd, command);
        else if (*p == '/') written = snprintf(out, cap, "%.*s/%s", (int)n, p, command);
        else written = snprintf(out, cap, "%s/%.*s/%s", cwd, (int)n, p, command);
        if (written >= 0 && (size_t)written < cap && access(out, X_OK) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

static char **child_environment(const char *term) {
    size_t count = 0;
    while (environ[count]) count++;
    char **env = safe_malloc((count + 2) * sizeof(*env));
    size_t n = 0;
    for (size_t i = 0; i < count; i++) if (strncmp(environ[i], "TERM=", 5)) env[n++] = safe_strdup(environ[i]);
    env[n] = safe_malloc(strlen(term) + 6);
    snprintf(env[n++], strlen(term) + 6, "TERM=%s", term);
    env[n] = NULL;
    return env;
}

static bool random_id(char id[33]) {
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + got, sizeof(bytes) - got);
        if (n > 0) got += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else { close(fd); return false; }
    }
    close(fd);
    for (size_t i = 0; i < sizeof(bytes); i++) snprintf(id + 2 * i, 3, "%02x", bytes[i]);
    return true;
}

/* Parent-side preparation is complete before fork. The child only performs
 * descriptor/signal setup and execve; it never uses the manager or its mutex. */
static pty_session_t *spawn_locked(const char *json, const char **error) {
    arguments_t a = {0};
    a.argv[a.count++] = json_get_str(json, "command");
    char *cwd_arg = json_get_str(json, "cwd");
    char *term_arg = json_get_str(json, "term");
    char *args_raw = json_get_raw(json, "args");
    pty_session_t *s = NULL;
    char cwd[PATH_MAX], executable[PATH_MAX], id[33];
    char **env = NULL;
    int handshake[2] = {-1, -1}, master = -1;
    *error = "invalid spawn arguments";
    if (!a.argv[0] || !a.argv[0][0] || strlen(a.argv[0]) >= PATH_MAX) goto done;
    if (args_raw && *args_raw != '[') goto done;
    json_array_foreach(json, "args", collect_arg, &a);
    if (a.invalid) { *error = "args require at most 64 strings, 16 KiB each and 128 KiB total"; goto done; }
    if (!realpath(cwd_arg && cwd_arg[0] ? cwd_arg : ".", cwd)) { *error = "cwd does not resolve"; goto done; }
    if (!executable_path(a.argv[0], cwd, executable, sizeof(executable))) { *error = "executable not found or not executable"; goto done; }
    const char *term = term_arg ? term_arg : "xterm-256color";
    if (!*term || strlen(term) > 128) goto done;
    for (const char *p = term; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') goto done;
    for (int i = 0; i < SESSION_CAP; i++) if (!sessions[i].used) { s = &sessions[i]; break; }
    if (!s) { *error = "32 PTY session limit reached; close a session first"; goto done; }
    if (!random_id(id)) { s = NULL; *error = "cannot generate session identity"; goto done; }
    env = child_environment(term);
    if (pipe(handshake) < 0) { s = NULL; *error = "exec handshake pipe failed"; goto done; }
    fcntl(handshake[0], F_SETFD, FD_CLOEXEC);
    fcntl(handshake[1], F_SETFD, FD_CLOEXEC);
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0) maxfd = 1024;
    struct winsize size = {.ws_col = (unsigned short)json_get_int(json, "cols", 100), .ws_row = (unsigned short)json_get_int(json, "rows", 30)};
    pid_t pid = forkpty(&master, NULL, NULL, &size);
    if (pid == 0) {
        close(handshake[0]);
        for (long fd = 3; fd < maxfd; fd++) if (fd != handshake[1]) close((int)fd);
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        const int signals[] = {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE, SIGCHLD, SIGTSTP, SIGTTIN, SIGTTOU};
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) sigaction(signals[i], &action, NULL);
        if (chdir(cwd) == 0) execve(executable, a.argv, env);
        int child_error = errno;
        (void)write(handshake[1], &child_error, sizeof(child_error));
        _exit(127);
    }
    close(handshake[1]); handshake[1] = -1;
    if (pid < 0) { s = NULL; *error = "forkpty failed"; goto done; }
    fcntl(master, F_SETFD, FD_CLOEXEC);
    fcntl(master, F_SETFL, O_NONBLOCK);
    int child_error = 0;
    struct pollfd pfd = {.fd = handshake[0], .events = POLLIN | POLLHUP};
    int ready;
    double exec_deadline = monotonic_ms() + 5000.0;
    do {
        int remaining = (int)(exec_deadline - monotonic_ms());
        if (remaining <= 0) { ready = 0; break; }
        ready = poll(&pfd, 1, remaining);
    } while (ready < 0 && errno == EINTR);
    ssize_t bytes = ready > 0 ? read(handshake[0], &child_error, sizeof(child_error)) : -1;
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->master = master; master = -1;
    s->pid = pid;
    s->cols = size.ws_col; s->rows = size.ws_row;
    s->output = safe_malloc(OUTPUT_CAP);
    s->deadline = monotonic_ms() + json_get_int(json, "ttl_seconds", 1800) * 1000.0;
    snprintf(s->id, sizeof(s->id), "%s", id);
    snprintf(s->command, sizeof(s->command), "%s", a.argv[0]);
    s->command_truncated = strlen(a.argv[0]) >= sizeof(s->command);
    if (bytes != 0) {
        *error = bytes > 0 ? "child could not exec executable or enter cwd" : "child exec handshake timed out";
        s->retire = true;
        begin_close(s, false);
        pthread_cond_broadcast(&changed);
        s = NULL;
        goto done;
    }
    *error = NULL;
    pthread_cond_broadcast(&changed);
done:
    if (master >= 0) close(master);
    if (handshake[0] >= 0) close(handshake[0]);
    if (handshake[1] >= 0) close(handshake[1]);
    if (env) { for (size_t i = 0; env[i]; i++) free(env[i]); free(env); }
    free_arguments(&a);
    free(cwd_arg); free(term_arg); free(args_raw);
    return s;
}

static bool bounded_integer(const char *json, const char *key, long long minimum, long long maximum) {
    char *raw = json_get_raw(json, key);
    if (!raw) return true;
    char *end;
    errno = 0;
    long long value = strtoll(raw, &end, 10);
    bool ok = !errno && end != raw && !*end && value >= minimum && value <= maximum;
    free(raw);
    return ok;
}

static bool optional_string(const char *json, const char *key) {
    char *raw = json_get_raw(json, key);
    bool ok = !raw || *raw == '"';
    free(raw);
    return ok;
}

bool tool_pty_session(const char *json, char *result, size_t result_len) {
    if (!result || !result_len) return false;
    if (result_len < 2048) return error_result(result, result_len, "response buffer must be at least 2048 bytes");
    if (json) while (isspace((unsigned char)*json)) json++;
    if (!json || strlen(json) > REQUEST_CAP || *json != '{' || !json_is_valid_container(json) || has_json_nul(json))
        return error_result(result, result_len, "expected a bounded JSON object without NUL strings");
    const char *strings[] = {"action", "command", "input", "cwd", "term", "session_id", "surface_id"};
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++)
        if (!optional_string(json, strings[i])) return error_result(result, result_len, "expected a string field");
    if (!bounded_integer(json, "cols", 1, 1000) || !bounded_integer(json, "rows", 1, 1000) ||
        !bounded_integer(json, "timeout_ms", 0, 30000) || !bounded_integer(json, "ttl_seconds", 1, 86400) ||
        !bounded_integer(json, "max_bytes", 4, 65536) || !bounded_integer(json, "offset", 0, LLONG_MAX))
        return error_result(result, result_len, "invalid PTY dimensions, offset, size or deadline");
    char *action = json_get_str(json, "action");
    char *input = json_get_str(json, "input");
    char *id = json_get_str(json, "session_id");
    char *surface = json_get_str(json, "surface_id");
    const char *error = NULL;
    bool ok = true, timed_out = false;
    size_t written = 0;
    pty_session_t *s = NULL;
    jbuf_t b;
    jbuf_init(&b, 2048);
    if (!action || (strcmp(action, "spawn") && strcmp(action, "list") && strcmp(action, "status") &&
        strcmp(action, "read") && strcmp(action, "write") && strcmp(action, "resize") &&
        strcmp(action, "wait") && strcmp(action, "close"))) { error = "unknown PTY action"; goto done; }
    if (input && strlen(input) > INPUT_CAP) { error = "input exceeds 64 KiB"; goto done; }
    if (!strcmp(action, "write") && !input) { error = "write requires input text"; goto done; }
    if (owner_pid && owner_pid != getpid()) { error = "PTY sessions are not inherited across fork; start a new DSCO process"; goto done; }
    pthread_mutex_lock(&mutex);
    if (!owner_pid) owner_pid = getpid();
    if (stopping) { error = "PTY manager is shutting down"; goto unlock; }
    if (!collector_started) {
        if (pthread_create(&collector, NULL, collect_main, NULL)) { error = "cannot start PTY collector"; goto unlock; }
        collector_started = true;
        if (!shutdown_registered) { atexit(pty_sessions_shutdown); shutdown_registered = true; }
    }
    if (!strcmp(action, "list")) {
        jbuf_append(&b, "{\"ok\":true,\"persistence\":\"process\",\"sessions\":[");
        bool first = true, truncated = false;
        for (int i = 0; i < SESSION_CAP; i++) {
            if (!sessions[i].used || sessions[i].retire) continue;
            collect_locked(&sessions[i]);
            jbuf_t entry; jbuf_init(&entry, 512);
            jbuf_append_char(&entry, '{'); metadata(&entry, &sessions[i]); jbuf_append_char(&entry, '}');
            if (b.len + entry.len + 64 >= result_len) { jbuf_free(&entry); truncated = true; break; }
            if (!first) jbuf_append_char(&b, ',');
            jbuf_append(&b, entry.data); first = false; jbuf_free(&entry);
        }
        jbuf_appendf(&b, "],\"truncated\":%s}", truncated ? "true" : "false");
        goto unlock;
    }
    if (!strcmp(action, "spawn")) s = spawn_locked(json, &error);
    else {
        if (id && surface && (strncmp(surface, "pty:", 4) || strcmp(id, surface + 4))) {
            error = "session_id and surface_id disagree"; goto unlock;
        }
        s = find_session(id ? id : surface);
        if (!s) error = "unknown PTY session";
    }
    if (!s) goto unlock;
    s->users++;
    collect_locked(s);
    int timeout = json_get_int(json, "timeout_ms", !strcmp(action, "wait") || !strcmp(action, "write") || !strcmp(action, "spawn") ? 1000 : 0);
    double deadline = monotonic_ms() + timeout;
    if (!strcmp(action, "close")) {
        begin_close(s, false);
        deadline = monotonic_ms() + 1500;
        while ((!s->reaped || !s->eof || (!groups_gone(s) && !s->killed)) && monotonic_ms() < deadline && !stopping) { pause_locked(20); collect_locked(s); }
        s->retire = true;
        timed_out = !s->reaped;
    } else if (!strcmp(action, "resize")) {
        struct winsize size = {.ws_col = (unsigned short)json_get_int(json, "cols", s->cols), .ws_row = (unsigned short)json_get_int(json, "rows", s->rows)};
        if (s->master < 0 || s->closing || ioctl(s->master, TIOCSWINSZ, &size) < 0) error = "cannot resize a closed PTY";
        else { s->cols = size.ws_col; s->rows = size.ws_row; }
    } else if ((!strcmp(action, "write") || !strcmp(action, "spawn")) && input) {
        size_t n = strlen(input);
        while (written < n && !stopping && !s->retire && !s->closing && s->master >= 0) {
            ssize_t amount = write(s->master, input + written, n - written);
            if (amount > 0) { written += (size_t)amount; continue; }
            if (amount < 0 && errno == EINTR) continue;
            if (amount < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
            if (monotonic_ms() >= deadline) { timed_out = true; break; }
            pause_locked(10); collect_locked(s);
        }
        if (written < n) { ok = false; error = timed_out ? "PTY write deadline reached; only bytes_written were delivered" : "PTY closed before all input was delivered"; }
    } else if (!strcmp(action, "read") || !strcmp(action, "wait")) {
        uint64_t offset = (uint64_t)json_get_i64(json, "offset", (long long)s->cursor);
        if (offset > s->end) error = "offset is beyond current buffer_end";
        else while (!stopping && !s->retire) {
            bool ready = !strcmp(action, "wait") ? s->reaped && s->eof : s->end > offset || s->eof;
            if (ready) break;
            if (monotonic_ms() >= deadline) { timed_out = true; break; }
            pause_locked(20); collect_locked(s);
        }
    }
    if (error) ok = false;
    jbuf_appendf(&b, "{\"ok\":%s,", ok ? "true" : "false");
    metadata(&b, s);
    jbuf_appendf(&b, ",\"timed_out\":%s,\"bytes_written\":%zu", timed_out ? "true" : "false", written);
    if (error) { jbuf_append(&b, ",\"error\":"); jbuf_append_json_str(&b, error); error = NULL; }
    if (!strcmp(action, "read") || !strcmp(action, "wait")) {
        long long explicit_offset = json_get_i64(json, "offset", -1);
        uint64_t offset = explicit_offset < 0 ? s->cursor : (uint64_t)explicit_offset;
        size_t budget = result_len > b.len + 256 ? (result_len - b.len - 256) / 8 : 0;
        size_t max_bytes = (size_t)json_get_int(json, "max_bytes", 16384);
        if (max_bytes > budget) max_bytes = budget;
        read_output(&b, s, offset, max_bytes, explicit_offset < 0);
    }
    jbuf_append_char(&b, '}');
    s->users--;
    collect_locked(s);
unlock:
    pthread_mutex_unlock(&mutex);
done:
    free(action); free(input); free(id); free(surface);
    if (error) { jbuf_free(&b); return error_result(result, result_len, error); }
    return finish_result(&b, result, result_len, ok);
}
