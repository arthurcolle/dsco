#ifndef DSCO_POOL_H
#define DSCO_POOL_H

#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 *  Fan-out task pool.
 *
 *  macOS:  libdispatch (Grand Central Dispatch). dispatch_apply gives us
 *          work-stealing across all P-cores for free.
 *  other:  pthread worker pool with a bounded MPMC queue.
 *
 *  Two surfaces:
 *
 *   dsco_pool_apply(n, ctx, fn)      — parallel-for. fn called with i in [0,n).
 *                                       Blocks until all iterations finish.
 *
 *   dsco_pool_submit(fn, ctx)        — fire-and-forget submission. Returns a
 *                                       handle; dsco_pool_join(handle) waits.
 *
 *  Use cases in dsco:
 *    - parallel tool dispatch within a single LLM turn
 *    - parallel cosine-sim over independent candidate batches
 *    - parallel drain of multiple project rings
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct dsco_pool dsco_pool_t;
typedef struct dsco_task dsco_task_t;

typedef void (*dsco_pool_iter_fn)(size_t i, void *ctx);
typedef void (*dsco_pool_work_fn)(void *ctx);

/* Global pool used by apply()/submit() when no explicit pool is passed. */
int  dsco_pool_global_init(int nthreads);   /* 0 → use cpu_count */
void dsco_pool_global_shutdown(void);

/* Parallel-for. n iterations, divided across worker threads. */
void dsco_pool_apply(size_t n, void *ctx, dsco_pool_iter_fn fn);

/* Fire-and-forget. Returns a handle suitable for dsco_pool_join. */
dsco_task_t *dsco_pool_submit(dsco_pool_work_fn fn, void *ctx);
void         dsco_pool_join(dsco_task_t *t);

/* Number of worker threads in the global pool. */
int dsco_pool_worker_count(void);

#endif /* DSCO_POOL_H */
