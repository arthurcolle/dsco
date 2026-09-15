#ifndef DSCO_COST_FRONTIER_H
#define DSCO_COST_FRONTIER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
/* Pure, opt-in selection among caller-authorized lanes. Quotes must already
 * select the applicable route, UTC band and context tier. No network, alias
 * resolution, credential discovery, or inference is performed here. */
typedef struct {
    const char *provider, *model, *source;
    time_t observed_at;
    bool route_specific, tools;
    int64_t context_tokens, max_output_tokens;
    double input_per_million, cached_per_million, output_per_million, request_fee;
    int64_t cached_tokens; /* Route-scoped expected hits, never transferred across lanes. */
    unsigned attempts, verified;
    double mean_latency_seconds;
} cost_frontier_candidate_t;
typedef struct {
    bool enabled;
    int64_t prompt_tokens, output_tokens;
    bool require_tools;
    time_t at;
    unsigned max_quote_age_seconds, min_attempts;
    double minimum_success_lower_bound;
    double max_latency_seconds;
} cost_frontier_scenario_t;
typedef struct {
    bool eligible, pareto, billed_free;
    const char *reason;
    double cost_per_call, cost_per_verified, seconds_per_verified;
    double success_lower_bound, success_upper_bound;
} cost_frontier_result_t;
/* Returns eligible count. Results retain input order; order[] receives eligible
 * indices, frontier first, then cost/verified, seconds/verified, original index.
 * No reciprocal cost/runtime projection is calculated for free billed quotes.
 * Disabled mode produces no selection and cannot alter existing routing. */
size_t cost_frontier_rank(const cost_frontier_candidate_t *candidates, size_t count,
                          const cost_frontier_scenario_t *scenario,
                          cost_frontier_result_t *results, size_t *order);
/* Native adapter passes facts about its canonical transport, not a price
 * quote's declared billing product. Ambiguous Anthropic auth cannot be pinned. */
bool cost_frontier_auth_compatible(const char *provider, const char *auth_class,
                                   bool local, bool api_key_transport,
                                   bool subscription_endpoint);
#define COST_FRONTIER_MAX_LANES 64
typedef struct {
    char provider[64], model[128], effort[16], source[512];
    char upstream[64], quantization[16], auth_class[16];
    cost_frontier_candidate_t quote;
    cost_frontier_result_t result;
    time_t expires_at;
    char audit_json[32768];
} cost_frontier_selection_t;
/* Validate bounded caller-supplied policy and return its best qualified lane.
 * Whole input includes matching workload/validator and frontier_policy object.
 * Rejections return a constant diagnostic in error; no fallback is performed. */
bool cost_frontier_select_json(const char *input, time_t at,
                               cost_frontier_selection_t *selected,
                               const char **error);
#endif
