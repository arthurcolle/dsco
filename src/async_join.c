#include "async_join.h"
#include <errno.h>
#include <stdlib.h>
typedef struct { struct async_join *join; async_subscription_t subscription; } join_edge_t;
struct async_join {
    async_runtime_t *runtime;
    async_handle_t output;
    size_t count, quorum, remaining, successes;
    int first_error;
    bool settled;
    join_edge_t edges[];
};
static void detach(async_join_t *j) {
    for (size_t i = 0; i < j->count; ++i) {
        if (j->edges[i].subscription) {
            async_unsubscribe(j->runtime,j->edges[i].subscription);
            j->edges[i].subscription = 0;
        }
    }
}
static void arrive(async_runtime_t *r, async_handle_t h, const async_result_t *result, void *ctx) {
    (void)h; join_edge_t *edge = ctx; async_join_t *j = edge->join;
    edge->subscription = 0; --j->remaining;
    if (result->outcome == ASYNC_SUCCESS) ++j->successes;
    else if (!j->first_error) j->first_error = result->code ? result->code : ECANCELED;
    if (j->successes >= j->quorum || j->successes + j->remaining < j->quorum) {
        async_result_t value = {.outcome = j->successes >= j->quorum ? ASYNC_SUCCESS : ASYNC_FAILURE,
            .code = j->successes >= j->quorum ? 0 : j->first_error, .value = j->successes};
        /* A caller must not publish into the join-owned output. */
        if (async_publish(r,j->output,value) == 0) { j->settled = true; detach(j); }
    }
}
async_join_t *async_join_new(async_runtime_t *r, const async_handle_t *inputs, size_t count, size_t quorum) {
    if (!r || !inputs || !count || !quorum || quorum > count ||
        count > (SIZE_MAX - sizeof(async_join_t)) / sizeof(join_edge_t)) {
        errno = EINVAL; return NULL;
    }
    /* Verify ownership even if no subscription can be allocated. */
    if (!async_runtime_loop(r)) return NULL;
    for (size_t i = 0; i < count; ++i) {
        async_result_t ignored;
        if (async_read(r,inputs[i],&ignored) < 0) return NULL;
        for (size_t k = 0; k < i; ++k) if (inputs[k] == inputs[i]) { errno = EINVAL; return NULL; }
    }
    async_join_t *j = calloc(1,sizeof(*j) + count * sizeof(join_edge_t)); if (!j) return NULL;
    j->runtime = r; j->count = j->remaining = count; j->quorum = quorum;
    j->output = async_cell_new(r); if (!j->output) { free(j); return NULL; }
    for (size_t i = 0; i < count; ++i) {
        j->edges[i].join = j;
        j->edges[i].subscription = async_subscribe(r,inputs[i],arrive,&j->edges[i]);
        if (!j->edges[i].subscription) {
            int saved = errno; detach(j); async_release(r,j->output); free(j); errno = saved; return NULL;
        }
    }
    return j;
}
async_handle_t async_join_output(const async_join_t *j) { return j ? j->output : 0; }
void async_join_free(async_join_t *j) {
    if (!j) return;
    if (!async_runtime_loop(j->runtime)) return;
    detach(j);
    if (!j->settled) {
        /* Queued completion retains the cell even when its last reference goes. */
        async_publish(j->runtime,j->output,(async_result_t){ASYNC_CANCELLED,ECANCELED,0});
    }
    async_release(j->runtime,j->output); free(j);
}
