#include "swarm_progress.h"

#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t mutex;
static swarm_t *attached;
static _Atomic pid_t owner_pid;
static double last_tick;
static _Thread_local bool ticking;

static void init_mutex(void) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) ||
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) ||
        pthread_mutex_init(&mutex, &attr)) abort();
    pthread_mutexattr_destroy(&attr);
}

swarm_progress_guard_t swarm_progress_acquire(void) {
    pthread_once(&once, init_mutex);
    if (pthread_mutex_lock(&mutex)) abort();
    return (swarm_progress_guard_t){true};
}

void swarm_progress_release(swarm_progress_guard_t *guard) {
    if (guard && guard->held) {
        guard->held = false;
        if (pthread_mutex_unlock(&mutex)) abort();
    }
}

void swarm_progress_attach(swarm_t *swarm) {
    SWARM_PROGRESS_GUARD;
    /* A live runtime has exactly one shared instance. Do not steal ownership. */
    if (attached && attached != swarm) abort();
    attached = swarm;
    last_tick = 0;
    atomic_store(&owner_pid, swarm ? getpid() : 0);
}

void swarm_progress_detach(swarm_t *swarm) {
    SWARM_PROGRESS_GUARD;
    if (attached == swarm) {
        attached = NULL;
        atomic_store(&owner_pid, 0);
        last_tick = 0;
    }
}

int swarm_progress_tick(void) {
    int saved_errno = errno;
    /* Check before touching an inherited mutex: only the parent owns these
     * pipes and PIDs. Recursive provider/callback ticks are also no-ops. */
    if (ticking || atomic_load(&owner_pid) != getpid()) return 0;
    pthread_once(&once, init_mutex);
    if (pthread_mutex_trylock(&mutex)) return 0;
    int events = 0;
    struct timespec ts;
    if (attached && attached->active.count > 0 && clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        double now = ts.tv_sec + ts.tv_nsec / 1e9;
        if (last_tick == 0 || now - last_tick >= 0.050) {
            last_tick = now;
            ticking = true;
            events = swarm_poll(attached, 0);
            ticking = false;
        }
    }
    pthread_mutex_unlock(&mutex);
    errno = saved_errno; /* callers may be inspecting poll()/EINTR */
    return events;
}

void swarm_progress_wait(pthread_cond_t *ready, pthread_mutex_t *completion_mutex) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        pthread_cond_wait(ready, completion_mutex);
        return;
    }
    deadline.tv_nsec += 100 * 1000 * 1000;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(ready, completion_mutex, &deadline);
    pthread_mutex_unlock(completion_mutex);
    (void)swarm_progress_tick();
    pthread_mutex_lock(completion_mutex);
}
