#ifndef DSCO_WAITER_H
#define DSCO_WAITER_H

/* ── Interruptible waiter ────────────────────────────────────────────────
 * Replaces sleep()/usleep() polling loops in background threads with a
 * condvar-based wait that can be interrupted instantly.
 *
 * Design goals (edge deployment):
 *   - Zero wakeups while idle (no periodic polling).
 *   - Instant shutdown: dsco_waiter_stop() wakes all waiters immediately,
 *     so threads join in microseconds instead of up to interval seconds.
 *   - Monotonic clock: immune to wall-clock jumps (NTP, suspend/resume).
 *
 * Usage pattern for a background loop:
 *
 *     while (!dsco_waiter_stopped(&w)) {
 *         do_work();
 *         dsco_waiter_wait_ms(&w, interval_ms);   // early-wakes on signal/stop
 *     }
 *
 * Shutdown from another thread:
 *
 *     dsco_waiter_stop(&w);
 *     pthread_join(thread, NULL);
 */

#include <pthread.h>
#include <stdbool.h>

typedef struct dsco_waiter {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int signaled; /* consumable one-shot wake */
    int stopped;  /* sticky: all waits return immediately */
} dsco_waiter_t;

/* Initialize. Returns 0 on success. Uses CLOCK_MONOTONIC where supported. */
int dsco_waiter_init(dsco_waiter_t *w);
void dsco_waiter_destroy(dsco_waiter_t *w);

/* Sleep up to timeout_ms (<0 = wait forever until signal/stop).
 * Returns true if woken early (signal or stop), false on timeout. */
bool dsco_waiter_wait_ms(dsco_waiter_t *w, long timeout_ms);

/* Wake one pending/next wait (one-shot, consumable). */
void dsco_waiter_signal(dsco_waiter_t *w);

/* Permanent wake-all: every current and future wait returns immediately. */
void dsco_waiter_stop(dsco_waiter_t *w);

/* Re-arm after stop (for restartable subsystems). */
void dsco_waiter_reset(dsco_waiter_t *w);

bool dsco_waiter_stopped(dsco_waiter_t *w);

#endif /* DSCO_WAITER_H */
