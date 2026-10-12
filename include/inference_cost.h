#ifndef DSCO_INFERENCE_COST_H
#define DSCO_INFERENCE_COST_H
#include "llm.h"

typedef struct {
    bool provider_reported_known;
    bool estimated_known;
    bool budget_known;
    bool subscription_included;
    double provider_reported_usd;
    double estimated_usd;
    double budget_usd;
    double input_per_million;
    double output_per_million;
    double cache_read_per_million;
    double cache_write_per_million;
    const char *budget_basis;
    const char *pricing_source;
    const char *pricing_scope;
    long long pricing_observed_at;
} inference_cost_t;

/* Billing entitlement never removes usage or either cost observation. */
void inference_cost_measure(const char *model, const stream_result_t *response,
                            bool subscription_included, inference_cost_t *out);
void inference_cost_measure_for_provider(const char *provider, const char *model,
                            const stream_result_t *response, bool subscription_included,
                            inference_cost_t *out);
/* Durable per-attempt record, including failures and unknown prices. Updates
 * independent session accumulators. Returns false only on persistence/delivery
 * failure. Unknown pricing remains explicit and never fails a successful turn. */
bool inference_cost_record(session_state_t *session, const char *provider,
                           const char *request_key, const stream_result_t *response,
                           inference_cost_t *out);
#endif
