/* test_waiter.c — dsco_waiter_t contract tests.
 *
 * Verifies the interruptible-wait primitive that replaced sleep-poll loops:
 *   1. timeout path: wait_ms returns false (no early wake) near the deadline
 *   2. signal path: a signal from another thread wakes the waiter fast
 *   3. stop path: stop() wakes instantly and is sticky
 *   4. reset path: re-arms after stop
 */
#include "waiter.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *signaler(void *arg) {
    dsco_waiter_t *w = (dsco_waiter_t *)arg;
    usleep(50 * 1000); /* 50ms */
    dsco_waiter_signal(w);
    return NULL;
}

static void *stopper(void *arg) {
    dsco_waiter_t *w = (dsco_waiter_t *)arg;
    usleep(50 * 1000);
    dsco_waiter_stop(w);
    return NULL;
}

int main(void) {
    dsco_waiter_t w;
    assert(dsco_waiter_init(&w) == 0);

    /* 1. Timeout: 100ms wait with no signal returns false, ~on time. */
    {
        double t0 = now_sec();
        bool early = dsco_waiter_wait_ms(&w, 100);
        double dt = now_sec() - t0;
        assert(!early);
        assert(dt >= 0.08 && dt < 2.0); /* generous upper bound for CI */
        printf("ok 1 timeout (%.0fms)\n", dt * 1000);
    }

    /* 2. Signal wakes early. */
    {
        pthread_t th;
        pthread_create(&th, NULL, signaler, &w);
        double t0 = now_sec();
        bool early = dsco_waiter_wait_ms(&w, 5000);
        double dt = now_sec() - t0;
        pthread_join(th, NULL);
        assert(early);
        assert(dt < 2.0); /* woke long before the 5s timeout */
        printf("ok 2 signal wake (%.0fms)\n", dt * 1000);
    }

    /* 3. Stop wakes instantly and is sticky. */
    {
        pthread_t th;
        pthread_create(&th, NULL, stopper, &w);
        double t0 = now_sec();
        bool early = dsco_waiter_wait_ms(&w, 10000);
        double dt = now_sec() - t0;
        pthread_join(th, NULL);
        assert(early);
        assert(dt < 2.0);
        assert(dsco_waiter_stopped(&w));
        /* Sticky: subsequent waits return immediately. */
        t0 = now_sec();
        early = dsco_waiter_wait_ms(&w, 10000);
        dt = now_sec() - t0;
        assert(early);
        assert(dt < 0.5);
        printf("ok 3 stop sticky (%.0fms)\n", dt * 1000);
    }

    /* 4. Reset re-arms. */
    {
        dsco_waiter_reset(&w);
        assert(!dsco_waiter_stopped(&w));
        double t0 = now_sec();
        bool early = dsco_waiter_wait_ms(&w, 60);
        double dt = now_sec() - t0;
        assert(!early);
        assert(dt >= 0.04);
        printf("ok 4 reset (%.0fms)\n", dt * 1000);
    }

    dsco_waiter_destroy(&w);
    printf("all waiter tests passed\n");
    return 0;
}
