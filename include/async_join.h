#ifndef DSCO_ASYNC_JOIN_H
#define DSCO_ASYNC_JOIN_H
#include "async_runtime.h"
typedef struct async_join async_join_t;
/* Reverse-edge success quorum: 1=first success, count=all, otherwise quorum.
 * Resolve once quorum is reached or mathematically impossible. value reports
 * successes observed at resolution; failure code is the first observed failure.
 * Duplicate input handles are rejected so one operation cannot vote twice.
 * Detaching losers releases observation only; it never cancels their work.
 * Output is borrowed from join; retain it to outlive async_join_free().
 * Owner-only. No further dependencies can be attached, so joins form a DAG.
 */
async_join_t *async_join_new(async_runtime_t *, const async_handle_t *, size_t count, size_t quorum);
async_handle_t async_join_output(const async_join_t *);
/* Detach edges, publish cancellation if still unresolved, release output ref.
 * Drain resulting mailbox before runtime destruction. */
void async_join_free(async_join_t *);
#endif
