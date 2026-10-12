#ifndef DSCO_SCHEDULER_LIFETIME_H
#define DSCO_SCHEDULER_LIFETIME_H
#include "scheduler.h"
/* Internal owner-side collection; never called during a task callback. */
void sched_lifetime_collect(scheduler_t *s);
#endif
