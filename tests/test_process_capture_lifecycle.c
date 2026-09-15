/* Real subprocess lifecycle regressions; no shell, model or user terminal.
 * cc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror -Iinclude \
 *   tests/test_process_capture_lifecycle.c src/process_capture.c src/json_util.c \
 *   -lpthread -lm -o /tmp/test_process_capture_lifecycle
 */
#include "process_capture.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char executable[PATH_MAX];
static volatile sig_atomic_t terminating;

static double now_ms(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}

static void write_all(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        p += n; size -= (size_t)n;
    }
}

static void on_term(int signal_number) { (void)signal_number; terminating = 1; }

static int fixture(const char *mode) {
    if (!strcmp(mode, "signals")) {
        sigset_t mask;
        struct sigaction action;
        assert(sigprocmask(SIG_SETMASK, NULL, &mask) == 0);
        assert(sigaction(SIGTERM, NULL, &action) == 0);
        return sigismember(&mask, SIGTERM) || action.sa_handler != SIG_DFL ? 31 : 0;
    }
    if (!strcmp(mode, "signal_exit")) { raise(SIGTERM); return 32; }
    if (!strcmp(mode, "silent_descendant")) {
        int ready[2]; assert(pipe(ready) == 0);
        pid_t child = fork(); assert(child >= 0);
        if (!child) {
            close(ready[0]);
            assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
            close(STDIN_FILENO); close(STDOUT_FILENO); close(STDERR_FILENO);
            write_all(ready[1], "R", 1); close(ready[1]);
            for (;;) pause();
        }
        close(ready[1]);
        char byte; assert(read(ready[0], &byte, 1) == 1); close(ready[0]);
        printf("%d\n", (int)child);
        return 0;
    }
    if (!strcmp(mode, "flush_on_term")) {
        struct sigaction action = {0};
        action.sa_handler = on_term;
        sigemptyset(&action.sa_mask);
        assert(sigaction(SIGTERM, &action, NULL) == 0);
        sigset_t blocked, previous;
        sigemptyset(&blocked); sigaddset(&blocked, SIGTERM);
        assert(sigprocmask(SIG_BLOCK, &blocked, &previous) == 0);
        while (!terminating) sigsuspend(&previous);
        assert(sigprocmask(SIG_SETMASK, &previous, NULL) == 0);
        char chunk[8192]; memset(chunk, 'd', sizeof(chunk));
        for (int i = 0; i < 16; i++) write_all(STDOUT_FILENO, chunk, sizeof(chunk));
        write_all(STDERR_FILENO, "FINAL-DIAGNOSTIC", 16);
        return 42;
    }
    if (!strcmp(mode, "flood")) {
        assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
        char chunk[8192]; memset(chunk, 'x', sizeof(chunk));
        for (;;) write_all(STDOUT_FILENO, chunk, sizeof(chunk));
    }
    if (!strcmp(mode, "closed_streams")) {
        assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
        close(STDOUT_FILENO); close(STDERR_FILENO);
        for (;;) pause();
    }
    if (!strcmp(mode, "escaped_root")) {
        /* Pipe spawns are group leaders, but not session leaders. The owned
         * root can leave its group; never signal the new (caller's) group. */
        assert(setpgid(0, getpgid(getppid())) == 0);
        assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
        alarm(2); /* Bound this regression even against the broken runner. */
        for (;;) pause();
    }
    return 33;
}

static void test_final_diagnostics(void) {
    char *args[] = {executable, "--fixture", "flush_on_term", NULL};
    process_capture_t capture;
    double started = now_ms();
    assert(!process_capture(executable, args, 250, 160 * 1024, &capture));
    assert(capture.timed_out && !capture.spawn_error);
    assert(capture.exit_code == 42);
    assert(capture.length == 128 * 1024 + 16 && !capture.truncated);
    assert(!memcmp(capture.output + 128 * 1024, "FINAL-DIAGNOSTIC", 16));
    assert(now_ms() - started < 1500);
    process_capture_free(&capture);
}

static void test_signal_isolation(void) {
    struct sigaction ignored = {0}, previous_action;
    ignored.sa_handler = SIG_IGN; sigemptyset(&ignored.sa_mask);
    assert(sigaction(SIGTERM, &ignored, &previous_action) == 0);
    sigset_t mask, previous_mask;
    sigemptyset(&mask); sigaddset(&mask, SIGTERM);
    assert(sigprocmask(SIG_BLOCK, &mask, &previous_mask) == 0);
    char *args[] = {executable, "--fixture", "signals", NULL};
    process_capture_t capture;
    bool ok = process_capture(executable, args, 2000, 128, &capture);
    assert(sigprocmask(SIG_SETMASK, &previous_mask, NULL) == 0);
    assert(sigaction(SIGTERM, &previous_action, NULL) == 0);
    assert(ok && capture.exit_code == 0);
    process_capture_free(&capture);
    args[2] = "signal_exit";
    assert(!process_capture(executable, args, 2000, 128, &capture));
    assert(!capture.timed_out && !capture.spawn_error);
    assert(capture.exit_code == 128 + SIGTERM);
    process_capture_free(&capture);
}

static bool gone(pid_t pid) {
    double deadline = now_ms() + 1500;
    do {
        if (kill(pid, 0) < 0 && errno == ESRCH) return true;
        usleep(10000);
    } while (now_ms() < deadline);
    return false;
}

static void test_silent_descendant(void) {
    char *args[] = {executable, "--fixture", "silent_descendant", NULL};
    process_capture_t capture;
    assert(process_capture(executable, args, 2000, 128, &capture));
    pid_t descendant = (pid_t)strtol(capture.output, NULL, 10);
    assert(descendant > 1);
    bool cleaned = gone(descendant);
    if (!cleaned) kill(descendant, SIGKILL);
    assert(cleaned);
    assert(!capture.timed_out && capture.exit_code == 0);
    process_capture_free(&capture);
}

static void test_deadline(const char *mode) {
    char *args[] = {executable, "--fixture", (char *)mode, NULL};
    process_capture_t capture;
    double started = now_ms();
    assert(!process_capture(executable, args, 150, 1024, &capture));
    assert(capture.timed_out && !capture.spawn_error);
    assert(capture.exit_code == 128 + SIGKILL);
    assert(now_ms() - started >= 150 && now_ms() - started < 1500);
    assert(capture.truncated == !strcmp(mode, "flood"));
    process_capture_free(&capture);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--fixture")) return fixture(argv[2]);
    assert(realpath(argv[0], executable));
    if (argc == 1 || !strcmp(argv[1], "diagnostics")) test_final_diagnostics();
    if (argc == 1 || !strcmp(argv[1], "signals")) test_signal_isolation();
    if (argc == 1 || !strcmp(argv[1], "descendants")) test_silent_descendant();
    if (argc == 1 || !strcmp(argv[1], "deadlines")) {
        test_deadline("flood"); test_deadline("closed_streams"); test_deadline("escaped_root");
    }
    puts("process capture lifecycle: termination output, signal isolation/status, silent descendants and bounded floods passed");
    return 0;
}
