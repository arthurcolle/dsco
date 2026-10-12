#ifndef DSCO_ASYNC_POOL_H
#define DSCO_ASYNC_POOL_H
#include "async_runtime.h"
#include "dsco_pool.h"
typedef async_result_t (*async_pool_fn)(async_runtime_t *, async_handle_t, void *);
/* Bridge the real GCD/pthread pool into retained reactor completions.
 * Submission is owner-only; admission reserves result capacity BEFORE dispatch.
 * Full runtime arena or portable worker queue returns EAGAIN, never waits for space.
 * Coordinate global pool init/shutdown with other pool users. The callback receives a
 * borrowed producer ref and can inspect cooperative cancellation. It must run
 * CPU/internal work; tool work still requires tools_execute_for_tier().
 * Caller owns *output ref AND task handle: release the former and join the
 * latter, including after unsubscribe/cancel. ctx must live until join returns.
 * This bridge neither starts the reactor nor joins a worker from its callback.
 * Publication status is retained in a caller-owned ticket; join exposes wake
 * failures rather than falsely claiming result delivery. */
typedef struct { dsco_task_t *task; async_handle_t output; void *private_work; } async_pool_ticket_t;
int async_pool_submit(async_runtime_t *, async_pool_fn, void *, async_pool_ticket_t *);
int async_pool_join(async_pool_ticket_t *);
#endif
