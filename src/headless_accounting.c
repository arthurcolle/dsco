#include "headless_accounting.h"
#include "inference_cost.h"
#include "chronicle.h"
#include "json_util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool receipt_enabled(void) {
    const char *value = getenv("DSCO_HEADLESS_RECEIPT");
    return value && value[0] && strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 && strcmp(value, "off") != 0;
}

static void append_nullable_number(jbuf_t *json, const char *name, bool known, double value) {
    jbuf_append(json, ",\"");
    jbuf_append(json, name);
    jbuf_append(json, "\":");
    if (known && isfinite(value) && value >= 0.0)
        jbuf_appendf(json, "%.10f", value);
    else
        jbuf_append(json, "null");
}

static void emit_headless_receipt(const session_state_t *session, const char *provider,
                                  const stream_result_t *response,
                                  const inference_cost_t *cost) {
    if (!receipt_enabled() || !session || !response || !cost)
        return;
    jbuf_t json;
    jbuf_init(&json, 1024);
    jbuf_append(&json, "{\"schema\":\"dsco.headless.response.v1\",\"provider\":");
    jbuf_append_json_str(&json, provider ? provider : "");
    jbuf_append(&json, ",\"requested_model\":");
    jbuf_append_json_str(&json, session->model);
    jbuf_append(&json, ",\"actual_model\":");
    jbuf_append_json_str(&json, response->actual_model ? response->actual_model : session->model);
    jbuf_append(&json, ",\"reasoning_effort\":");
    jbuf_append_json_str(&json, getenv("DSCO_EFFORT") ? getenv("DSCO_EFFORT") : "auto");
    jbuf_append(&json, ",\"generation_id\":");
    jbuf_append_json_str(&json, response->generation_id ? response->generation_id : "");
    jbuf_appendf(&json,
                 ",\"success\":%s,\"http_status\":%d,"
                 "\"input_tokens\":%d,\"output_tokens\":%d,"
                 "\"cache_read_tokens\":%d,\"cache_write_tokens\":%d,"
                 "\"reasoning_tokens\":%d,\"subscription_included\":%s",
                 response->ok ? "true" : "false", response->http_status,
                 response->usage.input_tokens, response->usage.output_tokens,
                 response->usage.cache_read_input_tokens,
                 response->usage.cache_creation_input_tokens, response->reasoning_tokens,
                 cost->subscription_included ? "true" : "false");
    append_nullable_number(&json, "latency_ms", true, response->telemetry.total_ms);
    append_nullable_number(&json, "ttft_ms", true, response->telemetry.ttft_ms);
    append_nullable_number(&json, "ttft_tool_ms", true, response->telemetry.ttft_tool_ms);
    append_nullable_number(&json, "tokens_per_second", true,
                           response->telemetry.tokens_per_sec);
    append_nullable_number(&json, "dns_ms", true, response->telemetry.latency.dns_ms);
    append_nullable_number(&json, "connect_ms", true, response->telemetry.latency.connect_ms);
    append_nullable_number(&json, "tls_ms", true, response->telemetry.latency.tls_ms);
    append_nullable_number(&json, "ttfb_ms", true, response->telemetry.latency.ttfb_ms);
    jbuf_appendf(&json, ",\"new_connections\":%ld",
                 response->telemetry.latency.new_connections);
    append_nullable_number(&json, "provider_reported_usd", cost->provider_reported_known,
                           cost->provider_reported_usd);
    append_nullable_number(&json, "estimated_inference_usd", cost->estimated_known,
                           cost->estimated_usd);
    append_nullable_number(&json, "budget_accounted_usd", cost->budget_known,
                           cost->budget_usd);
    jbuf_append(&json, ",\"budget_basis\":");
    jbuf_append_json_str(&json, cost->budget_basis ? cost->budget_basis : "unknown");
    jbuf_append(&json, ",\"pricing_source\":");
    jbuf_append_json_str(&json, cost->pricing_source ? cost->pricing_source : "unknown");
    jbuf_append(&json, ",\"pricing_scope\":");
    jbuf_append_json_str(&json, cost->pricing_scope ? cost->pricing_scope : "unknown");
    jbuf_append(&json, "}");
    fprintf(stderr, "\nDSCO_HEADLESS_RECEIPT %s\n", json.data ? json.data : "{}");
    fflush(stderr);
    jbuf_free(&json);
}

bool headless_account_response_at(session_state_t *s, const char *provider,
                                  const char *key, const stream_result_t *r,
                                  const char *trace_id, const char *llm_span_id,
                                  headless_chronicle_status_t *chronicle_status) {
    if (chronicle_status) *chronicle_status = HEADLESS_CHRONICLE_NOT_ATTEMPTED;
    if (!s || !r) return false;
    inference_cost_t cost = {0};
    bool accounting_ok = inference_cost_record(s,provider,key,r,&cost);
    if (!accounting_ok) {
        fprintf(stderr,"error: headless accounting unavailable; stopping work\n");
    }
    if (accounting_ok) emit_headless_receipt(s, provider, r, &cost);
    bool partial = !r->ok && r->visible_text_streamed && r->http_status == 200 &&
        (r->diagnostic_reason == STREAM_DIAG_PROVIDER_INCOMPLETE ||
         r->diagnostic_reason == STREAM_DIAG_MISSING_TERMINAL ||
         r->diagnostic_reason == STREAM_DIAG_MISSING_STOP_REASON ||
         r->diagnostic_reason == STREAM_DIAG_TRANSPORT_ERROR ||
         r->diagnostic_reason == STREAM_DIAG_CALLBACK_ABORT ||
         r->diagnostic_reason == STREAM_DIAG_INTERRUPTED);
    jbuf_t visible;
    jbuf_init(&visible, 256);
    int tool_count = 0;
    if (r->parsed.blocks) for (int i = 0; i < r->parsed.count; i++) {
        const content_block_t *block = &r->parsed.blocks[i];
        if (block->type && strcmp(block->type, "tool_use") == 0) tool_count++;
        if ((r->ok || partial) && block->type && strcmp(block->type, "text") == 0 && block->text)
            jbuf_append(&visible, block->text);
    }
    const char *status = r->ok ? (r->parsed.stop_reason &&
        strcmp(r->parsed.stop_reason, "max_tokens") == 0 ? "truncated" : "completed")
        : r->diagnostic_reason == STREAM_DIAG_INTERRUPTED ? "interrupted"
        : partial && visible.len ? "partial_failed" : "failed";
    const char *kind = visible.len ? (tool_count ? "mixed" : "text")
                                   : tool_count && r->ok ? "tool_only" : "empty";
    bool recorded = chronicle_llm_response_ex(trace_id, llm_span_id, provider, s->model,
        visible.data ? visible.data : "", NULL,
        r->usage.input_tokens, r->usage.output_tokens,
        r->usage.cache_read_input_tokens, r->usage.cache_creation_input_tokens,
        r->reasoning_tokens, cost.budget_usd, r->telemetry.total_ms,
        r->parsed.stop_reason, r->generation_id, cost.budget_known, status, kind);
    jbuf_free(&visible);
    if (chronicle_status)
        *chronicle_status = recorded ? HEADLESS_CHRONICLE_RECORDED :
            chronicle_ready() ? HEADLESS_CHRONICLE_FAILED : HEADLESS_CHRONICLE_NOT_ATTEMPTED;
    if (chronicle_ready() && !recorded)
        fprintf(stderr, "warning: Chronicle response recording failed\n");
    if (!cost.budget_known) {
        fprintf(stderr,"  usage: input=%d output=%d cost=unknown total_known=$%.8f\n",
                r->usage.input_tokens,r->usage.output_tokens,s->total_reported_cost_usd);
        return accounting_ok;
    }
    fprintf(stderr,"  usage: input=%d output=%d cost=$%.8f total=$%.8f basis=%s%s\n",
            r->usage.input_tokens,r->usage.output_tokens,cost.budget_usd,
            s->total_reported_cost_usd,cost.budget_basis,
            cost.subscription_included?" billing=subscription (inference retained)":"");
    return accounting_ok;
}

bool headless_account_response(session_state_t *s, const char *provider,
                               const char *key, const stream_result_t *r) {
    return headless_account_response_at(s, provider, key, r, NULL, NULL, NULL);
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
