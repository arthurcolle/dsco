#define _POSIX_C_SOURCE 200809L
#include "swarm_reactor.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t signals_seen;
static void signal_noop(int sig) { (void)sig; ++signals_seen; }
static long long ms(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void sleep_ms(int delay) {
    struct timespec t = {delay / 1000, (delay % 1000) * 1000000L};
    while (nanosleep(&t, &t) < 0)
        assert(errno == EINTR);
}
static void reap(pid_t child, int code) {
    int status;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    assert(result == child && WIFEXITED(status) && WEXITSTATUS(status) == code);
}
static pid_t sleeper(int delay, int code) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        sleep_ms(delay);
        _exit(code);
    }
    return child;
}

static void test_empty_and_arguments(void) {
    long long start = ms();
    assert(swarm_reactor_wait(NULL, 0, 0) == 0);
    assert(swarm_reactor_wait(NULL, 0, -1) == 0);
    assert(swarm_reactor_wait(NULL, 0, 10000) == 0);
    struct pollfd ignored = {.fd = -1, .events = POLLIN, .revents = POLLIN};
    pid_t ignored_pids[] = {0, -1};
    assert(swarm_reactor_wait_children(&ignored, 1, ignored_pids, 2, -1) == 0);
    assert(ignored.revents == 0);
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    ignored.fd = pipefd[0];
    ignored.events = 0;
    assert(swarm_reactor_wait(&ignored, 1, -1) == 0);
    ignored.events = POLLIN;
    assert(swarm_reactor_wait(&ignored, 1, 0) == 0);
    close(pipefd[0]);
    close(pipefd[1]);
    assert(swarm_reactor_wait_children(NULL, 1, NULL, 0, 0) == -1 && errno == EINVAL);
    assert(swarm_reactor_wait_children(NULL, 0, NULL, 1, 0) == -1 && errno == EINVAL);
    assert(ms() - start < 150);
}

static void test_readiness_and_eof(void) {
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    struct pollfd fds[] = {{pipefd[0], POLLIN, 0}, {pipefd[0], POLLIN, 0}};
    assert(write(pipefd[1], "x", 1) == 1);
    long long start = ms();
    assert(swarm_reactor_wait(fds, 2, 10000) == 2);
    assert((fds[0].revents & POLLIN) && (fds[1].revents & POLLIN));
    char c;
    assert(read(pipefd[0], &c, 1) == 1);
    struct pollfd writer = {pipefd[1], POLLOUT, 0};
    assert(swarm_reactor_wait(&writer, 1, -1) == 1);
    assert(writer.revents & POLLOUT);
    close(pipefd[1]);
    assert(swarm_reactor_wait(fds, 2, -1) == 2);
    assert((fds[0].revents & POLLHUP) && (fds[1].revents & POLLHUP));
    close(pipefd[0]);
    assert(swarm_reactor_wait(fds, 1, -1) == 1);
    assert(fds[0].revents & POLLNVAL);
    assert(ms() - start < 150);
}

static void test_negative_fd_wait(void) {
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    pid_t parent = getpid();
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(pipefd[0]);
        for (int i = 0; i < 6; ++i) {
            sleep_ms(40);
            assert(kill(parent, SIGUSR1) == 0);
        }
        assert(write(pipefd[1], "x", 1) == 1);
        _exit(0);
    }
    close(pipefd[1]);
    struct pollfd fd = {pipefd[0], POLLIN, 0};
    long long start = ms();
    assert(swarm_reactor_wait(&fd, 1, -7) == 1);
    long long elapsed = ms() - start;
    assert(fd.revents & POLLIN);
    assert(elapsed >= 210 && elapsed < 1500);
    close(pipefd[0]);
    reap(child, 0);
    printf("  indefinite fd wait with EINTR: %lld ms\n", elapsed);
}

static void test_silent_child_after_eof(void) {
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(pipefd[0]);
        assert(dup2(pipefd[1], STDOUT_FILENO) == STDOUT_FILENO);
        assert(dup2(pipefd[1], STDERR_FILENO) == STDERR_FILENO);
        close(pipefd[1]);
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        sleep_ms(300);
        _exit(23);
    }
    close(pipefd[1]);
    struct pollfd fd = {pipefd[0], POLLIN, 0};
    long long start = ms();
    assert(swarm_reactor_wait_children(&fd, 1, &child, 1, -1) == 1);
    assert(fd.revents & POLLHUP);
    assert(ms() - start < 200);
    close(pipefd[0]);
    assert(waitpid(child, NULL, WNOHANG) == 0);
    start = ms();
    assert(swarm_reactor_wait_children(NULL, 0, &child, 1, -1) == 1);
    long long elapsed = ms() - start;
    assert(elapsed >= 180 && elapsed < 1500);
    reap(child, 23); /* The reactor must not consume exit status. */
    printf("  silent child exit after stdio EOF: %lld ms\n", elapsed);
}

static void test_mixed_interests(void) {
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(pipefd[0]);
        sleep_ms(160);
        assert(write(pipefd[1], "x", 1) == 1);
        sleep_ms(200);
        _exit(0);
    }
    /* Keeping this writer open makes child exit independent of pipe EOF. */
    struct pollfd fds[] = {{pipefd[0], POLLIN, 0}, {pipefd[0], POLLIN, 0}};
    pid_t pids[] = {0, child, child, -1};
    long long start = ms();
    assert(swarm_reactor_wait_children(fds, 2, pids, 4, 0) == 0);
    assert(ms() - start < 100);
    assert(swarm_reactor_wait_children(fds, 2, pids, 4, 2000) == 2);
    assert((fds[0].revents & POLLIN) && (fds[1].revents & POLLIN));
    assert(waitpid(child, NULL, WNOHANG) == 0);
    char c;
    assert(read(pipefd[0], &c, 1) == 1);
    start = ms();
    assert(swarm_reactor_wait_children(fds, 2, pids, 4, -1) == 1);
    assert(fds[0].revents == 0 && fds[1].revents == 0);
    assert(ms() - start >= 120 && ms() - start < 1500);
    reap(child, 0);
    close(pipefd[0]);
    close(pipefd[1]);
}

static void test_exit_before_registration(void) {
    pid_t child = sleeper(0, 17);
    siginfo_t info = {0};
    int result;
    do {
        result = waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT);
    } while (result < 0 && errno == EINTR);
    assert(result == 0 && info.si_pid == child);
    long long start = ms();
    assert(swarm_reactor_wait_children(NULL, 0, &child, 1, -1) == 1);
    assert(ms() - start < 150);
    reap(child, 17);
    start = ms();
    /* Already reaped: EV_ADD must handle ESRCH, including a zero-time probe. */
    assert(swarm_reactor_wait_children(NULL, 0, &child, 1, -1) == 1);
    assert(swarm_reactor_wait_children(NULL, 0, &child, 1, 0) == 1);
    assert(ms() - start < 150);
    for (int i = 0; i < 40; ++i) {
        child = sleeper(0, 0);
        assert(swarm_reactor_wait_children(NULL, 0, &child, 1, 1000) == 1);
        reap(child, 0);
    }
}

static void test_deadline_eintr(int watch_child) {
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    struct pollfd fd = {pipefd[0], POLLIN, POLLIN};
    pid_t parent = getpid();
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        for (int i = 0; i < 55; ++i) {
            sleep_ms(10);
            assert(kill(parent, SIGUSR1) == 0);
        }
        _exit(0);
    }
    signals_seen = 0;
    long long start = ms();
    int result = watch_child ? swarm_reactor_wait_children(&fd, 1, &child, 1, 240)
                             : swarm_reactor_wait(&fd, 1, 240);
    long long elapsed = ms() - start;
    assert(result == 0 && fd.revents == 0);
    assert(signals_seen >= 5);
    assert(elapsed >= 220 && elapsed < 450);
    reap(child, 0);
    close(pipefd[0]);
    close(pipefd[1]);
    printf("  %s finite deadline under EINTR: %lld ms\n",
           watch_child ? "fd/process" : "fd-only", elapsed);
}

int main(void) {
    struct sigaction sa = {0}, old_usr, old_chld, before_usr, before_chld, after;
    sa.sa_handler = signal_noop;
    sigemptyset(&sa.sa_mask);
    assert(sigaction(SIGUSR1, &sa, &old_usr) == 0);
    assert(sigaction(SIGCHLD, &sa, &old_chld) == 0);
    assert(sigaction(SIGUSR1, NULL, &before_usr) == 0);
    assert(sigaction(SIGCHLD, NULL, &before_chld) == 0);
    alarm(15); /* A regression to an unobservable child must fail, not hang. */
    test_empty_and_arguments();
    test_readiness_and_eof();
    test_negative_fd_wait();
    test_silent_child_after_eof();
    test_mixed_interests();
    test_exit_before_registration();
    test_deadline_eintr(0);
    test_deadline_eintr(1);
    assert(sigaction(SIGUSR1, NULL, &after) == 0);
    assert(after.sa_handler == before_usr.sa_handler && after.sa_flags == before_usr.sa_flags);
    for (int sig = 1; sig < 32; ++sig)
        assert(sigismember(&after.sa_mask, sig) == sigismember(&before_usr.sa_mask, sig));
    assert(sigaction(SIGCHLD, NULL, &after) == 0);
    assert(after.sa_handler == before_chld.sa_handler && after.sa_flags == before_chld.sa_flags);
    for (int sig = 1; sig < 32; ++sig)
        assert(sigismember(&after.sa_mask, sig) == sigismember(&before_chld.sa_mask, sig));
    assert(sigaction(SIGUSR1, &old_usr, NULL) == 0);
    assert(sigaction(SIGCHLD, &old_chld, NULL) == 0);
    alarm(0);
    puts("PASS reactor: 8 scenarios; readiness, EOF, silent exits, exit races, timeouts, EINTR, signal dispositions");
    return 0;
}
