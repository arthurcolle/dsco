#ifndef DSCO_EVENT_LOOP_H
#define DSCO_EVENT_LOOP_H

#include <stdbool.h>
#include <stdint.h>

/* ── Event types ───────────────────────────────────────────────────── */

typedef enum {
    EV_READ   = 1,
    EV_WRITE  = 2,
    EV_ERROR  = 4,
    EV_TIMER  = 8,
    EV_SIGNAL = 16,
} ev_mask_t;

/* ── Callbacks ─────────────────────────────────────────────────────── */

typedef void (*ev_fd_cb)(int fd, ev_mask_t events, void *ctx);
typedef void (*ev_timer_cb)(int timer_id, void *ctx);
typedef void (*ev_defer_cb)(void *ctx);

/* ── Event loop handle ─────────────────────────────────────────────── */

typedef struct ev_loop ev_loop_t;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

ev_loop_t *ev_loop_new(void);
void       ev_loop_free(ev_loop_t *loop);

/* ── FD watching ───────────────────────────────────────────────────── */

int  ev_watch_fd(ev_loop_t *loop, int fd, ev_mask_t mask, ev_fd_cb cb, void *ctx);
int  ev_unwatch_fd(ev_loop_t *loop, int fd);

/* ── Timers ────────────────────────────────────────────────────────── */

int  ev_timer_once(ev_loop_t *loop, int ms, ev_timer_cb cb, void *ctx);
int  ev_timer_repeat(ev_loop_t *loop, int interval_ms, ev_timer_cb cb, void *ctx);
void ev_timer_cancel(ev_loop_t *loop, int timer_id);

/* ── Deferred callbacks ────────────────────────────────────────────── */

int  ev_defer(ev_loop_t *loop, ev_defer_cb cb, void *ctx);

/* ── Running ───────────────────────────────────────────────────────── */

int  ev_loop_poll(ev_loop_t *loop, int timeout_ms);
void ev_loop_run(ev_loop_t *loop);
void ev_loop_stop(ev_loop_t *loop);

/* ── Stats ─────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t polls;
    uint64_t fd_events;
    uint64_t timer_fires;
    uint64_t deferred_runs;
    int      active_fds;
    int      active_timers;
} ev_stats_t;

ev_stats_t ev_loop_stats(ev_loop_t *loop);

#endif /* DSCO_EVENT_LOOP_H */
