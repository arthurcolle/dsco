/* pthread_cond_timedwait_relative_np is a Darwin extension hidden by the
 * build's -D_POSIX_C_SOURCE; opt back in before any header pulls pthread.h. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "waiter.h"

#include <errno.h>
#include <time.h>

/* On Linux we pin the condvar to CLOCK_MONOTONIC so timed waits are immune
 * to wall-clock jumps. On macOS pthread_cond_timedwait_relative_np gives us
 * a relative (monotonic-behaved) wait directly. */

int dsco_waiter_init(dsco_waiter_t *w) {
    if (!w)
        return -1;
    w->signaled = 0;
    w->stopped = 0;
    if (pthread_mutex_init(&w->mu, NULL) != 0)
        return -1;
#if defined(__APPLE__)
    if (pthread_cond_init(&w->cv, NULL) != 0) {
        pthread_mutex_destroy(&w->mu);
        return -1;
    }
#else
    {
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
#if defined(CLOCK_MONOTONIC) && !defined(__COSMOPOLITAN__)
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
        int rc = pthread_cond_init(&w->cv, &attr);
        pthread_condattr_destroy(&attr);
        if (rc != 0) {
            pthread_mutex_destroy(&w->mu);
            return -1;
        }
    }
#endif
    return 0;
}

void dsco_waiter_destroy(dsco_waiter_t *w) {
    if (!w)
        return;
    /* Wake any stragglers before teardown. */
    pthread_mutex_lock(&w->mu);
    w->stopped = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
}

bool dsco_waiter_wait_ms(dsco_waiter_t *w, long timeout_ms) {
    if (!w)
        return false;
    pthread_mutex_lock(&w->mu);
    if (w->stopped || w->signaled) {
        bool early = true;
        w->signaled = 0;
        pthread_mutex_unlock(&w->mu);
        return early;
    }

    if (timeout_ms < 0) {
        while (!w->stopped && !w->signaled)
            pthread_cond_wait(&w->cv, &w->mu);
        w->signaled = 0;
        pthread_mutex_unlock(&w->mu);
        return true;
    }

#if defined(__APPLE__)
    struct timespec rel;
    rel.tv_sec = timeout_ms / 1000;
    rel.tv_nsec = (timeout_ms % 1000) * 1000000L;
    /* Loop to defend against spurious wakeups; recompute remaining time. */
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    long remaining_ms = timeout_ms;
    while (!w->stopped && !w->signaled && remaining_ms > 0) {
        rel.tv_sec = remaining_ms / 1000;
        rel.tv_nsec = (remaining_ms % 1000) * 1000000L;
        pthread_cond_timedwait_relative_np(&w->cv, &w->mu, &rel);
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (long)(now.tv_sec - start.tv_sec) * 1000L +
                          (long)(now.tv_nsec - start.tv_nsec) / 1000000L;
        remaining_ms = timeout_ms - elapsed_ms;
    }
#else
    struct timespec abs;
#if defined(CLOCK_MONOTONIC) && !defined(__COSMOPOLITAN__)
    clock_gettime(CLOCK_MONOTONIC, &abs);
#else
    clock_gettime(CLOCK_REALTIME, &abs);
#endif
    abs.tv_sec += timeout_ms / 1000;
    abs.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (abs.tv_nsec >= 1000000000L) {
        abs.tv_sec++;
        abs.tv_nsec -= 1000000000L;
    }
    while (!w->stopped && !w->signaled) {
        if (pthread_cond_timedwait(&w->cv, &w->mu, &abs) == ETIMEDOUT)
            break;
    }
#endif

    bool early = (w->stopped || w->signaled);
    w->signaled = 0;
    pthread_mutex_unlock(&w->mu);
    return early;
}

void dsco_waiter_signal(dsco_waiter_t *w) {
    if (!w)
        return;
    pthread_mutex_lock(&w->mu);
    w->signaled = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

void dsco_waiter_stop(dsco_waiter_t *w) {
    if (!w)
        return;
    pthread_mutex_lock(&w->mu);
    w->stopped = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

void dsco_waiter_reset(dsco_waiter_t *w) {
    if (!w)
        return;
    pthread_mutex_lock(&w->mu);
    w->stopped = 0;
    w->signaled = 0;
    pthread_mutex_unlock(&w->mu);
}

bool dsco_waiter_stopped(dsco_waiter_t *w) {
    if (!w)
        return true;
    pthread_mutex_lock(&w->mu);
    bool s = w->stopped != 0;
    pthread_mutex_unlock(&w->mu);
    return s;
}
