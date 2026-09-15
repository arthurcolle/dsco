#include "swarm_telemetry.h"
#include "json_util.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static bool amount_known(double value, bool known) {
    return known && isfinite(value) && value >= 0;
}
static void amount(jbuf_t *b, double value, bool known) {
    if (amount_known(value, known)) jbuf_appendf(b, "%.12g", value);
    else jbuf_append(b, "null");
}
static const char *status_name(swarm_status_t state) {
    static const char *names[] = {"pending","running","streaming","done","error","killed"};
    return state >= SWARM_PENDING && state <= SWARM_KILLED ? names[state] : "unknown";
}
static double age(const swarm_child_t *c, double now) {
    double end = c->end_time > 0 ? c->end_time : now;
    return c->start_time > 0 && end >= c->start_time ? end - c->start_time : 0;
}
bool swarm_health_json(const swarm_t *s, const char *input, char *out, size_t len) {
    if (!out || len < 3) return false;
    snprintf(out, len, "{}");
    if (!s) return false;
    double threshold = json_get_double(input, "stall_threshold_seconds", 300);
    int offset = json_get_int(input, "worker_offset", 0);
    int limit = json_get_int(input, "worker_limit", 128);
    if (!isfinite(threshold) || threshold <= 0 || offset < 0 || limit < 1 || limit > 1024) {
        const char *error = "{\"error\":\"invalid health threshold or worker pagination\"}";
        if (strlen(error) < len) strcpy(out, error);
        return false;
    }
    struct timeval tv; gettimeofday(&tv, NULL);
    double now = tv.tv_sec + tv.tv_usec / 1e6;
    int counts[6] = {0}, active = 0, retained = 0, unknown[3] = {0};
    int unpriced = 0, candidates = 0, progress_unknown = 0;
    double sums[3] = {0}, oldest = 0, last_progress = 0;
    for (int i = 0; i < s->child_count; i++) {
        const swarm_child_t *c = &s->children[i];
        if (c->reclaimable) continue;
        retained++;
        if (c->status >= SWARM_PENDING && c->status <= SWARM_KILLED) counts[c->status]++;
        bool running = swarm_active_test(s, i);
        if (running) {
            active++;
            if (age(c, now) > oldest) oldest = age(c, now);
            if (c->ui_last_emit_time > 0 && c->ui_last_emit_time <= now) {
                if (now - c->ui_last_emit_time >= threshold) candidates++;
                if (c->ui_last_emit_time > last_progress) last_progress = c->ui_last_emit_time;
            } else progress_unknown++;
        }
        double values[] = {c->reported_cost_usd, c->est_cost_usd, c->budget_accounted_usd};
        bool known[] = {c->reported_cost_known || c->reported_cost_usd > 0,
                       c->estimated_cost_known, c->budget_cost_known};
        for (int k = 0; k < 3; k++) {
            if (amount_known(values[k], known[k])) {
                sums[k] += values[k];
                if (!isfinite(sums[k])) unknown[k]++;
            }
            else unknown[k]++;
        }
        unpriced += c->unpriced_responses;
    }
    char total[3][48], retired[48];
    for (int k = 0; k < 3; k++) {
        if (isfinite(sums[k])) snprintf(total[k], sizeof(total[k]), "%.12g", sums[k]);
        else snprintf(total[k], sizeof(total[k]), "null");
    }
    double retired_value = s->retired_spent_usd+s->retired_subsidized_usd;
    bool retired_known = amount_known(s->retired_spent_usd, true) &&
                         amount_known(s->retired_subsidized_usd, true) && isfinite(retired_value);
    if (retired_known) snprintf(retired, sizeof(retired), "%.12g", retired_value);
    else snprintf(retired, sizeof(retired), "null");
    jbuf_t b; jbuf_init(&b, 4096);
    jbuf_appendf(&b, "{\"schema\":\"dsco.swarm_health.v1\",\"scope\":\"owned_runtime_retained_workers\","
        "\"counts\":{\"workers\":%d,\"active\":%d,\"pending\":%d,\"done\":%d,\"error\":%d,\"killed\":%d},"
        "\"oldest_active_worker_age_seconds\":%.3f,\"progress_source\":\"rate_limited_output_observation\","
        "\"last_observed_output_seconds_ago\":", retained, active, counts[0], counts[3], counts[4], counts[5], oldest);
    amount(&b, now-last_progress, last_progress > 0);
    jbuf_appendf(&b, ",\"progress_unavailable_workers\":%d,\"stall_threshold_seconds\":%.3f,"
        "\"output_silence_candidates\":%d,\"automatic_kill\":false,"
        "\"progress_caveat\":\"Output silence is not proof of stalled tools or inference; age alone is never a stall signal.\","
        "\"costs\":{\"includes_subscriptions\":true,\"reported_known_subtotal_usd\":%s,"
        "\"estimated_known_subtotal_usd\":%s,\"accounted_known_subtotal_usd\":%s,"
        "\"reported_unknown_workers\":%d,\"estimated_unknown_workers\":%d,\"accounted_unknown_workers\":%d,"
        "\"unpriced_responses\":%d,\"retired_unclassified_cost_usd\":%s,\"retired_breakdown_available\":false},"
        "\"capacity\":{\"configured_worker_limit\":%d,\"structural_worker_limit\":%d,\"active_slots\":%d,"
        "\"completion_notifications\":%d,\"allocated_group_slots\":%d,\"reusable_group_slots\":%d},"
        "\"enforced_budget_usd\":", progress_unknown, threshold, candidates, total[0], total[1], total[2],
        unknown[0], unknown[1], unknown[2], unpriced,
        retired, dsco_swarm_max_children(), SWARM_MAX_CHILDREN,
        active, s->done_q.count, s->group_count, s->free_group_count);
    amount(&b, s->swarm_budget_usd, s->swarm_budget_usd > 0);
    jbuf_append(&b, ",\"enforced_budget_basis\":\"metered_cost_only\",\"runtime_deadline\":null,"
                   "\"runtime_deadline_available\":false,\"workers\":[");
    int seen = 0, emitted = 0;
    for (int i = 0; i < s->child_count; i++) {
        const swarm_child_t *c = &s->children[i];
        if (c->reclaimable) continue;
        if (seen++ < offset || emitted >= limit) continue;
        jbuf_t row; jbuf_init(&row, 512);
        jbuf_appendf(&row, "%s{\"id\":%d,\"provider\":", emitted ? "," : "", c->id);
        jbuf_append_json_str(&row, c->provider);
        jbuf_append(&row, ",\"model\":"); jbuf_append_json_str(&row, c->model);
        jbuf_appendf(&row, ",\"status\":\"%s\",\"active\":%s,\"age_seconds\":%.3f,"
            "\"output_bytes\":%zu,\"last_observed_output_seconds_ago\":", status_name(c->status),
            swarm_active_test(s,i) ? "true" : "false", age(c,now), c->output_len);
        bool observed = c->ui_last_emit_time > 0 && c->ui_last_emit_time <= now;
        amount(&row, now-c->ui_last_emit_time, observed);
        jbuf_append(&row, ",\"stall_candidate\":");
        if (!swarm_active_test(s,i)) jbuf_append(&row, "false");
        else if (!observed) jbuf_append(&row, "null");
        else jbuf_append(&row, now-c->ui_last_emit_time >= threshold ? "true" : "false");
        jbuf_append(&row, "}");
        if (b.len + row.len + 160 < len) { jbuf_append(&b, row.data); emitted++; }
        else { jbuf_free(&row); break; }
        jbuf_free(&row);
    }
    int remaining = retained - offset - emitted;
    if (remaining < 0) remaining = 0;
    jbuf_appendf(&b, "],\"worker_offset\":%d,\"workers_returned\":%d,\"workers_remaining\":%d,\"next_worker_offset\":%d}",
                 offset, emitted, remaining, offset+emitted);
    bool ok = b.len < len;
    if (ok) memcpy(out, b.data, b.len+1);
    else if (len > 60) strcpy(out, "{\"error\":\"health output buffer too small\"}");
    jbuf_free(&b);
    return ok;
}
