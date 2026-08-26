#include "agent_event.h"

#include "chronicle.h"
#include "callbacks.h"
#include "json_util.h"
#include "rl_hooks.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static unsigned long long g_agent_event_seq = 0;

static const char *nz(const char *s) { return s ? s : ""; }

static long long now_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return (long long)time(NULL) * 1000LL;
    return (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000);
}

static const char *event_category(const char *event) {
    if (!event) return "unknown";
    if (strncmp(event, "run.", 4) == 0) return "run";
    if (strncmp(event, "turn.", 5) == 0) return "turn";
    if (strncmp(event, "model.", 6) == 0) return "model";
    if (strncmp(event, "tool.", 5) == 0) return "tool";
    if (strncmp(event, "approval.", 9) == 0 || strncmp(event, "permission.", 11) == 0 ||
        strncmp(event, "budget.", 7) == 0 || strncmp(event, "kill_switch.", 12) == 0)
        return "governance";
    if (strncmp(event, "artifact.", 9) == 0) return "artifact";
    if (strncmp(event, "swarm.", 6) == 0 || strncmp(event, "worker.", 7) == 0)
        return "swarm";
    if (strncmp(event, "eval.", 5) == 0) return "eval";
    if (strncmp(event, "callback.", 9) == 0) return "callback";
    return "agent";
}

static const char *event_phase(const char *event) {
    if (!event) return "observe";
    if (strncmp(event, "run.accept", 10) == 0 || strncmp(event, "auth.", 5) == 0)
        return "ingress";
    if (strncmp(event, "model.", 6) == 0 || strncmp(event, "turn.", 5) == 0)
        return "reason";
    if (strncmp(event, "tool.", 5) == 0 || strncmp(event, "artifact.", 9) == 0)
        return "execute";
    if (strncmp(event, "callback.", 9) == 0) return "egress";
    if (strstr(event, "resume") || strstr(event, "recover")) return "recover";
    return "observe";
}

static const char *event_severity(const char *event, const char *status) {
    if ((status && (strcmp(status, "error") == 0 || strcmp(status, "failed") == 0)) ||
        (event && (strstr(event, ".failed") || strstr(event, ".timeout") || strstr(event, ".blocked") || strstr(event, "exhausted"))))
        return "error";
    if (event && (strstr(event, "warning") || strstr(event, "denied") || strstr(event, "dead_letter")))
        return "warning";
    if (event && strstr(event, "delta")) return "debug";
    return "info";
}

static void append_json_or_empty(jbuf_t *b, const char *json) {
    if (json && json_is_valid_container(json)) jbuf_append(b, json);
    else if (json && json[0]) {
        jbuf_append(b, "{\"text\":");
        jbuf_append_json_str(b, json);
        jbuf_append(b, "}");
    } else {
        jbuf_append(b, "{}");
    }
}

bool agent_event_emit(const agent_event_ctx_t *ctx,
                      const char *event_name,
                      const char *status,
                      const char *payload_json,
                      agent_event_flags_t flags) {
    (void)flags;
    if (!event_name || !event_name[0]) return false;
    char event_id[37];
    chronicle_new_id(event_id, sizeof(event_id));
    const char *run_id = (ctx && ctx->run_id && ctx->run_id[0]) ? ctx->run_id : chronicle_run_id();
    if (!run_id || !run_id[0]) run_id = chronicle_session_id();
    unsigned long long seq = ++g_agent_event_seq;
    jbuf_t b;
    jbuf_init(&b, 1024);
    jbuf_append(&b, "{\"schema\":\"dsco.agent_event.v1\"");
    jbuf_append(&b, ",\"event_id\":"); jbuf_append_json_str(&b, event_id);
    jbuf_append(&b, ",\"run_id\":"); jbuf_append_json_str(&b, nz(run_id));
    jbuf_append(&b, ",\"root_run_id\":");
    jbuf_append_json_str(&b, ctx && ctx->root_run_id ? ctx->root_run_id : nz(run_id));
    if (ctx && ctx->parent_run_id && ctx->parent_run_id[0]) {
        jbuf_append(&b, ",\"parent_run_id\":"); jbuf_append_json_str(&b, ctx->parent_run_id);
    }
    jbuf_appendf(&b, ",\"sequence\":%llu,\"timestamp_ms\":%lld", seq, now_ms());
    jbuf_append(&b, ",\"event\":"); jbuf_append_json_str(&b, event_name);
    jbuf_append(&b, ",\"category\":"); jbuf_append_json_str(&b, event_category(event_name));
    jbuf_append(&b, ",\"phase\":"); jbuf_append_json_str(&b, event_phase(event_name));
    jbuf_append(&b, ",\"status\":"); jbuf_append_json_str(&b, status && status[0] ? status : "ok");
    jbuf_append(&b, ",\"severity\":"); jbuf_append_json_str(&b, event_severity(event_name, status));

    jbuf_append(&b, ",\"causality\":{");
    bool comma = false;
    if (ctx && ctx->command_id && ctx->command_id[0]) { jbuf_append(&b, "\"command_id\":"); jbuf_append_json_str(&b, ctx->command_id); comma = true; }
    if (ctx && ctx->caused_by_event_id && ctx->caused_by_event_id[0]) { if (comma) jbuf_append(&b, ","); jbuf_append(&b, "\"caused_by_event_id\":"); jbuf_append_json_str(&b, ctx->caused_by_event_id); comma = true; }
    if (ctx && ctx->parent_span_id && ctx->parent_span_id[0]) { if (comma) jbuf_append(&b, ","); jbuf_append(&b, "\"parent_span_id\":"); jbuf_append_json_str(&b, ctx->parent_span_id); }
    jbuf_append(&b, "}");

    jbuf_append(&b, ",\"principal\":{\"tier\":");
    jbuf_append_json_str(&b, ctx && ctx->principal_tier ? ctx->principal_tier : "agent");
    jbuf_append(&b, ",\"subject\":");
    jbuf_append_json_str(&b, ctx && ctx->principal_subject ? ctx->principal_subject : "local");
    jbuf_append(&b, "}");

    /* EntityCapsule identity is process-immutable admission context. Keep it
     * on every canonical event so Router, GraphSub, Chronicle, and dsco-ops
     * can reconcile the same organization/deployment/policy lineage. */
    jbuf_append(&b, ",\"organization\":{\"organization_id\":");
    jbuf_append_json_str(&b, nz(getenv("DSCO_ORGANIZATION_ID")));
    jbuf_append(&b, ",\"deployment_id\":");
    jbuf_append_json_str(&b, nz(getenv("DSCO_DEPLOYMENT_ID")));
    jbuf_append(&b, ",\"policy_sha256\":");
    jbuf_append_json_str(&b, nz(getenv("DSCO_POLICY_SHA256")));
    jbuf_append(&b, ",\"role_id\":");
    jbuf_append_json_str(&b, nz(getenv("DSCO_ROLE_ID")));
    jbuf_append(&b, ",\"agent_id\":");
    jbuf_append_json_str(&b, nz(getenv("DSCO_AGENT_ID")));
    jbuf_append(&b, "}");

    jbuf_append(&b, ",\"agent\":{\"provider\":");
    jbuf_append_json_str(&b, ctx && ctx->provider ? ctx->provider : "");
    jbuf_append(&b, ",\"model\":"); jbuf_append_json_str(&b, ctx && ctx->model ? ctx->model : "");
    jbuf_append(&b, ",\"topology\":"); jbuf_append_json_str(&b, ctx && ctx->topology ? ctx->topology : "");
    jbuf_append(&b, ",\"worker_id\":"); jbuf_append_json_str(&b, ctx && ctx->worker_id ? ctx->worker_id : "");
    jbuf_append(&b, "}");

    jbuf_append(&b, ",\"trace\":{\"trace_id\":");
    jbuf_append_json_str(&b, ctx && ctx->trace_id ? ctx->trace_id : "");
    jbuf_append(&b, ",\"span_id\":"); jbuf_append_json_str(&b, ctx && ctx->span_id ? ctx->span_id : "");
    jbuf_append(&b, ",\"parent_span_id\":"); jbuf_append_json_str(&b, ctx && ctx->parent_span_id ? ctx->parent_span_id : "");
    jbuf_append(&b, "}");

    jbuf_appendf(&b, ",\"cost\":{\"usd_delta\":%.8f,\"usd_total\":%.8f,\"input_tokens\":%d,\"output_tokens\":%d,\"cache_read_tokens\":%d,\"cache_write_tokens\":%d}",
                 ctx ? ctx->usd_delta : 0.0, ctx ? ctx->usd_total : 0.0,
                 ctx ? ctx->input_tokens : 0, ctx ? ctx->output_tokens : 0,
                 ctx ? ctx->cache_read_tokens : 0, ctx ? ctx->cache_write_tokens : 0);

    jbuf_append(&b, ",\"payload\":");
    append_json_or_empty(&b, payload_json);
    jbuf_append(&b, ",\"artifact_refs\":[]");
    jbuf_append(&b, ",\"redaction\":{\"mode\":\"local-full\",\"rules_applied\":[]}");
    char idem[256];
    snprintf(idem, sizeof(idem), "%s:%llu:%s", nz(run_id), seq, event_name);
    jbuf_append(&b, ",\"idempotency_key\":");
    jbuf_append_json_str(&b, idem);
    jbuf_append(&b, "}");

    bool ok = chronicle_journal_append(event_name, b.data, (flags & AGENT_EVENT_DURABLE) != 0);
    if (ok) {
        rl_hook_event_t trajectory_event = {
            .run_id = run_id,
            .sequence = seq,
            .timestamp_ms = now_ms(),
            .event_name = event_name,
            .category = event_category(event_name),
            .status = status && status[0] ? status : "ok",
            .payload_json = payload_json,
            .usd_delta = ctx ? ctx->usd_delta : 0.0,
            .input_tokens = ctx ? ctx->input_tokens : 0,
            .output_tokens = ctx ? ctx->output_tokens : 0,
        };
        /* Observability-only: failure to project a learning trajectory must not
         * affect an authorized agent action or its canonical event journal. */
        (void)rl_hooks_record_event(&trajectory_event);
    }
    if (ok && (flags & AGENT_EVENT_CALLBACK)) {
        callback_policy_t policy;
        if (callback_policy_from_env(&policy))
            callback_outbox_enqueue(&policy, nz(run_id), event_name, b.data);
    }
    if (ok && (flags & AGENT_EVENT_NDJSON)) {
        fputs(b.data, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    jbuf_free(&b);
    return ok;
}

bool agent_event_emit_simple(const char *event_name,
                             const char *status,
                             const char *payload_json,
                             agent_event_flags_t flags) {
    return agent_event_emit(NULL, event_name, status, payload_json, flags);
}
