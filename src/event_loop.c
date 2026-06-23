#include "event_loop.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#ifdef __APPLE__
#include <sys/event.h>
#include <unistd.h>
#define EV_USE_KQUEUE 1
#else
#include <poll.h>
#define EV_USE_KQUEUE 0
#endif

/* ── Constants ─────────────────────────────────────────────────────── */

#define EV_MAX_FDS 256
#define EV_MAX_TIMERS 64
#define EV_MAX_DEFERRED 128

/* ── Internal types ────────────────────────────────────────────────── */

typedef struct {
    int fd;
    ev_mask_t mask;
    ev_fd_cb cb;
    void *ctx;
    bool active;
} ev_fd_watcher_t;

typedef struct {
    int id;
    uint64_t deadline_ms;
    int interval_ms; /* 0 = one-shot */
    ev_timer_cb cb;
    void *ctx;
    bool active;
} ev_timer_t;

typedef struct {
    ev_defer_cb cb;
    void *ctx;
} ev_deferred_t;

struct ev_loop {
#if EV_USE_KQUEUE
    int kq;
#endif
    ev_fd_watcher_t fds[EV_MAX_FDS];
    int fd_count;

    ev_timer_t timers[EV_MAX_TIMERS];
    int timer_next_id;

    ev_deferred_t deferred[EV_MAX_DEFERRED];
    int defer_head;
    int defer_tail;
    int defer_count;

    volatile int stop_flag;
    ev_stats_t stats;
};

/* ── Monotonic clock ───────────────────────────────────────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

ev_loop_t *ev_loop_new(void) {
    ev_loop_t *loop = calloc(1, sizeof(ev_loop_t));
    if (!loop)
        return NULL;

#if EV_USE_KQUEUE
    loop->kq = kqueue();
    if (loop->kq < 0) {
        free(loop);
        return NULL;
    }
#endif

    for (int i = 0; i < EV_MAX_TIMERS; i++)
        loop->timers[i].active = false;

    return loop;
}

void ev_loop_free(ev_loop_t *loop) {
    if (!loop)
        return;
#if EV_USE_KQUEUE
    close(loop->kq);
#endif
    free(loop);
}

/* ── FD watching ───────────────────────────────────────────────────── */

int ev_watch_fd(ev_loop_t *loop, int fd, ev_mask_t mask, ev_fd_cb cb, void *ctx) {
    if (!loop || fd < 0)
        return -1;

    /* Check if already watched — update in place */
    for (int i = 0; i < loop->fd_count; i++) {
        if (loop->fds[i].fd == fd && loop->fds[i].active) {
            loop->fds[i].mask = mask;
            loop->fds[i].cb = cb;
            loop->fds[i].ctx = ctx;
#if EV_USE_KQUEUE
            struct kevent kev[2];
            int n = 0;
            if (mask & EV_READ)
                EV_SET(&kev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
            if (mask & EV_WRITE)
                EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
            if (n > 0)
                kevent(loop->kq, kev, n, NULL, 0, NULL);
#endif
            return 0;
        }
    }

    if (loop->fd_count >= EV_MAX_FDS)
        return -1;

    int idx = loop->fd_count++;
    loop->fds[idx].fd = fd;
    loop->fds[idx].mask = mask;
    loop->fds[idx].cb = cb;
    loop->fds[idx].ctx = ctx;
    loop->fds[idx].active = true;
    loop->stats.active_fds++;

#if EV_USE_KQUEUE
    struct kevent kev[2];
    int n = 0;
    if (mask & EV_READ)
        EV_SET(&kev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (mask & EV_WRITE)
        EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
    if (n > 0)
        kevent(loop->kq, kev, n, NULL, 0, NULL);
#endif

    return 0;
}

int ev_unwatch_fd(ev_loop_t *loop, int fd) {
    if (!loop)
        return -1;
    for (int i = 0; i < loop->fd_count; i++) {
        if (loop->fds[i].fd == fd && loop->fds[i].active) {
#if EV_USE_KQUEUE
            struct kevent kev[2];
            int n = 0;
            if (loop->fds[i].mask & EV_READ)
                EV_SET(&kev[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            if (loop->fds[i].mask & EV_WRITE)
                EV_SET(&kev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
            if (n > 0)
                kevent(loop->kq, kev, n, NULL, 0, NULL);
#endif
            loop->fds[i].active = false;
            loop->stats.active_fds--;
            return 0;
        }
    }
    return -1;
}

/* ── Timers ────────────────────────────────────────────────────────── */

static int alloc_timer(ev_loop_t *loop, int ms, int interval, ev_timer_cb cb, void *ctx) {
    if (!loop)
        return -1;
    for (int i = 0; i < EV_MAX_TIMERS; i++) {
        if (!loop->timers[i].active) {
            loop->timers[i].id = loop->timer_next_id++;
            loop->timers[i].deadline_ms = now_ms() + (uint64_t)ms;
            loop->timers[i].interval_ms = interval;
            loop->timers[i].cb = cb;
            loop->timers[i].ctx = ctx;
            loop->timers[i].active = true;
            loop->stats.active_timers++;
            return loop->timers[i].id;
        }
    }
    return -1;
}

int ev_timer_once(ev_loop_t *loop, int ms, ev_timer_cb cb, void *ctx) {
    return alloc_timer(loop, ms, 0, cb, ctx);
}

int ev_timer_repeat(ev_loop_t *loop, int interval_ms, ev_timer_cb cb, void *ctx) {
    return alloc_timer(loop, interval_ms, interval_ms, cb, ctx);
}

void ev_timer_cancel(ev_loop_t *loop, int timer_id) {
    if (!loop)
        return;
    for (int i = 0; i < EV_MAX_TIMERS; i++) {
        if (loop->timers[i].active && loop->timers[i].id == timer_id) {
            loop->timers[i].active = false;
            loop->stats.active_timers--;
            return;
        }
    }
}

/* ── Deferred ──────────────────────────────────────────────────────── */

int ev_defer(ev_loop_t *loop, ev_defer_cb cb, void *ctx) {
    if (!loop || loop->defer_count >= EV_MAX_DEFERRED)
        return -1;
    int idx = loop->defer_tail;
    loop->deferred[idx].cb = cb;
    loop->deferred[idx].ctx = ctx;
    loop->defer_tail = (loop->defer_tail + 1) % EV_MAX_DEFERRED;
    loop->defer_count++;
    return 0;
}

static void run_deferred(ev_loop_t *loop) {
    while (loop->defer_count > 0) {
        ev_deferred_t d = loop->deferred[loop->defer_head];
        loop->defer_head = (loop->defer_head + 1) % EV_MAX_DEFERRED;
        loop->defer_count--;
        if (d.cb) {
            d.cb(d.ctx);
            loop->stats.deferred_runs++;
        }
    }
}

/* ── Fire timers ───────────────────────────────────────────────────── */

static void fire_timers(ev_loop_t *loop) {
    uint64_t t = now_ms();
    for (int i = 0; i < EV_MAX_TIMERS; i++) {
        ev_timer_t *tm = &loop->timers[i];
        if (!tm->active)
            continue;
        if (t >= tm->deadline_ms) {
            tm->cb(tm->id, tm->ctx);
            loop->stats.timer_fires++;
            if (tm->interval_ms > 0) {
                tm->deadline_ms = t + (uint64_t)tm->interval_ms;
            } else {
                tm->active = false;
                loop->stats.active_timers--;
            }
        }
    }
}

/* ── Compute next timer deadline ───────────────────────────────────── */

static int next_timer_ms(ev_loop_t *loop, int max_ms) {
    uint64_t t = now_ms();
    int min = max_ms;
    for (int i = 0; i < EV_MAX_TIMERS; i++) {
        if (!loop->timers[i].active)
            continue;
        int64_t diff = (int64_t)loop->timers[i].deadline_ms - (int64_t)t;
        int d = diff <= 0 ? 0 : (int)diff;
        if (d < min)
            min = d;
    }
    return min;
}

/* ── Poll ──────────────────────────────────────────────────────────── */

int ev_loop_poll(ev_loop_t *loop, int timeout_ms) {
    if (!loop)
        return -1;

    /* Run deferred callbacks first */
    run_deferred(loop);

    /* Compute effective timeout from timers */
    int wait_ms = timeout_ms;
    if (wait_ms < 0)
        wait_ms = 60000; /* cap infinite at 60s */
    wait_ms = next_timer_ms(loop, wait_ms);

    int events_fired = 0;

#if EV_USE_KQUEUE
    struct kevent events[64];
    struct timespec ts;
    ts.tv_sec = wait_ms / 1000;
    ts.tv_nsec = (wait_ms % 1000) * 1000000L;

    int nev = kevent(loop->kq, NULL, 0, events, 64, &ts);
    loop->stats.polls++;

    for (int i = 0; i < nev; i++) {
        int fd = (int)events[i].ident;
        /* Find the watcher */
        for (int j = 0; j < loop->fd_count; j++) {
            if (loop->fds[j].fd == fd && loop->fds[j].active) {
                ev_mask_t fired = 0;
                if (events[i].filter == EVFILT_READ)
                    fired |= EV_READ;
                if (events[i].filter == EVFILT_WRITE)
                    fired |= EV_WRITE;
                if (events[i].flags & EV_ERROR)
                    fired |= EV_ERROR;
                loop->fds[j].cb(fd, fired, loop->fds[j].ctx);
                loop->stats.fd_events++;
                events_fired++;
                break;
            }
        }
    }
#else
    /* poll() fallback */
    struct pollfd pfds[EV_MAX_FDS];
    int pfd_count = 0;
    int pfd_map[EV_MAX_FDS]; /* maps pfd index -> fds index */

    for (int i = 0; i < loop->fd_count; i++) {
        if (!loop->fds[i].active)
            continue;
        pfds[pfd_count].fd = loop->fds[i].fd;
        pfds[pfd_count].events = 0;
        if (loop->fds[i].mask & EV_READ)
            pfds[pfd_count].events |= POLLIN;
        if (loop->fds[i].mask & EV_WRITE)
            pfds[pfd_count].events |= POLLOUT;
        pfds[pfd_count].revents = 0;
        pfd_map[pfd_count] = i;
        pfd_count++;
    }

    int ret = poll(pfds, (unsigned int)pfd_count, wait_ms);
    loop->stats.polls++;

    if (ret > 0) {
        for (int i = 0; i < pfd_count; i++) {
            if (pfds[i].revents == 0)
                continue;
            int idx = pfd_map[i];
            ev_mask_t fired = 0;
            if (pfds[i].revents & POLLIN)
                fired |= EV_READ;
            if (pfds[i].revents & POLLOUT)
                fired |= EV_WRITE;
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                fired |= EV_ERROR;
            loop->fds[idx].cb(loop->fds[idx].fd, fired, loop->fds[idx].ctx);
            loop->stats.fd_events++;
            events_fired++;
        }
    }
#endif

    /* Fire expired timers */
    fire_timers(loop);

    /* Run any deferred callbacks scheduled during this iteration */
    run_deferred(loop);

    return events_fired;
}

/* ── Run / Stop ────────────────────────────────────────────────────── */

void ev_loop_run(ev_loop_t *loop) {
    if (!loop)
        return;
    loop->stop_flag = 0;
    while (!loop->stop_flag) {
        ev_loop_poll(loop, -1);
    }
}

void ev_loop_stop(ev_loop_t *loop) {
    if (loop)
        loop->stop_flag = 1;
}

/* ── Stats ─────────────────────────────────────────────────────────── */

ev_stats_t ev_loop_stats(ev_loop_t *loop) {
    if (!loop) {
        ev_stats_t empty;
        memset(&empty, 0, sizeof(empty));
        return empty;
    }
    return loop->stats;
}
