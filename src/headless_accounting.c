#include "headless_accounting.h"
#include "inference_cost.h"
#include "chronicle.h"
#include <stdio.h>

bool headless_account_response(session_state_t *s, const char *provider,
                               const char *key, const stream_result_t *r) {
    inference_cost_t cost;
    if (!inference_cost_record(s,provider,key,r,&cost)) {
        fprintf(stderr,"error: headless accounting unavailable; stopping work\n");
        return false;
    }
    if (cost.budget_known)
        chronicle_llm_response(NULL,NULL,provider,s->model,"",NULL,
                               r->usage.input_tokens,r->usage.output_tokens,
                               r->usage.cache_read_input_tokens,r->usage.cache_creation_input_tokens,
                               r->reasoning_tokens,cost.budget_usd,r->telemetry.total_ms,
                               r->parsed.stop_reason,r->generation_id);
    if (!cost.budget_known) {
        fprintf(stderr,"  usage: input=%d output=%d cost=unknown total_known=$%.8f\n",
                r->usage.input_tokens,r->usage.output_tokens,s->total_reported_cost_usd);
        return true;
    }
    fprintf(stderr,"  usage: input=%d output=%d cost=$%.8f total=$%.8f basis=%s%s\n",
            r->usage.input_tokens,r->usage.output_tokens,cost.budget_usd,
            s->total_reported_cost_usd,cost.budget_basis,
            cost.subscription_included?" billing=subscription (inference retained)":"");
    return true;
}

void headless_session_retarget(session_state_t *s, const char *model) {
    if (!s || !model) return;
    session_state_t previous = *s;
    session_state_init(s, model);
    s->total_input_tokens = previous.total_input_tokens;
    s->total_output_tokens = previous.total_output_tokens;
    s->total_cache_read_tokens = previous.total_cache_read_tokens;
    s->total_cache_write_tokens = previous.total_cache_write_tokens;
    s->total_reported_cost_usd = previous.total_reported_cost_usd;
    s->turn_count = previous.turn_count;
    s->total_provider_reported_cost_usd = previous.total_provider_reported_cost_usd;
    s->total_estimated_inference_cost_usd = previous.total_estimated_inference_cost_usd;
    s->provider_cost_samples = previous.provider_cost_samples;
    s->estimated_cost_samples = previous.estimated_cost_samples;
    s->unpriced_response_count = previous.unpriced_response_count;
    s->subscription_response_count = previous.subscription_response_count;
    s->total_reasoning_tokens = previous.total_reasoning_tokens;
}
