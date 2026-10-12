/* Standalone: cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror
 * -Iinclude src/event_loop.c tests/test_event_loop_backend.c -o /tmp/ev-test
 * Repeat with -DEV_FORCE_POLL. No project runtime or external services needed.
 */
#include "event_loop.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static long long clock_ms(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct { int calls; ev_mask_t events; } seen_t;
static void record(int fd, ev_mask_t events, void *ctx) {
    (void)fd;
    seen_t *s = ctx;
    s->calls++;
    s->events |= events;
}
static void clear(seen_t *s) { *s = (seen_t){0}; }

static void test_readiness_eof(void) {
    ev_loop_t *loop = ev_loop_new();
    assert(loop);
    for (int socket = 0; socket < 2; socket++) {
        int fd[2];
        assert((socket ? socketpair(AF_UNIX, SOCK_STREAM, 0, fd) : pipe(fd)) == 0);
        seen_t s = {0};
        assert(ev_watch_fd(loop, fd[0], EV_READ, record, &s) == 0);
        assert(write(fd[1], "x", 1) == 1);
        assert(ev_loop_poll(loop, 100) > 0);
        assert(s.events == EV_READ);
        char c;
        assert(read(fd[0], &c, 1) == 1 && c == 'x');
        clear(&s);
        assert(close(fd[1]) == 0);
        assert(ev_loop_poll(loop, 100) > 0);
        assert(s.events & EV_ERROR);
        assert(!(s.events & ~(EV_READ | EV_ERROR))); /* Public ERROR is 4. */
        assert(read(fd[0], &c, 1) == 0);
        assert(ev_unwatch_fd(loop, fd[0]) == 0);
        close(fd[0]);
    }
    assert(ev_loop_stats(loop).active_fds == 0);
    ev_loop_free(loop);
}

static void test_mask_replacement(void) {
    ev_loop_t *loop = ev_loop_new();
    int fd[2];
    assert(loop && socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    seen_t old = {0}, current = {0};
    assert(ev_watch_fd(loop, fd[0], EV_READ | EV_WRITE, record, &old) == 0);
    assert(ev_loop_poll(loop, 100) > 0 && old.events == EV_WRITE);
    assert(ev_watch_fd(loop, fd[0], EV_READ, record, &current) == 0);
    long long start = clock_ms();
    assert(ev_loop_poll(loop, 40) == 0);
    assert(clock_ms() - start >= 20); /* No leftover WRITE filter busy-loop. */
    assert(old.calls == 1 && current.calls == 0);
    assert(write(fd[1], "x", 1) == 1);
    assert(ev_loop_poll(loop, 100) > 0 && current.events == EV_READ);

    /* Leave readable data pending, but make WRITE unready. */
    int flags = fcntl(fd[0], F_GETFL);
    assert(flags >= 0 && fcntl(fd[0], F_SETFL, flags | O_NONBLOCK) == 0);
    char buf[4096] = {0};
    while (write(fd[0], buf, sizeof(buf)) > 0) {}
    assert(errno == EAGAIN || errno == EWOULDBLOCK);
    clear(&current);
    assert(ev_watch_fd(loop, fd[0], EV_WRITE, record, &current) == 0);
    start = clock_ms();
    assert(ev_loop_poll(loop, 40) == 0);
    assert(clock_ms() - start >= 20); /* No leftover READ filter either. */
    assert(current.calls == 0 && ev_loop_stats(loop).active_fds == 1);
    assert(ev_unwatch_fd(loop, fd[0]) == 0);
    close(fd[0]); close(fd[1]);
    ev_loop_free(loop);
}

static void test_slot_reuse(void) {
    ev_loop_t *loop = ev_loop_new();
    int fd[2];
    seen_t s = {0};
    assert(loop && pipe(fd) == 0);
    for (int i = 0; i < 1024; i++) {
        assert(ev_watch_fd(loop, fd[0], EV_READ, record, &s) == 0);
        assert(ev_loop_stats(loop).active_fds == 1);
        assert(ev_unwatch_fd(loop, fd[0]) == 0);
        assert(ev_loop_stats(loop).active_fds == 0);
    }
    assert(ev_watch_fd(loop, fd[0], EV_READ, record, &s) == 0);
    assert(write(fd[1], "x", 1) == 1);
    assert(ev_loop_poll(loop, 100) == 1 && s.calls == 1);
    ev_loop_free(loop);
    close(fd[0]); close(fd[1]);
}

static void test_invalid_registration(void) {
    ev_loop_t *loop = ev_loop_new();
    seen_t s = {0};
    int fd[2];
    assert(loop && pipe(fd) == 0);
    assert(ev_watch_fd(loop, -1, EV_READ, record, &s) == -1 && errno == EBADF);
    assert(ev_watch_fd(loop, fd[0], 0, record, &s) == -1 && errno == EINVAL);
    assert(ev_watch_fd(loop, fd[0], EV_READ, NULL, NULL) == -1 && errno == EINVAL);
    int closed = fd[1];
    close(closed);
    assert(ev_watch_fd(loop, closed, EV_READ, record, &s) == -1 && errno == EBADF);
    assert(ev_loop_stats(loop).active_fds == 0);
    assert(ev_watch_fd(loop, fd[0], EV_READ, record, &s) == 0);
    assert(ev_watch_fd(loop, fd[0], EV_TIMER, record, &s) == -1);
    assert(ev_loop_stats(loop).active_fds == 1);
    assert(ev_loop_poll(loop, 0) == 1 && (s.events & EV_ERROR));
#if defined(__linux__) && !defined(EV_FORCE_POLL)
    int unsupported = open("/dev/null", O_RDONLY);
    assert(unsupported >= 0);
    assert(ev_watch_fd(loop, unsupported, EV_READ, record, &s) == -1);
    assert(errno == EPERM && ev_loop_stats(loop).active_fds == 1);
    close(unsupported);
#endif
    assert(ev_unwatch_fd(loop, fd[0]) == 0);
    close(fd[0]);
    ev_loop_free(loop);
}

typedef struct {
    ev_loop_t *loop;
    int fd[2];
    int old_calls, new_calls, remove;
} batch_t;
static void replacement(int fd, ev_mask_t events, void *ctx) {
    (void)fd; (void)events;
    ((batch_t *)ctx)->new_calls++;
}
static void replace_batch(int fd, ev_mask_t events, void *ctx) {
    (void)fd; (void)events;
    batch_t *b = ctx;
    b->old_calls++;
    for (int i = 0; i < 2; i++) {
        if (b->remove) assert(ev_unwatch_fd(b->loop, b->fd[i]) == 0);
        assert(ev_watch_fd(b->loop, b->fd[i], EV_READ, replacement, b) == 0);
    }
}
static void test_stale_batch(void) {
    for (int remove = 0; remove <= 1; remove++) {
        ev_loop_t *loop = ev_loop_new();
        int a[2], b[2];
        assert(loop && pipe(a) == 0 && pipe(b) == 0);
        batch_t state = { .loop = loop, .fd = {a[0], b[0]}, .remove = remove };
        assert(ev_watch_fd(loop, a[0], EV_READ, replace_batch, &state) == 0);
        assert(ev_watch_fd(loop, b[0], EV_READ, replace_batch, &state) == 0);
        assert(write(a[1], "a", 1) == 1 && write(b[1], "b", 1) == 1);
        assert(ev_loop_poll(loop, 100) == 1);
        assert(state.old_calls == 1 && state.new_calls == 0);
        assert(ev_loop_poll(loop, 100) == 2 && state.new_calls == 2);
        ev_loop_free(loop);
        close(a[0]); close(a[1]); close(b[0]); close(b[1]);
    }
}
static void replace_self(int fd, ev_mask_t events, void *ctx) {
    (void)events;
    batch_t *b = ctx;
    b->old_calls++;
    assert(ev_unwatch_fd(b->loop, fd) == 0);
    assert(ev_watch_fd(b->loop, fd, EV_READ | EV_WRITE, replacement, b) == 0);
}
static void test_stale_filters(void) {
    ev_loop_t *loop = ev_loop_new();
    int fd[2];
    assert(loop && socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    batch_t b = {.loop = loop};
    assert(ev_watch_fd(loop, fd[0], EV_READ | EV_WRITE, replace_self, &b) == 0);
    assert(write(fd[1], "x", 1) == 1);
    assert(ev_loop_poll(loop, 100) == 1);
    assert(b.old_calls == 1 && b.new_calls == 0);
    assert(ev_loop_poll(loop, 100) > 0 && b.new_calls > 0);
    ev_loop_free(loop);
    close(fd[0]); close(fd[1]);
}

typedef struct { ev_loop_t *loop; int calls, next, victim, victim_calls; } timer_tst;
static void count_timer(int id, void *ctx) { (void)id; (*(int *)ctx)++; }
static void rearm_timer(int id, void *ctx) {
    timer_tst *t = ctx;
    t->calls++;
    ev_timer_cancel(t->loop, id);
    t->next = ev_timer_once(t->loop, 0, count_timer, &t->calls);
    assert(t->next >= 0 && t->next != id);
}
static void cancel_other(int id, void *ctx) {
    (void)id;
    timer_tst *t = ctx;
    t->calls++;
    ev_timer_cancel(t->loop, t->victim);
    assert(ev_timer_once(t->loop, 0, count_timer, &t->calls) >= 0);
}
static void test_timer_mutation(void) {
    for (int repeat = 0; repeat < 2; repeat++) {
        ev_loop_t *loop = ev_loop_new();
        timer_tst t = {.loop = loop};
        assert(loop);
        int id = repeat ? ev_timer_repeat(loop, 1, rearm_timer, &t)
                        : ev_timer_once(loop, 0, rearm_timer, &t);
        assert(id >= 0);
        struct timespec delay = {0, 3000000};
        assert(nanosleep(&delay, NULL) == 0);
        assert(ev_loop_poll(loop, 0) == 0 && t.calls == 1);
        assert(ev_loop_stats(loop).active_timers == 1);
        assert(ev_loop_poll(loop, 0) == 0 && t.calls == 2);
        assert(ev_loop_stats(loop).active_timers == 0);
        assert(ev_loop_stats(loop).timer_fires == 2);
        ev_loop_free(loop);
    }
    ev_loop_t *loop = ev_loop_new();
    assert(loop);
    timer_tst t = {.loop = loop};
    assert(ev_timer_once(loop, 0, cancel_other, &t) >= 0);
    t.victim = ev_timer_once(loop, 0, count_timer, &t.victim_calls);
    assert(t.victim >= 0);
    assert(ev_loop_poll(loop, 0) == 0);
    assert(t.calls == 1 && t.victim_calls == 0);
    assert(ev_loop_stats(loop).active_timers == 1);
    assert(ev_loop_poll(loop, 0) == 0 && t.calls == 2 && t.victim_calls == 0);
    assert(ev_loop_stats(loop).active_timers == 0);
    assert(ev_timer_once(loop, -1, count_timer, &t.calls) == -1);
    assert(ev_timer_repeat(loop, 0, count_timer, &t.calls) == -1);
    ev_loop_free(loop);
}

static void defer_count(void *ctx) { (*(int *)ctx)++; }
static void defer_again(void *ctx) {
    timer_tst *t = ctx;
    t->calls++;
    assert(ev_defer(t->loop, defer_again, ctx) == 0);
}
static void defer_stop(void *ctx) { ev_loop_stop(ctx); }
static void test_deferred(void) {
    ev_loop_t *loop = ev_loop_new();
    int calls = 0;
    assert(loop && ev_defer(loop, defer_count, &calls) == 0);
    long long start = clock_ms();
    assert(ev_loop_poll(loop, 1000) == 0);
    assert(clock_ms() - start < 500 && calls == 1);
    timer_tst t = {.loop = loop};
    assert(ev_defer(loop, defer_again, &t) == 0);
    assert(ev_loop_poll(loop, 1000) == 0 && t.calls == 2);
    assert(ev_loop_poll(loop, 1000) == 0 && t.calls == 4);
    ev_loop_free(loop);
    loop = ev_loop_new();
    assert(loop && ev_defer(loop, defer_stop, loop) == 0);
    start = clock_ms();
    ev_loop_run(loop);
    assert(clock_ms() - start < 500);
    ev_loop_free(loop);
}

int main(void) {
    alarm(15);
    ev_loop_t *loop = ev_loop_new();
    assert(loop);
#if defined(EV_FORCE_POLL)
    const char *expected = "poll";
#elif defined(__APPLE__)
    const char *expected = "kqueue";
#elif defined(__linux__)
    const char *expected = "epoll";
#else
    const char *expected = "poll";
#endif
    assert(!strcmp(ev_loop_backend(loop), expected));
    printf("backend=%s\n", ev_loop_backend(loop));
    ev_loop_free(loop);
#define RUN(test) do { test(); puts(#test ": PASS"); } while (0)
    RUN(test_readiness_eof);
    RUN(test_mask_replacement);
    RUN(test_slot_reuse);
    RUN(test_invalid_registration);
    RUN(test_stale_batch);
    RUN(test_stale_filters);
    RUN(test_timer_mutation);
    RUN(test_deferred);
    puts("8 test groups passed");
    return 0;
}
