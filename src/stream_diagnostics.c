#include "stream_diagnostics.h"
#include "chronicle.h"
#include <math.h>
#include <stdio.h>

const char *stream_diagnostic_name(stream_diagnostic_t reason) {
    switch (reason) {
    case STREAM_DIAG_NONE: return "none";
    case STREAM_DIAG_MISSING_TERMINAL: return "missing_terminal_event";
    case STREAM_DIAG_MISSING_STOP_REASON: return "missing_stop_reason";
    case STREAM_DIAG_UNFINISHED_BLOCK: return "unfinished_content_block";
    case STREAM_DIAG_OVERLAPPING_BLOCKS: return "overlapping_content_blocks";
    case STREAM_DIAG_INVALID_TOOL_CALL: return "invalid_tool_call";
    case STREAM_DIAG_PROVIDER_INCOMPLETE: return "provider_reported_incomplete";
    case STREAM_DIAG_PROVIDER_ERROR: return "provider_error";
    case STREAM_DIAG_TRANSPORT_ERROR: return "transport_error";
    case STREAM_DIAG_CALLBACK_ABORT: return "callback_abort";
    case STREAM_DIAG_INTERRUPTED: return "interrupted";
    default: return "unspecified_failure";
    }
}

bool stream_diagnostics_record(stream_diagnostic_t reason, bool ok, int http_status,
                               double latency_ms, const char *trace_id,
                               const char *parent_span_id) {
    if (!ok && reason == STREAM_DIAG_NONE) reason = STREAM_DIAG_UNSPECIFIED_FAILURE;
    if (!isfinite(latency_ms) || latency_ms < 0) latency_ms = 0;
    char payload[384], span[37];
    snprintf(payload, sizeof(payload),
             "{\"schema\":\"dsco.stream_outcome.v1\",\"diagnostic_reason\":\"%s\","
             "\"ok\":%s,\"http_status\":%d,\"latency_ms\":%.3f}",
             stream_diagnostic_name(reason), ok ? "true" : "false", http_status, latency_ms);
    chronicle_new_id(span, sizeof(span));
    return chronicle_event("llm.stream.outcome", trace_id, span, parent_span_id,
                           "runtime", "stream_diagnostics", payload, "product_telemetry");
}
