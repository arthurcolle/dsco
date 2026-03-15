#ifndef DSCO_LLM_H
#define DSCO_LLM_H

#include <stdbool.h>
#include <stddef.h>
#include "json_util.h"

typedef enum { ROLE_USER, ROLE_ASSISTANT } msg_role_t;

typedef struct {
    char *type;
    char *text;
    char *tool_name;
    char *tool_id;
    char *tool_input;
    bool  is_error;
    /* Vision: image content */
    char *image_media_type;  /* e.g., "image/png", "image/jpeg" */
    char *image_data;        /* base64-encoded image data */
    char *image_url;         /* URL source for image */
    /* Document: PDF content */
    char *doc_media_type;    /* e.g., "application/pdf" */
    char *doc_data;          /* base64-encoded PDF data */
    char *doc_title;         /* optional title */
    /* Citation support */
    char *cited_text;        /* text being cited */
    int   cite_start;        /* citation start index */
    int   cite_end;          /* citation end index */
} msg_content_t;

typedef struct {
    msg_role_t    role;
    msg_content_t *content;
    int            content_count;
} message_t;

typedef struct {
    message_t *msgs;
    int        count;
    int        cap;
} conversation_t;

typedef struct {
    int input_tokens;
    int output_tokens;
    int cache_creation_input_tokens;
    int cache_read_input_tokens;
} usage_t;

/* ── Streaming telemetry ───────────────────────────────────────────────── */

/* F40: cURL latency breakdown phases */
typedef struct {
    double dns_ms;
    double connect_ms;
    double tls_ms;
    double ttfb_ms;
    double total_ms;
} llm_latency_breakdown_t;

typedef struct {
    double ttft_ms;           /* time to first token (ms) */
    double ttft_tool_ms;      /* time to first tool_use (ms) */
    double total_ms;          /* total streaming duration (ms) */
    double tokens_per_sec;    /* output token throughput */
    int    thinking_tokens;   /* tokens spent on thinking */
    llm_latency_breakdown_t latency;  /* F40: cURL timing phases */
} stream_telemetry_t;

/* Streaming callback: called with text deltas as they arrive */
typedef void (*stream_text_cb)(const char *text, void *ctx);
/* Called when a tool_use block starts */
typedef void (*stream_tool_start_cb)(const char *name, const char *id, void *ctx);
/* Called with thinking text deltas (extended thinking / interleaved thinking) */
typedef void (*stream_thinking_cb)(const char *text, void *ctx);

/* Streaming result — accumulated content blocks + usage + telemetry */
typedef struct {
    parsed_response_t  parsed;
    usage_t            usage;
    stream_telemetry_t telemetry;
    int                http_status;
    bool               ok;
} stream_result_t;

/* ── Session state (mutable per-session settings) ──────────────────────── */

typedef enum {
    DSCO_TRUST_STANDARD = 0,
    DSCO_TRUST_TRUSTED,
    DSCO_TRUST_UNTRUSTED,
} dsco_trust_tier_t;

typedef struct {
    char   model[128];        /* current model ID */
    char   active_skill[128];
    char   active_topology[48];
    char   effort[16];        /* "low", "medium", "high" */
    dsco_trust_tier_t trust_tier; /* tool permission tier */
    bool   web_search;        /* enable server-side web search */
    bool   code_execution;    /* enable server-side code execution */
    char   container_id[128]; /* reuse code execution container */
    int    context_window;    /* tokens for current model */
    /* Cost tracking */
    int    total_input_tokens;
    int    total_output_tokens;
    int    total_cache_read_tokens;
    int    total_cache_write_tokens;
    int    turn_count;
    /* Compaction */
    bool   compact_enabled;
    /* Tool choice: "" = auto, "any" = any, "tool:name" = force specific */
    char   tool_choice[128];
    /* Prefill: pre-seed assistant response (for JSON forcing, format control) */
    char   prefill[1024];
    /* Stop sequences */
    char   stop_seq[256];
    /* Sampling parameters */
    double temperature;       /* -1 = not set (use default) */
    double top_p;             /* -1 = not set */
    int    top_k;             /* -1 = not set */
    /* Thinking budget */
    int    thinking_budget;   /* 0 = adaptive (default), >0 = fixed budget */
    /* Fallback chain */
    char   fallback_models[4][128];
    int    fallback_count;
    bool   model_locked;      /* block auto-routing model switches */
    /* Telemetry aggregates */
    double total_ttft_ms;
    double total_stream_ms;
    int    telemetry_samples;
    bool   topology_auto;
} session_state_t;

void  session_state_init(session_state_t *s, const char *model);
const char *session_trust_tier_to_string(dsco_trust_tier_t tier);
dsco_trust_tier_t session_trust_tier_from_string(const char *s, bool *ok);

void  conv_init(conversation_t *c);
void  conv_free(conversation_t *c);
void  conv_add_user_text(conversation_t *c, const char *text);
void  conv_add_assistant_text(conversation_t *c, const char *text);
void  conv_add_assistant_tool_use(conversation_t *c, const char *tool_id,
                                   const char *tool_name, const char *tool_input);
void  conv_add_tool_result(conversation_t *c, const char *tool_id,
                           const char *result, bool is_error);
void  conv_add_assistant_raw(conversation_t *c, parsed_response_t *resp);
void  conv_add_user_image_base64(conversation_t *c, const char *media_type,
                                  const char *base64_data, const char *text);
void  conv_add_user_image_url(conversation_t *c, const char *url, const char *text);
void  conv_add_user_document(conversation_t *c, const char *media_type,
                              const char *base64_data, const char *title,
                              const char *text);

void  conv_pop_last(conversation_t *c);
void  conv_trim_old_results(conversation_t *c, int keep_recent, int max_chars);

bool  conv_save(conversation_t *c, const char *path);
bool  conv_load(conversation_t *c, const char *path);
bool  conv_save_ex(conversation_t *c, const session_state_t *session, const char *path);
bool  conv_load_ex(conversation_t *c, session_state_t *session, const char *path);

char *llm_build_request(conversation_t *c, const char *model, int max_tokens);
char *llm_build_request_ex(conversation_t *c, session_state_t *session, int max_tokens);

int   llm_count_tokens(const char *api_key, const char *request_json);
const char *llm_get_custom_system_prompt(void);
void  llm_debug_save_request(const char *request_json, int http_status);

stream_result_t llm_stream(const char *api_key, const char *request_json,
                           stream_text_cb text_cb,
                           stream_tool_start_cb tool_cb,
                           stream_thinking_cb thinking_cb,
                           void *cb_ctx);
void dsco_strip_terminal_controls_inplace(char *s);

/* ── Per-tool metrics ──────────────────────────────────────────────────── */

#define TOOL_METRICS_MAX 256

typedef struct {
    char   name[64];
    int    calls;
    int    successes;
    int    failures;
    int    timeouts;          /* watchdog timeout count */
    double total_latency_ms;
    double max_latency_ms;
    double min_latency_ms;
} tool_metric_t;

typedef struct {
    tool_metric_t entries[TOOL_METRICS_MAX];
    int           count;
} tool_metrics_t;

void  tool_metrics_init(tool_metrics_t *m);
void  tool_metrics_record(tool_metrics_t *m, const char *name,
                            bool success, double latency_ms);
const tool_metric_t *tool_metrics_get(tool_metrics_t *m, const char *name);

/* ── Tool result cache (LRU) ──────────────────────────────────────────── */

#define TOOL_CACHE_SIZE 128

typedef struct {
    char    key[256];         /* "tool_name:input_hash" */
    char   *result;
    bool    success;
    double  timestamp;
    double  ttl;              /* seconds */
} tool_cache_entry_t;

typedef struct {
    tool_cache_entry_t entries[TOOL_CACHE_SIZE];
    int                count;
    int                hits;
    int                misses;
} tool_cache_t;

void  tool_cache_init(tool_cache_t *c);
void  tool_cache_free(tool_cache_t *c);
bool  tool_cache_get(tool_cache_t *c, const char *tool, const char *input,
                       char *result, size_t rlen, bool *success);
void  tool_cache_put(tool_cache_t *c, const char *tool, const char *input,
                       const char *result, bool success, double ttl);

/* ── Streaming checkpoint (retry resilience) ──────────────────────────── */

typedef struct {
    content_block_t *saved_blocks;  /* completed blocks before retry */
    int              saved_count;
    char            *partial_text;  /* partial text buffer */
    char            *partial_input; /* partial tool input buffer */
    usage_t          saved_usage;
    stream_telemetry_t saved_telemetry;
} stream_checkpoint_t;

void stream_checkpoint_init(stream_checkpoint_t *cp);
void stream_checkpoint_save(stream_checkpoint_t *cp,
                            const content_block_t *blocks, int block_count,
                            const char *partial_text, const char *partial_input,
                            const usage_t *usage, const stream_telemetry_t *telemetry);
void stream_checkpoint_free(stream_checkpoint_t *cp);

/* ── Prompt injection detection ────────────────────────────────────────── */

typedef enum {
    INJECTION_NONE = 0,
    INJECTION_LOW  = 1,
    INJECTION_MED  = 2,
    INJECTION_HIGH = 3,
} injection_level_t;

injection_level_t detect_prompt_injection(const char *text);

#endif
