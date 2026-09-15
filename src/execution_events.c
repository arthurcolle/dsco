#include "execution_events.h"
#include "event_stream.h"
#include "json_util.h"
#include "gov_experiment.h"

static _Thread_local const execution_attempt_t *current;

const execution_attempt_t *execution_events_enter(const execution_attempt_t *attempt) {
    const execution_attempt_t *previous = current;
    current = attempt;
    return previous;
}
void execution_events_leave(const execution_attempt_t *previous) { current = previous; }

static void identity(jbuf_t *b, const execution_attempt_t *a) {
    jbuf_append(b, "{\"execution_id\":");
    jbuf_append_json_str(b, a ? a->execution_id : "");
    jbuf_append(b, ",\"parent_execution_id\":");
    jbuf_append_json_str(b, a ? a->parent_execution_id : "");
    jbuf_append(b, ",\"tool\":"); jbuf_append_json_str(b, a ? a->tool_name : "");
    jbuf_append(b, ",\"tier\":"); jbuf_append_json_str(b, a ? a->tier : "");
    jbuf_append(b, ",\"capabilities\":"); jbuf_append_json_str(b, a ? a->capabilities : "");
    jbuf_append(b, ",\"governance_model\":");
    jbuf_append_json_str(b, gov_model_name(gov_experiment_model()));
}
bool execution_events_record(const char *event, const execution_attempt_t *a,
                             const char *detail) {
    if (!event_stream_active()) return true;
    jbuf_t b; jbuf_init(&b, 1024);
    identity(&b, a);
    jbuf_append(&b, ",\"status\":");
    jbuf_append_json_str(&b, execution_attempt_status_name(a->status));
    jbuf_append(&b, ",\"dispatch_tool\":"); jbuf_append_json_str(&b, a->dispatch_tool_name);
    /* Exact tool I/O stays in the private journal; no JSON-string assumptions. */
    jbuf_append(&b, ",\"detail\":"); jbuf_append_json_str(&b, detail ? detail : "");
    jbuf_append(&b, "}");
    bool ok = event_stream_emit("execution", event, b.data);
    jbuf_free(&b);
    return ok;
}
void execution_events_stage(const char *stage, double elapsed_ms,
                            bool would_deny, bool enforced) {
    if (!event_stream_active()) return;
    jbuf_t b; jbuf_init(&b, 512);
    identity(&b, current);
    jbuf_append(&b, ",\"stage\":"); jbuf_append_json_str(&b, stage);
    jbuf_appendf(&b, ",\"elapsed_ms\":%.6f,\"would_deny\":%s,\"enforced\":%s}",
                 elapsed_ms, would_deny ? "true" : "false", enforced ? "true" : "false");
    (void)event_stream_emit("governance", "governance.stage", b.data);
    jbuf_free(&b);
}
