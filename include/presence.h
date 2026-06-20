#ifndef DSCO_PRESENCE_H
#define DSCO_PRESENCE_H

/* ── Presence / Auto-lock ──────────────────────────────────────────────────
 *
 * Detects user idle time via CoreGraphics HID event timestamps and fires a
 * callback when the terminal should auto-lock.  Uses a kqueue EVFILT_TIMER
 * instead of a busy-polling thread — one wakeup per poll_interval, zero CPU
 * when the user is active.
 *
 * Usage:
 *   presence_init(300, lock_fn, ctx);   // 5-minute idle threshold
 *   presence_start();                   // launch background kqueue thread
 *   ...
 *   presence_stop();                    // join thread on shutdown
 * ────────────────────────────────────────────────────────────────────────── */

#include <stdbool.h>

typedef void (*presence_lock_fn)(void *ctx);

/* Configure idle threshold and lock callback.  Must be called before start. */
void   presence_init(int idle_threshold_s, presence_lock_fn cb, void *ctx);

/* Start/stop the background kqueue thread. */
void   presence_start(void);
void   presence_stop(void);

/* Seconds since last keyboard or mouse event (wraps CoreGraphics HID API). */
double presence_idle_seconds(void);

/* True if lock callback has fired and unlock hasn't been confirmed. */
bool   presence_is_locked(void);

/* Called by the Touch ID success path to clear the locked flag. */
void   presence_mark_unlocked(void);

/* Reset the idle timer (call on any user keystroke to suppress false locks). */
void   presence_poke(void);

#endif /* DSCO_PRESENCE_H */
