#define _POSIX_C_SOURCE 200809L
#include "scheduler_async.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

typedef struct {
    sched_async_t async;
    scheduler_t *s;
    int fd;
    int peer;
    int done;
    char byte;
    task_id_t child;
    ev_mask_t events;
} context_t;

static int consumer(void *arg) {
    context_t *c = arg;
    SCHED_ASYNC_BEGIN(&c->async);
    SCHED_AWAIT_FD(&c->async, c->s, c->fd, EV_READ);
    c->events = sched_current_events(c->s);
    assert(c->events & EV_READ);
    ++c->done;
    SCHED_AWAIT_SLEEP(&c->async, c->s, 3);
    ++c->done;
    SCHED_ASYNC_END(&c->async);
}
static int read_retry(void *arg) {
    context_t *c = arg;
    SCHED_ASYNC_BEGIN(&c->async);
    while (read(c->fd, &c->byte, 1) < 0) {
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
        SCHED_AWAIT_FD(&c->async, c->s, c->fd, EV_READ);
    }
    c->done = (unsigned char)c->byte;
    SCHED_ASYNC_END(&c->async);
}
static int producer(void *arg) {
    context_t *c = arg;
    SCHED_ASYNC_BEGIN(&c->async);
    SCHED_AWAIT_SLEEP(&c->async, c->s, 20);
    assert(write(c->fd, "x", 1) == 1);
    SCHED_ASYNC_END(&c->async);
}
static int parent(void *arg) {
    context_t *c = arg;
    SCHED_ASYNC_BEGIN(&c->async);
    SCHED_AWAIT_TASK(&c->async, c->s, c->child);
    c->done = 1;
    SCHED_ASYNC_END(&c->async);
}
static int await_error(void *arg) {
    context_t *c = arg;
    SCHED_ASYNC_BEGIN(&c->async);
    SCHED_AWAIT_FD(&c->async, c->s, c->fd, EV_READ);
    c->events = sched_current_events(c->s);
    assert(c->events & EV_ERROR);
    c->done = 1;
    SCHED_ASYNC_END(&c->async);
}
static int writable_then_read(void *arg) {
    context_t *c = arg;
    SCHED_ASYNC_BEGIN(&c->async);
    SCHED_AWAIT_FD(&c->async, c->s, c->fd, EV_WRITE);
    assert(sched_current_events(c->s) & EV_WRITE);
    SCHED_AWAIT_FD(&c->async, c->s, c->fd, EV_READ);
    assert(sched_current_events(c->s) & EV_READ);
    c->done = 1;
    SCHED_ASYNC_END(&c->async);
}
static int cancel_consumer(void *arg) {
    context_t *c = arg;
    SCHED_ASYNC_BEGIN(&c->async);
    SCHED_AWAIT_SLEEP(&c->async, c->s, 3);
    assert(sched_cancel(c->s, c->child));
    SCHED_ASYNC_END(&c->async);
}
static double clock_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
int main(void) {
    alarm(10); /* A broken await must fail, never hang the suite. */
    scheduler_t s;
    int p[2];
    assert(pipe(p) == 0);
    assert(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0);
    sched_init(&s);
    context_t a = {.s=&s,.fd=p[0]}, b = {.s=&s,.fd=p[0]};
    context_t w = {.s=&s,.fd=p[1]}, par = {.s=&s};
    par.child = sched_spawn(&s, consumer, &a, "reader", SCHED_PRIO_HIGH);
    sched_spawn(&s, consumer, &b, "shared-fd", SCHED_PRIO_HIGH);
    sched_spawn(&s, producer, &w, "writer", SCHED_PRIO_NORMAL);
    sched_spawn(&s, parent, &par, "parent", SCHED_PRIO_CRITICAL);
    double start = clock_ms();
    assert(sched_run(&s, 1000) == 0);
    assert(clock_ms() - start >= 20);
    assert(a.done == 2 && b.done == 2 && par.done == 1);
    assert(s.tick_count < 30); /* no writable-fd busy spin or 1ms polling */
    close(p[0]); close(p[1]);

    assert(pipe(p) == 0);
    sched_init(&s);
    context_t eof = {.s=&s,.fd=p[0]};
    close(p[1]);
    assert(sched_run_sync(&s, await_error, &eof, "EOF") == 0);
    assert(eof.done);
    close(p[0]);

    sched_init(&s);
    context_t bad = {.s=&s,.fd=-1};
    assert(sched_run_sync(&s, await_error, &bad, "invalid") == 0);
    assert(bad.done);

    assert(pipe(p) == 0);
    assert(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0);
    sched_init(&s);
    context_t retry = {.s=&s,.fd=p[0]}, delayed = {.s=&s,.fd=p[1]};
    sched_spawn(&s, read_retry, &retry, "retry-EAGAIN", SCHED_PRIO_HIGH);
    sched_spawn(&s, producer, &delayed, "delayed-write", SCHED_PRIO_NORMAL);
    assert(sched_run(&s, 1000) == 0);
    assert(retry.done == 'x');
    close(p[0]); close(p[1]);

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    sched_init(&s);
    context_t rw = {.s=&s,.fd=sockets[0]};
    context_t send = {.s=&s,.fd=sockets[1]};
    sched_spawn(&s, writable_then_read, &rw, "mask-change", SCHED_PRIO_HIGH);
    sched_spawn(&s, producer, &send, "send", SCHED_PRIO_NORMAL);
    assert(sched_run(&s, 1000) == 0);
    assert(rw.done && s.tick_count < 20);
    close(sockets[0]); close(sockets[1]);

    assert(pipe(p) == 0);
    sched_init(&s);
    context_t blocked = {.s=&s,.fd=p[0]}, cancel = {.s=&s};
    cancel.child = sched_spawn(&s, consumer, &blocked, "cancelled", SCHED_PRIO_HIGH);
    sched_spawn(&s, cancel_consumer, &cancel, "cancel", SCHED_PRIO_NORMAL);
    assert(sched_run(&s, 1000) == 0);
    assert(s.tasks[cancel.child].state == TASK_CANCELLED && !blocked.done);
    close(p[0]); close(p[1]);
    sched_destroy(&s);
    puts("PASS async fd read/write, shared fd, timers, task await, EOF, invalid fd, cancellation");
}
