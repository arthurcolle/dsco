#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "event_loop.h"
#include "scheduler_async.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#if defined(__APPLE__) && !defined(EV_FORCE_POLL)
#include <sys/event.h>
#endif

static double ms(clockid_t clock) {
    struct timespec ts; assert(clock_gettime(clock, &ts) == 0);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static void delay(int milliseconds) {
    struct timespec ts = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0) assert(errno == EINTR);
}
typedef struct { ev_loop_t *loop; int fd; } producer_t;
static void *notify(void *arg) {
    producer_t *p = arg; delay(1000);
    if (p->loop) assert(ev_loop_wake(p->loop) == 0);
    else assert(write(p->fd, "x", 1) == 1);
    return NULL;
}
static void once(int id, void *ctx) { (void)id; ++*(int *)ctx; }

typedef struct { sched_async_t async; scheduler_t *scheduler; int fd, done; } task_t;
static int reader(void *ctx) {
    task_t *t = ctx;
    SCHED_ASYNC_BEGIN(&t->async);
    SCHED_AWAIT_FD(&t->async, t->scheduler, t->fd, EV_READ);
    t->done = 1;
    SCHED_ASYNC_END(&t->async);
}
static int sleeper(void *ctx) {
    task_t *t = ctx;
    SCHED_ASYNC_BEGIN(&t->async);
    SCHED_AWAIT_SLEEP(&t->async, t->scheduler, 500);
    t->done = 1;
    SCHED_ASYNC_END(&t->async);
}
static int cyclic(void *ctx) {
    task_t *t = ctx;
    sched_wait_task(t->scheduler, t->scheduler->current);
    return 1;
}

int main(void) {
    alarm(10); /* Test harness bound, not an execution policy. */
    ev_loop_t *loop = ev_loop_new(); assert(loop);
    printf("backend=%s\n", ev_loop_backend(loop));
    producer_t p = {.loop = loop}; pthread_t thread;
    assert(pthread_create(&thread, NULL, notify, &p) == 0);
    double start = ms(CLOCK_MONOTONIC), cpu = ms(CLOCK_THREAD_CPUTIME_ID);
    assert(ev_loop_poll(loop, -1) == 0);
    double elapsed = ms(CLOCK_MONOTONIC) - start, used = ms(CLOCK_THREAD_CPUTIME_ID) - cpu;
    assert(pthread_join(thread, NULL) == 0);
    ev_stats_t stats = ev_loop_stats(loop);
    assert(elapsed >= 800 && elapsed < 3000 && used < 100);
    assert(stats.polls == 1 && stats.wake_events == 1);
    printf("indefinite_wait_ms=%.3f owner_cpu_ms=%.3f kernel_waits=%llu notifications=%llu\n",
           elapsed, used, (unsigned long long)stats.polls, (unsigned long long)stats.wake_events);
    /* A posted notification survives until the owner waits; bursts coalesce. */
    for (int i = 0; i < 20000; ++i) assert(ev_loop_wake(loop) == 0);
    assert(ev_loop_poll(loop, -1) == 0);
    assert(ev_loop_stats(loop).wake_events == 2);
    assert(ev_loop_poll(loop, 0) == 0 && ev_loop_stats(loop).wake_events == 2);
    int fired = 0;
    assert(ev_timer_once(loop, 100, once, &fired) >= 0);
    assert(ev_loop_poll(loop, -1) == 0 && fired == 1);
    ev_loop_free(loop);

    int fds[2]; assert(pipe(fds) == 0);
    scheduler_t scheduler; sched_init(&scheduler);
    task_t t = {.scheduler = &scheduler, .fd = fds[0]};
    p = (producer_t){.fd = fds[1]};
    assert(pthread_create(&thread, NULL, notify, &p) == 0);
    assert(sched_run_sync(&scheduler, reader, &t, "await pipe") == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(t.done && scheduler.tick_count == 2);
    printf("io_await_scheduler_ticks=%llu\n", (unsigned long long)scheduler.tick_count);
    close(fds[0]); close(fds[1]); sched_destroy(&scheduler);
    sched_init(&scheduler); t = (task_t){.scheduler = &scheduler};
    assert(sched_spawn(&scheduler, sleeper, &t, "requested sleep", SCHED_PRIO_NORMAL) >= 0);
    assert(sched_run(&scheduler, 0) == 0 && t.done);
    assert(scheduler.tick_count <= 3);
    printf("timer_await_scheduler_ticks=%llu\n", (unsigned long long)scheduler.tick_count);
    sched_destroy(&scheduler);
    sched_init(&scheduler); t = (task_t){.scheduler = &scheduler};
    assert(sched_run_sync(&scheduler, cyclic, &t, "cycle") == -1 && errno == EDEADLK);
    sched_destroy(&scheduler);

#if defined(__APPLE__) && !defined(EV_FORCE_POLL)
    /* Native process exit with no pipe data: NOTE_EXIT wakes the owner, which
     * retains sole responsibility for waitpid. Registration precedes release. */
    int gate[2]; assert(pipe(gate) == 0);
    int kq = kqueue(); assert(kq >= 0);
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(gate[1]); char byte;
        if (read(gate[0], &byte, 1) != 1) _exit(99);
        close(gate[0]); close(kq); close(STDOUT_FILENO); close(STDERR_FILENO);
        delay(250); _exit(7);
    }
    close(gate[0]); struct kevent change, event;
    EV_SET(&change, child, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    assert(kevent(kq, &change, 1, NULL, 0, NULL) == 0);
    assert(write(gate[1], "x", 1) == 1); close(gate[1]);
    start = ms(CLOCK_MONOTONIC);
    assert(kevent(kq, NULL, 0, &event, 1, NULL) == 1);
    assert(event.filter == EVFILT_PROC && (event.fflags & NOTE_EXIT));
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    printf("native_process_exit_ms=%.3f exit_code=7 kernel_waits=1\n", ms(CLOCK_MONOTONIC) - start);
    close(kq);
#endif
    puts("PASS: notification, coalescing, indefinite wait, exact timer, nonpolling await and cycle diagnosis");
}
