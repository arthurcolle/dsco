#include "scheduler_io.h"
#include "scheduler.h"
#include "event_loop.h"
#include <stdlib.h>

struct sched_io {
    ev_loop_t *loop;
    scheduler_t *scheduler;
    int fds[SCHED_MAX_TASKS];
    ev_mask_t masks[SCHED_MAX_TASKS];
    int count;
};

/* Readiness is a hint, not a completed read/write. Callers use nonblocking
 * descriptors and await again on EAGAIN. Resume every matching waiter. */
static void ready(int fd, ev_mask_t events, void *ctx) {
    sched_io_t *io = ctx;
    scheduler_t *s = io->scheduler;
    for (int i = 0; i < s->task_count; ++i) {
        sched_task_t *t = &s->tasks[i];
        ev_mask_t mask = t->wait_events ? t->wait_events : EV_READ;
        if (t->state != TASK_WAITING_IO || t->wait_fd != fd ||
            !(events & (mask | EV_ERROR)))
            continue;
        t->ready_events = events;
        t->state = TASK_READY;
        int p = t->priority;
        if (p < 0 || p >= SCHED_PRIO_COUNT)
            p = SCHED_PRIO_NORMAL;
        int n = s->run_queue_len[p];
        bool queued = false;
        for (int j = 0; j < n; ++j)
            if (s->run_queue[p][j] == t->id)
                queued = true;
        if (!queued && n < SCHED_MAX_TASKS)
            s->run_queue[p][s->run_queue_len[p]++] = t->id;
    }
}

sched_io_t *sched_io_new(void) {
    sched_io_t *io = calloc(1, sizeof(*io));
    if (!io)
        return NULL;
    io->loop = ev_loop_new();
    if (!io->loop) {
        free(io);
        return NULL;
    }
    return io;
}

void sched_io_free(sched_io_t *io) {
    if (io) {
        ev_loop_free(io->loop);
        free(io);
    }
}

int sched_io_poll(sched_io_t *io, scheduler_t *s, int timeout_ms) {
    int fds[SCHED_MAX_TASKS];
    ev_mask_t masks[SCHED_MAX_TASKS];
    int count = 0;
    io->scheduler = s;
    for (int i = 0; i < s->task_count; ++i) {
        sched_task_t *t = &s->tasks[i];
        if (t->state != TASK_WAITING_IO)
            continue;
        if (t->wait_fd < 0) {
            ready(t->wait_fd, EV_ERROR, io);
            continue;
        }
        int j;
        for (j = 0; j < count && fds[j] != t->wait_fd; ++j) {}
        if (j == count) {
            fds[count] = t->wait_fd;
            masks[count++] = 0;
        }
        masks[j] |= t->wait_events ? t->wait_events : EV_READ;
    }
    for (int i = 0; i < io->count; ++i) {
        int j;
        for (j = 0; j < count && fds[j] != io->fds[i]; ++j) {}
        if (j == count)
            ev_unwatch_fd(io->loop, io->fds[i]);
    }
    int installed = 0;
    for (int i = 0; i < count; ++i) {
        int j;
        for (j = 0; j < io->count && io->fds[j] != fds[i]; ++j) {}
        if (j == io->count || io->masks[j] != masks[i]) {
            if (ev_watch_fd(io->loop, fds[i], masks[i], ready, io) < 0) {
                ev_unwatch_fd(io->loop, fds[i]);
                ready(fds[i], EV_ERROR, io);
                continue;
            }
        }
        fds[installed] = fds[i];
        masks[installed++] = masks[i];
    }
    io->count = installed;
    for (int i = 0; i < installed; ++i) {
        io->fds[i] = fds[i];
        io->masks[i] = masks[i];
    }
    for (int p = 0; p < SCHED_PRIO_COUNT; ++p)
        if (s->run_queue_len[p])
            return 0;
    return ev_loop_poll(io->loop, timeout_ms);
}
