#ifndef DSCO_BASELINE_H
#define DSCO_BASELINE_H

#include <stdbool.h>
#include <stddef.h>

/* Initialize baseline storage for this process instance. */
bool baseline_start(const char *model, const char *mode);

/* Flush and close baseline storage. Safe to call multiple times. */
void baseline_stop(void);

/* Record a timeline event for the current instance. */
bool baseline_log(const char *category, const char *title,
                  const char *detail, const char *metadata_json);

/* Current instance id and SQLite file path. Empty string if not started. */
const char *baseline_instance_id(void);
const char *baseline_db_path(void);

/* Run a local HTTP timeline server (blocking). */
int baseline_serve_http(int port, const char *default_instance_filter);

/* ── Trace span system ────────────────────────────────────────────────── */

typedef struct {
    char   span_id[37];       /* UUID v4 */
    char   trace_id[37];      /* UUID v4 per user prompt */
    char   parent_span[37];   /* empty if root */
    char   name[128];
    double start_epoch;
    double end_epoch;
    char   status[16];        /* "ok", "error", "timeout" */
    char   metadata_json[512];
} trace_span_t;

/* Generate a new UUID-based trace ID */
void trace_new_id(char *out, size_t out_len);

/* Begin a span: writes span_id to span_id_out (must be >= 37 chars) */
bool trace_span_begin(const char *trace_id, const char *name,
                      const char *parent_span, char *span_id_out);

/* End a span: records end_epoch, status, and optional metadata */
bool trace_span_end(const char *span_id, const char *status,
                    const char *metadata_json);

/* Query recent traces (prints formatted output to stderr) */
void trace_query_recent(int limit);

/* Print timing waterfall for a specific trace */
void trace_print_waterfall(const char *trace_id);

#endif
