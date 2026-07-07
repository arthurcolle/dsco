#ifndef DSCO_AGENT_EVENT_H
#define DSCO_AGENT_EVENT_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    AGENT_EVENT_DURABLE      = 1u << 0,
    AGENT_EVENT_CALLBACK     = 1u << 1,
    AGENT_EVENT_NDJSON       = 1u << 2,
    AGENT_EVENT_OTLP         = 1u << 3,
    AGENT_EVENT_REDACT_LOCAL = 1u << 4,
    AGENT_EVENT_ARTIFACTIZE  = 1u << 5
} agent_event_flags_t;

typedef struct {
    const char *run_id;
    const char *root_run_id;
    const char *parent_run_id;
    const char *principal_tier;
    const char *principal_subject;
    const char *provider;
    const char *model;
    const char *topology;
    const char *worker_id;
    const char *trace_id;
    const char *span_id;
    const char *parent_span_id;
    const char *command_id;
    const char *caused_by_event_id;
    double usd_delta;
    double usd_total;
    int input_tokens;
    int output_tokens;
    int cache_read_tokens;
    int cache_write_tokens;
} agent_event_ctx_t;

bool agent_event_emit(const agent_event_ctx_t *ctx,
                      const char *event_name,
                      const char *status,
                      const char *payload_json,
                      agent_event_flags_t flags);

bool agent_event_emit_simple(const char *event_name,
                             const char *status,
                             const char *payload_json,
                             agent_event_flags_t flags);

#endif
