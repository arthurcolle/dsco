#ifndef DSCO_JEV_ROUTER_H
#define DSCO_JEV_ROUTER_H
#include <stdbool.h>
#include <stddef.h>
#define JEV_ROUTER_MODEL "jev-1.13.0"
#define JEV_ROUTER_MAX_CANDIDATES 6
#define JEV_ROUTER_TASK_MAX 2048
#define JEV_ROUTER_RECEIPT_MAX 1024
/* Caller supplies only locally eligible candidates. No credential or message
 * history enters the payload. This client can recommend; it cannot dispatch. */
typedef struct {
    char model[128];
    int tier;
    double input_price;
    double output_price;
    double latency_ms;
} jev_router_candidate_t;
/* selected=-1 means explicit stay/abstain. Always writes a bounded receipt.
 * At most 32 attempts/process, 2s each, no retry. Existing tool gates apply. */
bool jev_router_pick(const char *task, const char *current_model,
                     const jev_router_candidate_t *candidates, size_t count,
                     int *selected, char *receipt, size_t receipt_len, const char *tier);
#endif
