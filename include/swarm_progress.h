#ifndef DSCO_SWARM_PROGRESS_H
#define DSCO_SWARM_PROGRESS_H

#include "swarm.h"
#include <pthread.h>

/* One recursive ownership lease for the tool runtime's shared swarm. Interior
 * pointers must remain inside a lease. Independent local swarm_t instances are
 * not registered here. No thread, child, tool call or authority is created. */
typedef struct { bool held; } swarm_progress_guard_t;
swarm_progress_guard_t swarm_progress_acquire(void);
void swarm_progress_release(swarm_progress_guard_t *guard);
#define SWARM_PROGRESS_GUARD \
    swarm_progress_guard_t swarm_progress_guard __attribute__((cleanup(swarm_progress_release))) = swarm_progress_acquire()

/* Attach/detach under the lease; detach before destroying the swarm. */
void swarm_progress_attach(swarm_t *swarm);
void swarm_progress_detach(swarm_t *swarm);
/* Cooperative, nonblocking tick for provider and subprocess wait callbacks.
 * Returns zero when unregistered, busy, throttled or inherited after fork. */
int swarm_progress_tick(void);
/* Caller holds mutex; returns with mutex held. Recheck the completion predicate.
 * Tick runs with the completion mutex released, avoiding lock inversion. */
void swarm_progress_wait(pthread_cond_t *ready, pthread_mutex_t *mutex);

#endif
