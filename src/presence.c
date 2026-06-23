#define _DARWIN_C_SOURCE 1
#include "presence.h"
#include "audit_log.h"
#include "sealed_store.h" /* Cross-pollination: wipe secrets on idle lock */

#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

#ifdef __APPLE__
#include <sys/event.h>
#include <sys/time.h>
#include <CoreGraphics/CGEventSource.h>

/* ── module state ────────────────────────────────────────────────────────── */

#define POLL_INTERVAL_S 5  /* kqueue timer fires every N seconds */
#define DEFAULT_IDLE_S 300 /* 5-minute default lock threshold */

static int s_idle_threshold_s = DEFAULT_IDLE_S;
static presence_lock_fn s_lock_cb = NULL;
static void *s_lock_ctx = NULL;
static pthread_t s_thread;
static atomic_int s_running = 0;
static atomic_int s_locked = 0;
static atomic_int s_kq = -1; /* kqueue fd, for stop() */

/* ── kqueue presence thread ──────────────────────────────────────────────── */

static void *presence_thread(void *arg) {
    (void)arg;

    int kq = kqueue();
    if (kq < 0) {
        audit_log("presence", "kqueue() failed");
        return NULL;
    }
    atomic_store(&s_kq, kq);

    /* EVFILT_TIMER: fires every POLL_INTERVAL_S seconds (NOTE_SECONDS unit) */
    struct kevent ev;
    EV_SET(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE, NOTE_SECONDS, POLL_INTERVAL_S, NULL);
    kevent(kq, &ev, 1, NULL, 0, NULL);

    /* EVFILT_USER: allows presence_stop() to wake and exit the thread */
    EV_SET(&ev, 2, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    kevent(kq, &ev, 1, NULL, 0, NULL);

    struct kevent triggered;
    while (atomic_load(&s_running)) {
        int n = kevent(kq, NULL, 0, &triggered, 1, NULL);
        if (n <= 0)
            break;

        /* EVFILT_USER wake → time to exit */
        if (triggered.filter == EVFILT_USER)
            break;

        if (!atomic_load(&s_running))
            break;
        if (atomic_load(&s_locked))
            continue; /* already locked; skip */

        double idle = presence_idle_seconds();
        if (idle >= (double)s_idle_threshold_s) {
            atomic_store(&s_locked, 1);
            audit_log("presence", "idle threshold reached — locking terminal");
            /* Cross-pollination: wipe in-memory secrets when the terminal
             * auto-locks. Reduces exposure window if the user walks away
             * with secrets still in sealed_store. Secrets are re-loaded
             * from env on next tool call that needs them. */
            sealed_store_wipe_all();
            audit_log("presence", "sealed_store wiped on idle lock");
            if (s_lock_cb)
                s_lock_cb(s_lock_ctx);
        }
    }

    close(kq);
    atomic_store(&s_kq, -1);
    return NULL;
}

/* ── public API ──────────────────────────────────────────────────────────── */

void presence_init(int idle_threshold_s, presence_lock_fn cb, void *ctx) {
    s_idle_threshold_s = idle_threshold_s > 0 ? idle_threshold_s : DEFAULT_IDLE_S;
    s_lock_cb = cb;
    s_lock_ctx = ctx;
}

void presence_start(void) {
    if (atomic_exchange(&s_running, 1))
        return; /* already running */
    pthread_create(&s_thread, NULL, presence_thread, NULL);
}

void presence_stop(void) {
    if (!atomic_exchange(&s_running, 0))
        return;

    /* Wake the kqueue thread via EVFILT_USER trigger */
    int kq = atomic_load(&s_kq);
    if (kq >= 0) {
        struct kevent wake;
        EV_SET(&wake, 2, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
        kevent(kq, &wake, 1, NULL, 0, NULL);
    }
    pthread_join(s_thread, NULL);
}

double presence_idle_seconds(void) {
    return CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState,
                                                  kCGAnyInputEventType);
}

bool presence_is_locked(void) {
    return atomic_load(&s_locked) != 0;
}

void presence_mark_unlocked(void) {
    atomic_store(&s_locked, 0);
    audit_log("presence", "terminal unlocked");
}

void presence_poke(void) {
    /* Reset the software idle counter to < threshold so we don't re-lock
     * immediately on the next timer tick after a user keystroke. */
    atomic_store(&s_locked, 0);
}

#else /* !__APPLE__ — stubs */

void presence_init(int s, presence_lock_fn cb, void *ctx) {
    (void)s;
    (void)cb;
    (void)ctx;
}
void presence_start(void) {}
void presence_stop(void) {}
double presence_idle_seconds(void) {
    return 0.0;
}
bool presence_is_locked(void) {
    return false;
}
void presence_mark_unlocked(void) {}
void presence_poke(void) {}

#endif /* __APPLE__ */
