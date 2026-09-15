#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#include "inference_cost.h"
#include "deepseek_pricing.h"
#include "config.h"
#include "provider.h"
#include "chronicle.h"
#include "json_util.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

static int add_count(int a, int b) {
    if (b < 0) return a;
    return b > INT_MAX - a ? INT_MAX : a + b;
}
static bool rate_known_for_usage(int tokens, double rate) {
    return tokens == 0 || (isfinite(rate) && rate >= 0);
}
void inference_cost_measure(const char *model, const stream_result_t *r,
                            bool included, inference_cost_t *out) {
    inference_cost_measure_for_provider(NULL, model, r, included, out);
}
void inference_cost_measure_for_provider(const char *provider, const char *model,
                            const stream_result_t *r, bool included, inference_cost_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->budget_basis = "unknown";
    out->pricing_source = "unknown";
    out->pricing_scope = "reference";
    out->subscription_included = included;
    if (!r) return;
    const usage_t *u = &r->usage;
    out->provider_reported_known = (r->cost_reported || r->cost_usd > 0) &&
                                   isfinite(r->cost_usd) && r->cost_usd >= 0;
    if (out->provider_reported_known) out->provider_reported_usd = r->cost_usd;
    const char *actual = r->actual_model && r->actual_model[0] ? r->actual_model : model;
    model_info_t priced_model;
    const model_info_t *mi = model_lookup_priced(actual, &priced_model);
    out->pricing_source = model_lookup_pricing_source(actual);
    if (provider && !strcmp(provider, "deepseek")) {
        model_price_t direct; time_t observed = 0; const char *source = NULL;
        if (deepseek_pricing_lookup(actual, time(NULL), &direct, &source, &observed)) {
            if (mi) priced_model = *mi; else memset(&priced_model, 0, sizeof(priced_model));
            priced_model.input_price = direct.input; priced_model.output_price = direct.output;
            priced_model.cache_read_price = direct.cached_input; priced_model.cache_write_price = direct.cache_write;
            mi = &priced_model; out->pricing_source = source; out->pricing_scope = "route_specific";
            out->pricing_observed_at = (long long)observed;
        } else {
            out->pricing_scope = "reference_fallback_direct_quote_unavailable";
        }
    }

    /* A static zero for an included billing lane is not a model price quote.
     * Keep the usage and any reported bill, but mark its inference value unknown. */
    bool unpriced_subscription = included && mi &&
        mi->input_price == 0.0 && mi->output_price == 0.0 &&
        mi->cache_read_price == 0.0 && mi->cache_write_price == 0.0 &&
        strcmp(model_lookup_pricing_source(actual), "static_registry_fallback") == 0;
    bool tokens_known = u->input_tokens > 0 || u->output_tokens > 0 ||
                        u->cache_read_input_tokens > 0 || u->cache_creation_input_tokens > 0;
    if (mi && !unpriced_subscription && tokens_known && u->input_tokens >= 0 && u->output_tokens >= 0 &&
        u->cache_read_input_tokens >= 0 && u->cache_creation_input_tokens >= 0 &&
        rate_known_for_usage(u->input_tokens, mi->input_price) &&
        rate_known_for_usage(u->output_tokens, mi->output_price) &&
        rate_known_for_usage(u->cache_read_input_tokens, mi->cache_read_price) &&
        rate_known_for_usage(u->cache_creation_input_tokens, mi->cache_write_price)) {
        out->input_per_million = mi->input_price;
        out->output_per_million = mi->output_price;
        out->cache_read_per_million = mi->cache_read_price;
        out->cache_write_per_million = mi->cache_write_price;
        out->estimated_usd = ((u->input_tokens ? u->input_tokens * mi->input_price : 0) +
                              (u->output_tokens ? u->output_tokens * mi->output_price : 0) +
                              (u->cache_read_input_tokens ? u->cache_read_input_tokens * mi->cache_read_price : 0) +
                              (u->cache_creation_input_tokens ? u->cache_creation_input_tokens * mi->cache_write_price : 0)) / 1e6;
        out->estimated_known = isfinite(out->estimated_usd) && out->estimated_usd >= 0;
    }
    /* A zero subscription bill is not zero inference value. Preserve both and
     * use the estimate for the resource budget when it is available. */
    if (out->provider_reported_known &&
        (!included || out->provider_reported_usd > 0)) {
        out->budget_usd = out->provider_reported_usd;
        out->budget_basis = "provider_reported";
        out->budget_known = true;
    } else if (out->estimated_known) {
        out->budget_usd = out->estimated_usd;
        out->budget_basis = "estimated";
        out->budget_known = true;
    }
}
static void nullable_cost(jbuf_t *b, const char *name, bool known, double value) {
    jbuf_appendf(b, ",\"%s\":", name);
    if (known) jbuf_appendf(b, "%.12f", value); else jbuf_append(b, "null");
}
/* Keep accounting even when the optional Chronicle journal is disabled.
 * Never store API keys. Each write is locked and fsync'd before returning. */
static bool fallback_append(const char *json) {
    char path[PATH_MAX];
    const char *override = getenv("DSCO_COST_LEDGER_PATH");
    if (override && *override) {
        if (snprintf(path, sizeof(path), "%s", override) >= (int)sizeof(path)) return false;
    } else {
        const char *home = getenv("HOME");
        if (!home || snprintf(path, sizeof(path), "%s/.dsco", home) >= (int)sizeof(path)) return false;
        if (mkdir(path, 0700) != 0 && errno != EEXIST) return false;
        if (snprintf(path, sizeof(path), "%s/.dsco/inference-costs.jsonl", home) >= (int)sizeof(path)) return false;
    }
    int fd = open(path, O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    bool ok = flock(fd, LOCK_EX) == 0;
    const char *p = json;size_t left = strlen(json);
    while (ok && left) {
        ssize_t n = write(fd,p,left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {ok=false;break;}
        p+=n;left-=(size_t)n;
    }
    if (ok) ok=write(fd,"\n",1)==1 && fsync(fd)==0;
    flock(fd,LOCK_UN);close(fd);return ok;
}
/* Native swarm workers receive a private, unlinked append descriptor. Keep
 * canonical usage separate from model stdout and stop if its transport fails. */
static bool worker_append(const char *json) {
    static int worker_fd = -1;
    const char *value = getenv("DSCO_WORKER_COST_FD");
    if (value && *value) {
        char *end = NULL;
        long parsed = strtol(value, &end, 10);
        if (!end || *end || parsed < 3 || parsed > INT_MAX) return false;
        worker_fd = (int)parsed;
        if (fcntl(worker_fd, F_SETFD, FD_CLOEXEC) < 0) return false;
        /* Tool subprocesses must inherit neither the descriptor nor a stale
         * descriptor number that could identify an unrelated open file. */
        unsetenv("DSCO_WORKER_COST_FD");
    }
    if (worker_fd < 0) return true;
    int fd = worker_fd;
    const char *p = json;
    size_t remaining = strlen(json);
    while (remaining) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; remaining -= (size_t)n;
    }
    return write(fd, "\n", 1) == 1;
}
bool inference_cost_record(session_state_t *s, const char *provider, const char *key,
                           const stream_result_t *r, inference_cost_t *out) {
    if (!s || !r || !out) return false;
    inference_cost_measure_for_provider(provider,s->model,r,provider_usage_is_included(provider,key),out);
    const usage_t *u=&r->usage;
    s->total_input_tokens=add_count(s->total_input_tokens,u->input_tokens);
    s->total_output_tokens=add_count(s->total_output_tokens,u->output_tokens);
    s->total_cache_read_tokens=add_count(s->total_cache_read_tokens,u->cache_read_input_tokens);
    s->total_cache_write_tokens=add_count(s->total_cache_write_tokens,u->cache_creation_input_tokens);
    s->total_reasoning_tokens=add_count(s->total_reasoning_tokens,r->reasoning_tokens);
    s->turn_count=add_count(s->turn_count,1);
    if (out->provider_reported_known) {
        s->total_provider_reported_cost_usd+=out->provider_reported_usd;
        s->provider_cost_samples=add_count(s->provider_cost_samples,1);
    }
    if (out->estimated_known) {
        s->total_estimated_inference_cost_usd+=out->estimated_usd;
        s->estimated_cost_samples=add_count(s->estimated_cost_samples,1);
    }
    if (out->budget_known) s->total_reported_cost_usd+=out->budget_usd;
    else s->unpriced_response_count=add_count(s->unpriced_response_count,1);
    if (out->subscription_included) s->subscription_response_count=add_count(s->subscription_response_count,1);
    jbuf_t b;jbuf_init(&b,1024);
    jbuf_append(&b,"{\"schema\":\"dsco.inference_cost.v1\",\"currency\":\"USD\",\"run_id\":");
    jbuf_append_json_str(&b,chronicle_run_id()?chronicle_run_id():"");
    jbuf_appendf(&b,",\"timestamp_unix\":%lld,\"attempt\":%d,\"http_status\":%d,\"success\":%s",
                 (long long)time(NULL),s->turn_count,r->http_status,r->ok?"true":"false");
    jbuf_append(&b,",\"request_id\":");jbuf_append_json_str(&b,r->generation_id?r->generation_id:"");
    jbuf_append(&b,",\"requested_model\":");jbuf_append_json_str(&b,s->model);
    jbuf_append(&b,",\"actual_model\":");jbuf_append_json_str(&b,r->actual_model?r->actual_model:s->model);
    jbuf_append(&b,",\"provider\":");jbuf_append_json_str(&b,provider?provider:"");
    jbuf_append(&b,",\"billing_lane\":");jbuf_append_json_str(&b,provider_auth_mode(provider,key));
    jbuf_append(&b,",\"token_basis\":\"input_excludes_cache\"");
    jbuf_appendf(&b,",\"subscription_included\":%s,\"input_tokens\":%d,\"output_tokens\":%d,\"cache_read_tokens\":%d,\"cache_write_tokens\":%d,\"reasoning_tokens\":%d,\"latency_ms\":%.3f",
                 out->subscription_included?"true":"false",u->input_tokens,u->output_tokens,
                 u->cache_read_input_tokens,u->cache_creation_input_tokens,r->reasoning_tokens,
                 isfinite(r->telemetry.total_ms)?r->telemetry.total_ms:0.0);
    nullable_cost(&b,"provider_reported_usd",out->provider_reported_known,out->provider_reported_usd);
    nullable_cost(&b,"estimated_inference_usd",out->estimated_known,out->estimated_usd);
    nullable_cost(&b,"budget_accounted_usd",out->budget_known,out->budget_usd);
    jbuf_append(&b,",\"budget_basis\":");jbuf_append_json_str(&b,out->budget_basis);
    jbuf_append(&b,",\"invoiced_usd\":null,\"allocated_subscription_usd\":null,\"subscription_allocation\":\"unallocated\"");
    jbuf_append(&b,",\"pricing_source\":");
    jbuf_append_json_str(&b,out->pricing_source);
    jbuf_append(&b,",\"pricing_scope\":");jbuf_append_json_str(&b,out->pricing_scope);
    jbuf_appendf(&b,",\"pricing_observed_at_unix\":%lld",out->pricing_observed_at);
    if (!strcmp(out->pricing_scope,"route_specific")) {
        jbuf_append(&b,",\"pricing_url\":");jbuf_append_json_str(&b,DEEPSEEK_PRICING_URL);
    }
    nullable_cost(&b,"input_per_million_usd",out->estimated_known && isfinite(out->input_per_million) && out->input_per_million >= 0,out->input_per_million);
    nullable_cost(&b,"output_per_million_usd",out->estimated_known && isfinite(out->output_per_million) && out->output_per_million >= 0,out->output_per_million);
    nullable_cost(&b,"cache_read_per_million_usd",out->estimated_known && isfinite(out->cache_read_per_million) && out->cache_read_per_million >= 0,out->cache_read_per_million);
    nullable_cost(&b,"cache_write_per_million_usd",out->estimated_known && isfinite(out->cache_write_per_million) && out->cache_write_per_million >= 0,out->cache_write_per_million);
    jbuf_append(&b,"}");
    bool saved=chronicle_journal_append("inference.cost.v1",b.data,true);
    if (!saved) saved=fallback_append(b.data);
    chronicle_event("inference.cost",NULL,NULL,NULL,"runtime","accounting",b.data,"product_telemetry");
    bool delivered = worker_append(b.data);
    jbuf_free(&b);
    if (!delivered) fprintf(stderr,"error: could not deliver native worker cost record\n");
    if (!saved) fprintf(stderr,"error: could not persist inference cost record\n");
    /* Unknown pricing is an accounting observation, not a generation failure.
     * The durable receipt retains null costs and the session's unpriced count;
     * do not discard an otherwise successful answer or trigger another request. */
    return saved && delivered;
}
