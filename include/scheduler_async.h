#ifndef DSCO_SCHEDULER_ASYNC_H
#define DSCO_SCHEDULER_ASYNC_H
#include "scheduler.h"

/* Stackless cooperative continuations, not stackful coroutines.
 * Embed this zero-initialized handle in task ctx. All values needed after an
 * await belong in ctx, NOT automatic locals. One await per source line.
 * Do not put awaits inside another switch, or change ctx while suspended.
 * These macros are for task_func_t callbacks and return 1 when suspended.
 * Scheduling is single-threaded; blocking work still blocks the scheduler. */
typedef struct { unsigned int pc; } sched_async_t;
#define SCHED_ASYNC_BEGIN(a) switch ((a)->pc) { case 0:
#define SCHED_AWAIT_FD(a, s, fd, events) do { \
    (a)->pc = __LINE__; \
    sched_wait_fd_events((s), (fd), (events)); \
    return 1; case __LINE__:; \
} while (0)
#define SCHED_AWAIT_SLEEP(a, s, ms) do { \
    (a)->pc = __LINE__; \
    sched_sleep((s), (ms)); \
    return 1; case __LINE__:; \
} while (0)
#define SCHED_AWAIT_TASK(a, s, id) do { \
    (a)->pc = __LINE__; \
    sched_wait_task((s), (id)); \
    return 1; case __LINE__:; \
} while (0)
#define SCHED_ASYNC_END(a) } (a)->pc = 0; return 0
#endif
