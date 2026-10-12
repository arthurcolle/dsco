#include "request_context.h"
#include "compact_policy.h"
#include "input_budget.h"
#include "tools.h"
#include "chronicle.h"
#include "env_config.h"
#include "error.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUEST_WIRE_TARGET_DEFAULT 0U
#define REQUEST_WIRE_TARGET_MIN     (64U * 1024U)
#define REQUEST_WIRE_TARGET_MAX     (64U * 1024U * 1024U)

/* Opt-in latency/transfer working-set target, not an admission limit.
 * Default: retain history until the model-context policy requires archival.
 * `off`/`0` leaves the model-context policy as the only rolling trigger. */
static size_t request_wire_target_bytes(void) {
    const char *raw = getenv("DSCO_REQUEST_TARGET_BYTES");
    if (raw && dsco_env_falsy_str(raw)) return 0;
    return dsco_env_size("DSCO_REQUEST_TARGET_BYTES", REQUEST_WIRE_TARGET_DEFAULT,
                         REQUEST_WIRE_TARGET_MIN, REQUEST_WIRE_TARGET_MAX);
}

static bool archive_step(conversation_t *conv, session_state_t *session, int keep, bool *ok) {
    *ok = true;
    if (!session || !session->compact_enabled) return false;
    tools_set_active_conversation(conv);
    tools_set_active_session(session);
    char args[128], result[2048] = "";
    snprintf(args, sizeof(args), "{\"keep_recent\":%d,\"max_turns\":64,\"min_bytes\":512}",
             keep < 2 ? 2 : keep);
    *ok = tools_execute_for_tier("context_evict", args,
        session_trust_tier_to_string(session->trust_tier), result, sizeof(result));
    chronicle_event("context.rolling", NULL, NULL, NULL,
                    "runtime", "archive", result, "product_telemetry");
    /* Partial success still changed history: always rebuild after mutation. */
    return json_get_bool(result, "changed", false);
}

bool request_context_archive(conversation_t *conv, session_state_t *session, int keep) {
    bool ok;
    return archive_step(conv,session,keep,&ok);
}

char *request_context_build(provider_t *provider, conversation_t *conv,
                            session_state_t *session, const char *key, int max_tokens) {
    if (!conv || !session) return NULL;
    int previous = INT_MAX;
    size_t previous_wire = SIZE_MAX, initial_wire = 0;
    const size_t wire_target = request_wire_target_bytes();
    int archive_attempts = 0, archive_passes = 0;
    bool wire_target_triggered = false, archive_ok = true;
    const char *archive_stop = "pressure_satisfied";
    for (;;) {
        char *request = provider
            ? provider->build_request(provider, conv, session, max_tokens, key)
            : llm_build_request_ex_for_credential(conv, session, max_tokens, key);
        if (!request) return NULL;
        size_t wire = strlen(request);
        if (!initial_wire) initial_wire = wire;
        int raw = input_budget_estimate(request);
        int limit = input_budget_effective_limit(session->context_window, max_tokens);
        conversation_t recent = {0};
        if (conv->count) {
            recent.count = conv->count < 2 ? conv->count : 2;
            recent.msgs = conv->msgs + conv->count - recent.count;
            recent.cap = recent.count;
        }
        int trigger = compact_rolling_trigger(limit, conv_rough_estimate(&recent));
        bool token_pressure = trigger > 0 && raw > trigger;
        bool wire_pressure = wire_target > 0 && wire > wire_target;
        wire_target_triggered |= wire_pressure;
        bool tried_archive = false;
        if (archive_ok && (token_pressure || wire_pressure) && raw <= previous && wire <= previous_wire &&
            (raw < previous || wire < previous_wire)) {
            previous = raw;
            previous_wire = wire;
            archive_attempts++;
            tried_archive = true;
            bool changed = archive_step(conv, session, 2, &archive_ok);
            archive_stop = archive_ok ? "no_eligible_old_content" : "archive_denied_or_failed";
            if (changed) {
                archive_passes++;
                free(request);
                continue;
            }
        }
        if (!archive_ok) archive_stop="archive_denied_or_failed";
        else if (!token_pressure && !wire_pressure) archive_stop="pressure_satisfied";
        else if (!session->compact_enabled) archive_stop="compaction_disabled";
        else if (archive_ok && archive_passes && !tried_archive)
            archive_stop="no_further_reduction";
        int envelope = input_budget_overhead_estimate(request);
        if (envelope >= 0) session->non_conv_overhead_tokens = envelope;
        input_budget_result_t budget;
        bool allowed = input_budget_apply(&request, session->context_window,
                                          max_tokens, &budget);
        tools_set_request_budget(budget.before_tokens, budget.after_tokens, budget.limit);
        char metadata[896];
        snprintf(metadata, sizeof(metadata),
            "{\"basis\":\"local_estimate_v1\",\"limit\":%d,\"before_tokens\":%d,"
            "\"after_tokens\":%d,\"reduced_fields\":%d,\"admitted\":%s,"
            "\"wire_bytes_before\":%zu,\"wire_bytes_after\":%zu,"
            "\"wire_target_bytes\":%zu,\"wire_target_triggered\":%s,"
            "\"wire_target_satisfied\":%s,\"archive_attempts\":%d,"
            "\"archive_passes\":%d,\"archive_stop\":\"%s\",\"archive_ok\":%s,\"envelope_tokens\":%d,\"compaction_enabled\":%s}",
            budget.limit, budget.before_tokens, budget.after_tokens, budget.reduced_fields,
            allowed ? "true" : "false", initial_wire, strlen(request), wire_target,
            wire_target_triggered ? "true" : "false",
            (!wire_target || strlen(request) <= wire_target) ? "true" : "false",
            archive_attempts, archive_passes, archive_stop, archive_ok ? "true" : "false", envelope, session->compact_enabled ? "true" : "false");
        chronicle_event("request.input_budget", NULL, NULL, NULL, "runtime", "admission",
                        metadata, "product_telemetry");
        if (!allowed) {
            DSCO_SET_ERR(DSCO_ERR_BUDGET, "%s", budget.reason);
            free(request);
            return NULL;
        }
        return request;
    }
}
