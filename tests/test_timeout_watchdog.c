#include "tools.h"
#include "cancel_wait.h"
#include "vm.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int g_cheap_mode;
vm_t g_vm;
volatile int g_interrupted;
double g_cost_budget;
static atomic_int ready;
static atomic_bool first_done;

static double now(void) {
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void *owner(void *arg) {
    bool short_call = arg != NULL;
    tool_watchdog_t wd;
    watchdog_start(&wd, pthread_self(), short_call ? "short_fixture" : "long_fixture", short_call ? 1 : 10);
    assert(wd.thread_started);
    atomic_fetch_add(&ready, 1);
    while (atomic_load(&ready) < 2) poll(NULL, 0, 1);
    double until = now() + 3;
    if (short_call) {
        cancel_wait_t *wait = cancel_wait_open(&tl_tool_cancelled, NULL); assert(wait);
        assert(cancel_wait_poll(wait, -1, 0, -1) == -1 && errno == ECANCELED);
        cancel_wait_close(wait);
        assert(__atomic_load_n(&tl_tool_cancelled, __ATOMIC_ACQUIRE));
        watchdog_stop(&wd); assert(wd.timed_out);
        assert(g_interrupted == 0);
        atomic_store(&first_done, true);
    } else {
        while (!atomic_load(&first_done) && now() < until) {
            assert(!__atomic_load_n(&tl_tool_cancelled, __ATOMIC_ACQUIRE));
            assert(g_interrupted == 0); poll(NULL, 0, 5);
        }
        assert(atomic_load(&first_done));
        assert(!__atomic_load_n(&tl_tool_cancelled, __ATOMIC_ACQUIRE));
        watchdog_stop(&wd); assert(!wd.timed_out);
    }
    return NULL;
}

int main(void) {
    setenv("DSCO_TOOL_GRACE_PERIOD_S", "0", 1);
    pthread_t a, b;
    assert(pthread_create(&a, NULL, owner, (void *)1) == 0);
    assert(pthread_create(&b, NULL, owner, NULL) == 0);
    pthread_join(a, NULL); pthread_join(b, NULL);
    assert(g_interrupted == 0);
    tool_watchdog_t wd;
    watchdog_start(&wd, pthread_self(), "renew_fixture", 1);
    assert(watchdog_renew(&wd, 10));
    poll(NULL, 0, 1200); watchdog_stop(&wd);
    assert(!wd.timed_out && !__atomic_load_n(&tl_tool_cancelled, __ATOMIC_ACQUIRE));
    puts("PASS: one timeout cancels only its owner; parallel work and deadline renewal survive");
    return 0;
}
