#ifndef DSCO_EVENT_LOOP_H
#define DSCO_EVENT_LOOP_H

#include <stdbool.h>
#include <stdint.h>

/* Single-threaded, level-triggered loop. All operations, including stop, must
 * run on the owning thread, except ev_loop_wake(). Wake signals a notification,
 * not shared-data synchronization: publish work with a mutex/atomic mailbox.
 * Callbacks may watch/unwatch, schedule/cancel timers and defer work. They must
 * not recursively poll/run or free the loop. Unwatch before closing/reusing an
 * fd. Replacing a watch invalidates already-collected events for the old watch.
 * Define EV_FORCE_POLL when compiling event_loop.c to test the portable backend.
 */

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
const char *ev_loop_backend(const ev_loop_t *loop); /* kqueue, epoll, poll; NULL if NULL */

/* ── FD watching ───────────────────────────────────────────────────── */

/* Watch replaces the entire mask/callback/context. Requires READ and/or WRITE;
 * ERROR is delivered automatically on backend error/hangup/EOF. Returns 0 on
 * success, -1 with errno on failure (including invalid/unsupported descriptors).
 */
int  ev_watch_fd(ev_loop_t *loop, int fd, ev_mask_t mask, ev_fd_cb cb, void *ctx);
int  ev_unwatch_fd(ev_loop_t *loop, int fd);

/* ── Timers ────────────────────────────────────────────────────────── */

/* Delays must be nonnegative; repeating intervals must be positive. Newly
 * armed timers never fire in the same timer batch; cancel/rearm is callback-safe.
 */
int  ev_timer_once(ev_loop_t *loop, int ms, ev_timer_cb cb, void *ctx);
int  ev_timer_repeat(ev_loop_t *loop, int interval_ms, ev_timer_cb cb, void *ctx);
void ev_timer_cancel(ev_loop_t *loop, int timer_id);

/* ── Deferred callbacks ────────────────────────────────────────────── */

/* Each deferred phase runs only its entry snapshot, so self-requeue is bounded.
 * A poll entered with deferred work does not block after running that work.
 */
int  ev_defer(ev_loop_t *loop, ev_defer_cb cb, void *ctx);

/* Thread-safe notification only. The owner resumes its wait and drains its
 * mailbox; no callback runs on the producer thread. Notifications may coalesce.
 * Stop/join all producers before freeing the loop. Not an async-signal-safe API. */
int ev_loop_wake(ev_loop_t *loop);

/* ── Running ───────────────────────────────────────────────────────── */

/* Negative timeout blocks until an event or the next registered timer. */
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
    uint64_t wake_events;
} ev_stats_t;

ev_stats_t ev_loop_stats(ev_loop_t *loop);

#endif /* DSCO_EVENT_LOOP_H */
