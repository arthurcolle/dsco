#ifndef DSCO_STREAM_DIAGNOSTICS_H
#define DSCO_STREAM_DIAGNOSTICS_H
#include <stdbool.h>

/* Diagnostic labels only: never used to select retry or authorization policy. */
typedef enum {
    STREAM_DIAG_NONE = 0,
    STREAM_DIAG_MISSING_TERMINAL,
    STREAM_DIAG_MISSING_STOP_REASON,
    STREAM_DIAG_UNFINISHED_BLOCK,
    STREAM_DIAG_OVERLAPPING_BLOCKS,
    STREAM_DIAG_INVALID_TOOL_CALL,
    STREAM_DIAG_PROVIDER_INCOMPLETE,
    STREAM_DIAG_PROVIDER_ERROR,
    STREAM_DIAG_TRANSPORT_ERROR,
    STREAM_DIAG_CALLBACK_ABORT,
    STREAM_DIAG_INTERRUPTED,
    STREAM_DIAG_UNSPECIFIED_FAILURE
} stream_diagnostic_t;

const char *stream_diagnostic_name(stream_diagnostic_t reason);
/* Inherits Chronicle's existing enablement, retention and session context.
 * No request, result, error-body, credential or raw provider event capture. */
bool stream_diagnostics_record(stream_diagnostic_t reason, bool ok, int http_status,
                               double latency_ms, const char *trace_id,
                               const char *parent_span_id);
#endif
