#ifndef DSCO_NATIVE_SESSION_DETAILS_H
#define DSCO_NATIVE_SESSION_DETAILS_H
#include <stddef.h>
/* Borrowed strings valid only for this synchronous formatting call. */
typedef struct {
    const char *model, *slot, *state, *configuration;
    double elapsed_s, context_percent, cost_usd, budget_usd;
    double reported_usd, estimated_usd;
    int turn, input_tokens, output_tokens, tools_used, running_tools;
    int queue_depth, queue_capacity, swarm_active, swarm_done, swarm_errors;
    int reported_samples, estimated_samples, unpriced;
} native_session_details_t;
void native_session_details_format(const native_session_details_t *s, char *out, size_t cap);
#endif
