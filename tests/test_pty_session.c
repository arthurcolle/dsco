/* Real, local PTYs only. No Kitty, model requests, shell or user terminal.
 * cc -std=gnu11 -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -Iinclude \
 *   tests/test_pty_session.c src/pty_session.c src/json_util.c -lpthread \
 *   -o /tmp/test_pty_session && /tmp/test_pty_session
 * Linux may need -D_GNU_SOURCE and -lutil. */
#include "pty_session.h"
#include "json_util.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RESPONSE_CAP (128 * 1024)
static char self[PATH_MAX];

static void write_all(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        p += n; size -= (size_t)n;
    }
}

static void raw_terminal(void) {
    struct termios t;
    assert(tcgetattr(STDIN_FILENO, &t) == 0);
    cfmakeraw(&t);
    assert(tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0);
}

static int child_main(int argc, char **argv) {
    const char *mode = argv[2];
    if (!strcmp(mode, "exit")) return 7;
    if (!strcmp(mode, "args")) {
        for (int i = 3; i < argc; i++) printf("ARG:%s\n", argv[i]);
        char cwd[PATH_MAX]; assert(getcwd(cwd, sizeof(cwd)));
        printf("CWD:%s\nTERM:%s\n", cwd, getenv("TERM"));
        return 0;
    }
    raw_terminal();
    if (!strcmp(mode, "fragments")) {
        write_all(1, "\xf0", 1);
        usleep(250000);
        write_all(1, "\x9f\xa6\x89\n", 4);
        return 0;
    }
    if (!strcmp(mode, "flood")) {
        unsigned char data[8192]; memset(data, 'x', sizeof(data));
        data[17] = 0; data[19] = 0xff;
        for (int i = 0; i < 160; i++) write_all(1, data, sizeof(data));
        write_all(1, "END-OF-FLOOD", 12);
        return 0;
    }
    if (!strcmp(mode, "quiet")) { for (;;) pause(); }
    if (!strcmp(mode, "group")) {
        signal(SIGTERM, SIG_IGN);
        pid_t child = fork(); assert(child >= 0);
        if (!child) { for (;;) pause(); }
        printf("CHILD:%d\n", (int)child); fflush(stdout);
        for (;;) pause();
    }
    if (!strcmp(mode, "foreground_group")) {
        /* Interactive shells place foreground jobs in a different group.
         * Both groups ignore TERM/HUP to require confirmed hard cleanup. */
        assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
        assert(signal(SIGHUP, SIG_IGN) != SIG_ERR);
        int ready[2]; assert(pipe(ready) == 0);
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            close(ready[0]);
            assert(setpgid(0, 0) == 0);
            write_all(ready[1], "R", 1); close(ready[1]);
            for (;;) pause();
        }
        close(ready[1]);
        char byte; assert(read(ready[0], &byte, 1) == 1); close(ready[0]);
        assert(tcsetpgrp(STDIN_FILENO, child) == 0);
        printf("FOREGROUND:%d\n", (int)child); fflush(stdout);
        for (;;) pause();
    }
    write_all(1, "READY\n", 6);
    char line[4096]; size_t used = 0;
    for (;;) {
        char c;
        ssize_t n = read(0, &c, 1);
        if (n <= 0 || c == 4) return 0;
        if (c != '\n' && used + 1 < sizeof(line)) { line[used++] = c; continue; }
        line[used] = 0;
        if (!strcmp(line, "size")) {
            struct winsize size; assert(ioctl(0, TIOCGWINSZ, &size) == 0);
            printf("SIZE:%ux%u\n", size.ws_col, size.ws_row); fflush(stdout);
        } else { write_all(1, "ECHO:", 5); write_all(1, line, used); write_all(1, "\n", 1); }
        used = 0;
    }
}

static bool request(const char *json, char *out, size_t cap) {
    bool ok = tool_pty_session(json, out, cap);
    if (!json_is_valid_container(out)) { fprintf(stderr, "Invalid response to %s: %s\n", json, out); abort(); }
    return ok;
}

static char *spawn(const char *mode, const char *extra, char *out) {
    jbuf_t b; jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"action\":\"spawn\",\"command\":"); jbuf_append_json_str(&b, self);
    jbuf_append(&b, ",\"args\":[\"--child\","); jbuf_append_json_str(&b, mode); jbuf_append(&b, "]");
    if (extra) jbuf_append(&b, extra);
    jbuf_append(&b, "}");
    if (!request(b.data, out, RESPONSE_CAP)) { fprintf(stderr, "Spawn failed: %s\n", out); abort(); }
    jbuf_free(&b);
    char *id = json_get_str(out, "session_id"); assert(id && strlen(id) == 32);
    return id;
}

static bool call(const char *action, const char *id, const char *extra, char *out, size_t cap) {
    jbuf_t b; jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"action\":"); jbuf_append_json_str(&b, action);
    jbuf_append(&b, ",\"session_id\":"); jbuf_append_json_str(&b, id);
    if (extra) jbuf_append(&b, extra);
    jbuf_append(&b, "}"); bool ok = request(b.data, out, cap); jbuf_free(&b); return ok;
}

static void close_session(char *id, char *out) {
    assert(call("close", id, NULL, out, RESPONSE_CAP));
    assert(json_get_bool(out, "reaped", false));
    assert(!call("status", id, NULL, out, RESPONSE_CAP));
    free(id);
}

static char *wait_text(const char *id, const char *needle, char *out) {
    jbuf_t all; jbuf_init(&all, 1024);
    for (int i = 0; i < 30; i++) {
        assert(call("read", id, ",\"timeout_ms\":200", out, RESPONSE_CAP));
        char *part = json_get_str(out, "output"); assert(part);
        jbuf_append(&all, part); free(part);
        if (strstr(all.data, needle)) return all.data;
    }
    fprintf(stderr, "Missing %s in %s\n", needle, all.data); abort();
}

static void test_interactive(void) {
    char *out = malloc(RESPONSE_CAP); assert(out);
    char *id = spawn("echo", NULL, out);
    char *text = wait_text(id, "READY", out); free(text);
    assert(call("write", id, ",\"input\":\"π 🦉 fragmented\\n\"", out, RESPONSE_CAP));
    assert(json_get_int(out, "bytes_written", 0) == (int)strlen("π 🦉 fragmented\n"));
    text = wait_text(id, "ECHO:π 🦉 fragmented", out); free(text);
    assert(call("resize", id, ",\"cols\":144,\"rows\":48", out, RESPONSE_CAP));
    assert(call("write", id, ",\"input\":\"size\\n\"", out, RESPONSE_CAP));
    text = wait_text(id, "SIZE:144x48", out); free(text);
    assert(call("wait", id, ",\"timeout_ms\":25", out, RESPONSE_CAP));
    assert(json_get_bool(out, "timed_out", false));
    assert(!json_get_bool(out, "reaped", true));
    assert(call("write", id, ",\"input\":\"\\u0004\"", out, RESPONSE_CAP));
    assert(call("wait", id, ",\"timeout_ms\":2000", out, RESPONSE_CAP));
    assert(json_get_bool(out, "eof", false)); assert(json_get_int(out, "exit_code", -1) == 0);
    assert(call("read", id, ",\"offset\":0", out, RESPONSE_CAP));
    text = json_get_str(out, "output"); assert(strstr(text, "READY") && strstr(text, "π 🦉")); free(text);
    close_session(id, out); free(out);
}

static void test_initial_input_and_canonical_eof(void) {
    char *out = malloc(RESPONSE_CAP); assert(out);
    assert(request("{\"action\":\"spawn\",\"command\":\"/bin/cat\",\"input\":\"INITIAL-INPUT\\n\"}", out, RESPONSE_CAP));
    char *id = json_get_str(out, "session_id"); assert(id);
    assert(json_get_int(out, "bytes_written", -1) == 14);
    char *text = wait_text(id, "INITIAL-INPUT", out); free(text);
    char json[256]; snprintf(json, sizeof(json), "{\"action\":\"status\",\"surface_id\":\"pty:%s\"}", id);
    assert(request(json, out, RESPONSE_CAP));
    assert(call("write", id, ",\"input\":\"\\u0004\"", out, RESPONSE_CAP));
    assert(call("wait", id, ",\"timeout_ms\":2000", out, RESPONSE_CAP));
    assert(json_get_bool(out, "eof", false)); assert(json_get_int(out, "exit_code", -1) == 0);
    close_session(id, out); free(out);
}

static void test_utf8_fragments_and_capacity(void) {
    char *out = malloc(RESPONSE_CAP); assert(out);
    char *id = spawn("fragments", NULL, out);
    assert(call("read", id, ",\"timeout_ms\":100", out, RESPONSE_CAP));
    char *text = json_get_str(out, "output"); assert(text && !*text); free(text);
    assert(json_get_int(out, "next_offset", -1) == 0);
    assert(call("wait", id, ",\"timeout_ms\":2000", out, RESPONSE_CAP));
    text = json_get_str(out, "output"); assert(!strcmp(text, "🦉\n")); free(text);
    char *base64 = json_get_str(out, "output_base64"); assert(!strcmp(base64, "8J+miQo=")); free(base64);
    close_session(id, out);
    id = spawn("flood", NULL, out);
    /* The producer must finish with no read calls: drain is independent. */
    usleep(300000);
    assert(call("wait", id, ",\"timeout_ms\":3000,\"offset\":0", out, RESPONSE_CAP));
    if (!json_get_bool(out, "reaped", false)) fprintf(stderr, "Flood wait: %.1000s\n", out);
    assert(json_get_bool(out, "reaped", false)); assert(json_get_bool(out, "eof", false));
    long long start = json_get_i64(out, "buffer_start", 0), end = json_get_i64(out, "buffer_end", 0);
    assert(start > 0 && end - start == 256 * 1024);
    assert(json_get_i64(out, "lost_bytes", 0) == start);
    char extra[160]; snprintf(extra, sizeof(extra), ",\"offset\":%lld,\"max_bytes\":65536", end - 12);
    assert(call("read", id, extra, out, 2048));
    text = json_get_str(out, "output"); assert(!strcmp(text, "END-OF-FLOOD")); free(text);
    assert(call("read", id, ",\"offset\":0,\"max_bytes\":65536", out, 2048));
    assert(strlen(out) < 2048 && json_get_bool(out, "more", false));
    close_session(id, out); free(out);
}

static void test_validation_and_argv(void) {
    char *out = malloc(RESPONSE_CAP); assert(out);
    assert(!request("{\"action\":\"spawn\",\"command\":\"/bin/echo\",\"args\":\"bad\"}", out, RESPONSE_CAP));
    assert(!request("{\"action\":\"spawn\",\"command\":\"/bin/echo\",\"args\":[1]}", out, RESPONSE_CAP));
    assert(!request("{\"action\":\"spawn\",\"command\":\"/bin/echo\",\"args\":[\"x\\u0000y\"]}", out, RESPONSE_CAP));
    assert(!request("{\"action\":\"spawn\",\"command\":\"/not/a/program\"}", out, RESPONSE_CAP));
    assert(!request("{\"action\":\"spawn\",\"command\":\"/bin/echo\",\"cols\":0}", out, RESPONSE_CAP));
    assert(!request("{\"action\":\"spawn\",\"command\":\"/bin/echo\",\"ttl_seconds\":\"bad\"}", out, RESPONSE_CAP));
    jbuf_t b; jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"action\":\"spawn\",\"command\":"); jbuf_append_json_str(&b, self);
    jbuf_append(&b, ",\"args\":[\"--child\",\"args\",\"$(touch /tmp/PTY_MUST_NOT_EXIST)\",\"x; echo NO\",\"🦉\",\"\\\\u0000\"],\"cwd\":\"/tmp\",\"term\":\"vt100\"}");
    assert(request(b.data, out, RESPONSE_CAP)); jbuf_free(&b);
    char *id = json_get_str(out, "session_id"); assert(id);
    assert(call("wait", id, ",\"timeout_ms\":2000", out, RESPONSE_CAP));
    char *text = json_get_str(out, "output"); assert(text);
    assert(strstr(text, "$(touch /tmp/PTY_MUST_NOT_EXIST)")); assert(strstr(text, "x; echo NO"));
    assert(strstr(text, "ARG:🦉")); assert(strstr(text, "ARG:\\u0000")); assert(strstr(text, "TERM:vt100"));
    assert(strstr(text, "CWD:/private/tmp") || strstr(text, "CWD:/tmp")); free(text);
    close_session(id, out);
    id = spawn("exit", NULL, out);
    assert(call("wait", id, ",\"timeout_ms\":2000", out, RESPONSE_CAP));
    assert(json_get_int(out, "exit_code", -1) == 7); close_session(id, out);
    free(out);
}

static void test_cancel_and_ttl(void) {
    char *out = malloc(RESPONSE_CAP); assert(out);
    char *id = spawn("group", NULL, out);
    char *text = wait_text(id, "CHILD:", out); free(text);
    assert(call("close", id, NULL, out, RESPONSE_CAP));
    assert(json_get_bool(out, "reaped", false)); assert(json_get_int(out, "signal", 0) == SIGKILL);
    free(id);
    id = spawn("foreground_group", NULL, out);
    text = wait_text(id, "FOREGROUND:", out);
    pid_t foreground = (pid_t)strtol(strstr(text, "FOREGROUND:") + 11, NULL, 10);
    assert(foreground > 1); free(text);
    assert(call("close", id, NULL, out, RESPONSE_CAP));
    bool foreground_gone = false;
    for (int i = 0; i < 150; i++) {
        if (kill(foreground, 0) < 0 && errno == ESRCH) { foreground_gone = true; break; }
        usleep(10000);
    }
    if (!foreground_gone) kill(foreground, SIGKILL);
    assert(foreground_gone);
    assert(json_get_bool(out, "reaped", false));
    free(id);
    id = spawn("quiet", ",\"ttl_seconds\":1", out);
    assert(call("wait", id, ",\"timeout_ms\":2500", out, RESPONSE_CAP));
    text = json_get_str(out, "state"); assert(!strcmp(text, "expired")); free(text);
    assert(json_get_bool(out, "reaped", false)); close_session(id, out); free(out);
}

static void *parallel_session(void *arg) {
    (void)arg;
    char *out = malloc(RESPONSE_CAP); assert(out);
    char *id = spawn("exit", NULL, out);
    assert(call("wait", id, ",\"timeout_ms\":2000", out, RESPONSE_CAP));
    assert(json_get_int(out, "exit_code", -1) == 7);
    close_session(id, out); free(out); return NULL;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--child")) return child_main(argc, argv);
    assert(realpath(argv[0], self));
    test_interactive();
    test_initial_input_and_canonical_eof();
    test_utf8_fragments_and_capacity();
    test_validation_and_argv();
    test_cancel_and_ttl();
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, parallel_session, NULL));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    char out[4096]; assert(request("{\"action\":\"list\"}", out, sizeof(out)));
    assert(strstr(out, "\"sessions\":[]"));
    char *final_out = malloc(RESPONSE_CAP); assert(final_out);
    char *final_id = spawn("quiet", NULL, final_out);
    pid_t final_pid = (pid_t)json_get_int(final_out, "pid", -1); assert(final_pid > 1);
    pty_sessions_shutdown();
    assert(waitpid(final_pid, NULL, WNOHANG) == -1 && errno == ECHILD);
    assert(kill(final_pid, 0) == -1 && errno == ESRCH);
    free(final_id); free(final_out);
    puts("PTY sessions: real Unicode/fragmented streams, replay, resize, EOF, bounded output, argv, exit codes, foreground/group cancellation, TTL and concurrency passed");
    return 0;
}
