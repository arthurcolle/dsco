#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#if !defined(EV_FORCE_POLL) && defined(__APPLE__)
#include <sys/event.h>
enum { EV_KQUEUE_ERROR = EV_ERROR };
#undef EV_ERROR /* Keep the public ev_mask_t value, not kqueue's flag. */
#define EV_USE_KQUEUE 1
#elif !defined(EV_FORCE_POLL) && defined(__linux__)
#include <sys/epoll.h>
#define EV_USE_EPOLL 1
#else
#include <poll.h>
#endif
#include "event_loop.h"
#include "event_wakeup.h"

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
    uintptr_t token;
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
#if EV_USE_KQUEUE || EV_USE_EPOLL
    int backend_fd;
#endif
    ev_wakeup_t wake;
    ev_fd_watcher_t fds[EV_MAX_FDS];
    uintptr_t next_token;

    ev_timer_t timers[EV_MAX_TIMERS];
    uint64_t timer_next_id;

    ev_deferred_t deferred[EV_MAX_DEFERRED];
    int defer_head;
    int defer_tail;
    int defer_count;

    int stop_flag;
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
    loop->backend_fd = kqueue();
#elif EV_USE_EPOLL
    loop->backend_fd = epoll_create1(EPOLL_CLOEXEC);
#endif
#if EV_USE_KQUEUE || EV_USE_EPOLL
    if (loop->backend_fd < 0) {
        int saved_errno = errno;
        free(loop);
        errno = saved_errno;
        return NULL;
    }
    if (fcntl(loop->backend_fd, F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno; close(loop->backend_fd); free(loop); errno = saved;
        return NULL;
    }
#endif
    if (ev_wakeup_init(&loop->wake,
#if EV_USE_KQUEUE || EV_USE_EPOLL
                       loop->backend_fd
#else
                       -1
#endif
                       ) < 0) {
        int saved = errno;
#if EV_USE_KQUEUE || EV_USE_EPOLL
        close(loop->backend_fd);
#endif
        free(loop); errno = saved; return NULL;
    }
#if EV_USE_EPOLL
    struct epoll_event wake_event = {.events = EPOLLIN}; /* token zero reserved */
    if (epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, loop->wake.read_fd, &wake_event) < 0) {
        int saved = errno; ev_wakeup_close(&loop->wake);
        close(loop->backend_fd); free(loop); errno = saved; return NULL;
    }
#endif

    for (int i = 0; i < EV_MAX_TIMERS; i++)
        loop->timers[i].active = false;

    return loop;
}

void ev_loop_free(ev_loop_t *loop) {
    if (!loop)
        return;
    ev_wakeup_close(&loop->wake);
#if EV_USE_KQUEUE || EV_USE_EPOLL
    close(loop->backend_fd);
#endif
    free(loop);
}

/* ── FD watching ───────────────────────────────────────────────────── */

#if EV_USE_KQUEUE
/* Each change is synchronous: registration failures must reach the caller. */
static int kqueue_mask(ev_loop_t *loop, int fd, ev_mask_t mask,
                       ev_mask_t previous, uintptr_t token) {
    const int filters[] = { EVFILT_READ, EVFILT_WRITE };
    const ev_mask_t bits[] = { EV_READ, EV_WRITE };
    for (int i = 0; i < 2; i++) {
        if (!((mask | previous) & bits[i]))
            continue;
        struct kevent change;
        int flags = (mask & bits[i]) ? EV_ADD | EV_ENABLE : EV_DELETE;
        EV_SET(&change, fd, filters[i], flags, 0, 0, (void *)token);
        if (kevent(loop->backend_fd, &change, 1, NULL, 0, NULL) < 0) {
            if (!(mask & bits[i]) && (errno == ENOENT || errno == EBADF))
                continue; /* Closing a watched descriptor removes its filters. */
            return -1;
        }
    }
    return 0;
}
#endif

int ev_watch_fd(ev_loop_t *loop, int fd, ev_mask_t mask, ev_fd_cb cb, void *ctx) {
    if (!loop || !cb || !(mask & (EV_READ | EV_WRITE)) ||
        (mask & ~(EV_READ | EV_WRITE | EV_ERROR))) {
        errno = EINVAL;
        return -1;
    }
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (fcntl(fd, F_GETFD) < 0)
        return -1;

    int idx = -1;
    for (int i = 0; i < EV_MAX_FDS; i++) {
        if (loop->fds[i].active && loop->fds[i].fd == fd) {
            idx = i;
            break;
        }
        if (!loop->fds[i].active && idx < 0)
            idx = i;
    }
    if (idx < 0) {
        errno = ENOSPC;
        return -1;
    }
    if (loop->next_token == UINTPTR_MAX) {
        errno = EOVERFLOW; /* Never recycle a generation token. */
        return -1;
    }
    uintptr_t token = ++loop->next_token;
    ev_fd_watcher_t *w = &loop->fds[idx];
#if EV_USE_KQUEUE
    ev_mask_t old_mask = w->active ? w->mask : 0;
    if (kqueue_mask(loop, fd, mask, old_mask, token) < 0) {
        int saved_errno = errno;
        /* Undo partially applied changes, preserving the previous registration. */
        if (kqueue_mask(loop, fd, old_mask, mask | old_mask, w->token) < 0) {
            (void)kqueue_mask(loop, fd, 0, mask | old_mask, 0);
            if (w->active) {
                w->active = false;
                loop->stats.active_fds--;
            }
        }
        errno = saved_errno;
        return -1;
    }
#elif EV_USE_EPOLL
    struct epoll_event event = {0};
    if (mask & EV_READ) event.events |= EPOLLIN;
    if (mask & EV_WRITE) event.events |= EPOLLOUT;
    event.events |= EPOLLRDHUP;
    event.data.u64 = token;
    int op = w->active ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(loop->backend_fd, op, fd, &event) < 0)
        return -1;
#endif
    if (!w->active)
        loop->stats.active_fds++;
    *w = (ev_fd_watcher_t){ .fd = fd, .mask = mask, .cb = cb, .ctx = ctx,
                            .active = true, .token = token };
    return 0;
}

int ev_unwatch_fd(ev_loop_t *loop, int fd) {
    if (!loop) {
        errno = EINVAL;
        return -1;
    }
    for (int i = 0; i < EV_MAX_FDS; i++) {
        ev_fd_watcher_t *w = &loop->fds[i];
        if (!w->active || w->fd != fd)
            continue;
        int result = 0;
#if EV_USE_KQUEUE
        result = kqueue_mask(loop, fd, 0, w->mask, 0);
#elif EV_USE_EPOLL
        result = epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, NULL);
        if (result < 0 && (errno == ENOENT || errno == EBADF))
            result = 0;
#endif
        /* Invalidate even on deletion failure: queued events cannot call back. */
        w->active = false;
        loop->stats.active_fds--;
        return result;
    }
    errno = ENOENT;
    return -1;
}

/* ── Timers ────────────────────────────────────────────────────────── */

static int alloc_timer(ev_loop_t *loop, int ms, int interval, ev_timer_cb cb, void *ctx) {
    if (!loop || !cb || ms < 0) {
        errno = EINVAL;
        return -1;
    }
    if (loop->timer_next_id > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    for (int i = 0; i < EV_MAX_TIMERS; i++) {
        if (!loop->timers[i].active) {
            loop->timers[i].id = (int)loop->timer_next_id++;
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
    if (interval_ms <= 0) {
        errno = EINVAL;
        return -1;
    }
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
    int count = loop->defer_count; /* Re-enqueued work waits for the next phase. */
    while (count-- > 0) {
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
    int due[EV_MAX_TIMERS];
    for (int i = 0; i < EV_MAX_TIMERS; i++) {
        ev_timer_t *tm = &loop->timers[i];
        due[i] = tm->active && t >= tm->deadline_ms ? tm->id : -1;
    }
    for (int i = 0; i < EV_MAX_TIMERS; i++) {
        ev_timer_t *tm = &loop->timers[i];
        if (due[i] < 0 || !tm->active || tm->id != due[i])
            continue;
        ev_timer_cb cb = tm->cb;
        void *ctx = tm->ctx;
        /* Commit before calling user code; never overwrite callback mutations. */
        if (tm->interval_ms > 0) {
            tm->deadline_ms = t + (uint64_t)tm->interval_ms;
        } else {
            tm->active = false;
            loop->stats.active_timers--;
        }
        loop->stats.timer_fires++;
        cb(due[i], ctx);
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
        int d = diff <= 0 ? 0 : diff > INT_MAX ? INT_MAX : (int)diff;
        if (min < 0 || d < min)
            min = d;
    }
    return min;
}

/* ── Poll ──────────────────────────────────────────────────────────── */

static int dispatch_fd(ev_loop_t *loop, uintptr_t token, ev_mask_t fired) {
    for (int i = 0; i < EV_MAX_FDS; i++) {
        ev_fd_watcher_t *w = &loop->fds[i];
        if (!w->active || w->token != token)
            continue;
        fired &= w->mask | EV_ERROR;
        if (!fired)
            return 0;
        w->cb(w->fd, fired, w->ctx);
        loop->stats.fd_events++;
        return 1;
    }
    return 0; /* Removed/replaced after this batch was collected. */
}

int ev_loop_poll(ev_loop_t *loop, int timeout_ms) {
    if (!loop) {
        errno = EINVAL;
        return -1;
    }

    bool had_deferred = loop->defer_count > 0;
    run_deferred(loop);
    int wait_ms = timeout_ms;
    wait_ms = next_timer_ms(loop, wait_ms);
    if (had_deferred || loop->defer_count || loop->stop_flag)
        wait_ms = 0;
    int events_fired = 0;
    int ret;

#if EV_USE_KQUEUE
    struct kevent events[64];
    struct timespec ts = {0, 0};
    if (wait_ms >= 0) {
        ts.tv_sec = wait_ms / 1000; ts.tv_nsec = (wait_ms % 1000) * 1000000L;
    }
    ret = kevent(loop->backend_fd, NULL, 0, events, 64, wait_ms < 0 ? NULL : &ts);
    for (int i = 0; i < ret; i++) {
        if (events[i].filter == EVFILT_USER) {
            loop->stats.wake_events++;
            continue;
        }
        ev_mask_t fired = 0;
        if (events[i].filter == EVFILT_READ) fired |= EV_READ;
        if (events[i].filter == EVFILT_WRITE) fired |= EV_WRITE;
        if (events[i].flags & (EV_KQUEUE_ERROR | EV_EOF)) fired |= EV_ERROR;
        events_fired += dispatch_fd(loop, (uintptr_t)events[i].udata, fired);
    }
#elif EV_USE_EPOLL
    struct epoll_event events[64];
    ret = epoll_wait(loop->backend_fd, events, 64, wait_ms);
    for (int i = 0; i < ret; i++) {
        if (events[i].data.u64 == 0) {
            ev_wakeup_drain(&loop->wake); loop->stats.wake_events++;
            continue;
        }
        ev_mask_t fired = 0;
        if (events[i].events & EPOLLIN) fired |= EV_READ;
        if (events[i].events & EPOLLOUT) fired |= EV_WRITE;
        if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) fired |= EV_ERROR;
        events_fired += dispatch_fd(loop, (uintptr_t)events[i].data.u64, fired);
    }
#else
    struct pollfd pfds[EV_MAX_FDS + 1];
    uintptr_t tokens[EV_MAX_FDS];
    int count = 0;
    for (int i = 0; i < EV_MAX_FDS; i++) {
        ev_fd_watcher_t *w = &loop->fds[i];
        if (!w->active)
            continue;
        pfds[count] = (struct pollfd){ .fd = w->fd };
        if (w->mask & EV_READ) pfds[count].events |= POLLIN;
        if (w->mask & EV_WRITE) pfds[count].events |= POLLOUT;
#ifdef POLLRDHUP
        pfds[count].events |= POLLRDHUP;
#endif
        tokens[count++] = w->token;
    }
    pfds[count] = (struct pollfd){ .fd = loop->wake.read_fd, .events = POLLIN };
    ret = poll(pfds, (nfds_t)count + 1, wait_ms);
    if (ret > 0) {
        if (pfds[count].revents) {
            ev_wakeup_drain(&loop->wake); loop->stats.wake_events++;
        }
        for (int i = 0; i < count; i++) {
            ev_mask_t fired = 0;
            if (pfds[i].revents & POLLIN) fired |= EV_READ;
            if (pfds[i].revents & POLLOUT) fired |= EV_WRITE;
            if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) fired |= EV_ERROR;
#ifdef POLLRDHUP
            if (pfds[i].revents & POLLRDHUP) fired |= EV_ERROR;
#endif
            if (fired)
                events_fired += dispatch_fd(loop, tokens[i], fired);
        }
    }
#endif
    loop->stats.polls++;
    if (ret < 0 && errno != EINTR)
        return -1;
    fire_timers(loop);
    run_deferred(loop);
    return events_fired;
}

const char *ev_loop_backend(const ev_loop_t *loop) {
    if (!loop) return NULL;
#if EV_USE_KQUEUE
    return "kqueue";
#elif EV_USE_EPOLL
    return "epoll";
#else
    return "poll";
#endif
}

int ev_loop_wake(ev_loop_t *loop) {
    if (!loop) { errno = EINVAL; return -1; }
    return ev_wakeup_signal(&loop->wake);
}

/* ── Run / Stop ────────────────────────────────────────────────────── */

void ev_loop_run(ev_loop_t *loop) {
    if (!loop)
        return;
    loop->stop_flag = 0;
    while (!loop->stop_flag) {
        if (ev_loop_poll(loop, -1) < 0)
            break;
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
