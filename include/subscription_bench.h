#ifndef DSCO_SUBSCRIPTION_BENCH_H
#define DSCO_SUBSCRIPTION_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    const char *provider;
    const char *model;
    const char *label;
    const char *expected_auth_mode;
} dsco_subscription_lane_spec_t;

typedef struct {
    const char *prompt;
    const char *fallback_api_key;
    int rounds;
    int concurrency_per_lane;
    int max_tokens;
} dsco_subscription_bench_options_t;

size_t dsco_subscription_lane_count(void);
const dsco_subscription_lane_spec_t *dsco_subscription_lane_at(size_t index);

/* A tier-1 lane must have subscription-class credentials and an in-process
 * HTTP transport. External CLI fallbacks intentionally do not qualify. */
bool dsco_subscription_lane_native_ready(const dsco_subscription_lane_spec_t *lane,
                                         const char *fallback_api_key,
                                         char *auth_mode, size_t auth_mode_len,
                                         char *endpoint, size_t endpoint_len,
                                         char *reason, size_t reason_len);

int dsco_subscription_lanes_print_json(FILE *out, const char *fallback_api_key);
int dsco_subscription_bench_run(FILE *out,
                                const dsco_subscription_bench_options_t *options);

#endif
