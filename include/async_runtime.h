#ifndef DSCO_ASYNC_RUNTIME_H
#define DSCO_ASYNC_RUNTIME_H
#include "event_loop.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Completion cells + reserved mailbox + reverse dependency edges.
 * Handles are scoped to one runtime and encode slot and generation; zero is invalid. Results are copied,
 * contain no owned pointers, and remain available while a reference exists.
 * new/retain/release/read/publish/cancel are thread safe. A producer must own
 * a retained reference until it has finished all accesses to the runtime.
 * subscribe/unsubscribe/step/wait/free and reactor mutations are owner-only.
 * Callbacks run on that owner, outside locks, and may create/resolve/subscribe.
 * They must not recursively step/wait or destroy the runtime.
 * No implicit deadline, retry, tool dispatch or cancellation of side effects.
 */
typedef struct async_runtime async_runtime_t;
typedef uint64_t async_handle_t;
typedef uint64_t async_subscription_t;
typedef enum { ASYNC_SUCCESS = 1, ASYNC_FAILURE, ASYNC_CANCELLED } async_outcome_t;
typedef struct { async_outcome_t outcome; int code; uint64_t value; } async_result_t;
typedef void (*async_completion_fn)(async_runtime_t *, async_handle_t,
                                    const async_result_t *, void *);
typedef struct {
    size_t cells_live, edges_live, mailbox_depth, ready_depth;
    uint64_t publications, callbacks, recycled, wakes;
} async_stats_t;

/* Capacities bound concurrent retained operations/edges, not lifetime count.
 * Each admitted cell reserves its own completion-mailbox link. No allocation
 * occurs in publish, and accepted terminal results cannot overflow the mailbox.
 */
async_runtime_t *async_runtime_new(size_t cells, size_t edges);
/* EBUSY until all refs/subscriptions are released and mailbox is drained.
 * Producers must be stopped/joined before destruction. */
int async_runtime_free(async_runtime_t *);
ev_loop_t *async_runtime_loop(async_runtime_t *); /* borrowed; owner only */
async_stats_t async_runtime_stats(async_runtime_t *);
async_handle_t async_cell_new(async_runtime_t *); /* one owned ref; EAGAIN if full */
int async_retain(async_runtime_t *, async_handle_t);
int async_release(async_runtime_t *, async_handle_t);
/* 1 terminal (including queued publication), 0 pending, -1 invalid/stale. */
int async_read(async_runtime_t *, async_handle_t, async_result_t *);
/* Exactly one accepted terminal result. EALREADY for repeats, ESTALE for old
 * generations. Wake failure rejects publication before it is committed. */
int async_publish(async_runtime_t *, async_handle_t, async_result_t);
/* Sticky cooperative request, distinct from ASYNC_CANCELLED acknowledgement.
 * Requesting cancellation does not invent a terminal outcome. */
int async_request_cancel(async_runtime_t *, async_handle_t);
int async_cancel_requested(async_runtime_t *, async_handle_t);
/* Each edge pins its source until delivery/detachment. Late subscription uses
 * retained completion. Unsubscribe never cancels the operation. */
async_subscription_t async_subscribe(async_runtime_t *, async_handle_t,
                                     async_completion_fn, void *);
int async_unsubscribe(async_runtime_t *, async_subscription_t);
/* Drain entry snapshots; callbacks created by callbacks wait for next step.
 * Negative observation timeout blocks on the reactor, never a polling cadence.
 * Return delivered callback count, -1 error. wait returns 0 with result or -1;
 * it pins the cell and blocks for completion with no execution deadline. */
int async_step(async_runtime_t *, int observation_timeout_ms);
int async_wait(async_runtime_t *, async_handle_t, async_result_t *);
#endif
