#ifndef DSCO_LLM_H
#define DSCO_LLM_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
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
    bool               context_overflow;  /* provider rejected prompt as too long → reactive compaction can retry */
    double             cost_usd;           /* authoritative per-turn cost reported by provider (OpenRouter usage.cost); 0 = not reported, fall back to token math */
    time_t             credit_reset_at;    /* provider-supplied epoch seconds when exhausted subscription/rate window reopens */
    /* OpenRouter/provider metadata — printed after md_flush, never inside stream */
    char              *actual_model;       /* model actually used (may differ from requested) */
    char              *generation_id;      /* provider generation/request ID */
    int                reasoning_tokens;   /* reasoning tokens this turn */
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
    /* Authoritative session cost. Accumulates the provider-reported cost
     * (OpenRouter usage.cost) when present, else the per-turn token-math
     * estimate. Used for budget enforcement so caching discounts are
     * reflected instead of billing every cached token at full input price. */
    double total_reported_cost_usd;
    /* Most recent API response's input usage (single turn, not cumulative).
     * Used by conv_token_estimate to calibrate the rough estimate against
     * what the API actually counted. */
    int    last_input_tokens;
    /* Tokens consumed by things that don't live in `conv` (system prompt,
     * tool schemas, cached prefix). Derived after each API response as
     * last_input_tokens - rough_conv_estimate, and added back into
     * conv_token_estimate so threshold checks see the true context size
     * while pre/post compaction deltas still reflect real conversation
     * shrinkage. */
    int    non_conv_overhead_tokens;
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
    char   prompt_cache_key[128];       /* provider-native cache routing hint */
    char   prompt_cache_retention[16];  /* OpenAI/Azure: "in_memory" or "24h" */
    /* Telemetry aggregates */
    double total_ttft_ms;
    double total_stream_ms;
    int    telemetry_samples;
    bool   topology_auto;
    /* Tool paging: budget ratio for adaptive tool set sizing */
    float  tool_budget_ratio;  /* 0.0–1.0, 1.0 = full budget, updated each turn */
    /* Pinned context: injected as first user turn every request */
    char   pin_text[1024];
    /* Output token auto-escalation (Phase 4: Claude Code methodology) */
    int    max_output_override;      /* 0 = use default, >0 = escalated limit */
    int    max_output_recovery_count; /* recovery attempts after escalation */
    /* Memory context injection (Phase 3: Claude Code methodology) */
    char   memory_context[4096];     /* recalled memories, injected into system prompt */
    /* Workspace slot */
    char   slot_name[64];            /* active named slot, empty = default */
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
void  conv_add_tool_result_named(conversation_t *c, const char *tool_id,
                                 const char *tool_name,
                                 const char *result, bool is_error);
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
bool  conv_pop_last_turn(conversation_t *c);
void  conv_ensure_tool_results(conversation_t *c);
void  conv_trim_old_results(conversation_t *c, int keep_recent, int max_chars);
bool  conv_compact_recent_tool_turn(conversation_t *c, int max_chars, int protect_tail);

/* ── Multi-tier compaction (inspired by Claude Code) ─────────────────── */

typedef enum {
    COMPACT_MICRO,      /* per-result truncation (existing behavior) */
    COMPACT_SNIP,       /* drop middle API rounds, keep head+tail */
    COMPACT_SESSION,    /* LLM-generated summary of dropped turns */
    COMPACT_FULL,       /* aggressive LLM rewrite + post-compact restore */
} compact_tier_t;

typedef struct {
    compact_tier_t tier;
    int max_result_chars;         /* micro: per-result char cap */
    int snip_keep_head;           /* snip: messages to keep at start */
    int snip_keep_tail;           /* snip: messages to keep at end */
    int session_min_tokens;       /* session: minimum tokens before trigger */
    int session_max_tokens;       /* session: target token count after compact */
    int full_restore_files;       /* full: max files to re-inject */
    int full_restore_budget;      /* full: token budget for restoration */
    int consecutive_failures;     /* circuit breaker counter */
    int max_failures;             /* circuit breaker limit */
} compact_config_t;

typedef struct {
    int pre_token_count;
    int post_token_count;
    compact_tier_t tier_used;
    double duration_ms;
    int messages_removed;
    int messages_kept;
} compact_result_t;

/* Default configuration */
void compact_config_init(compact_config_t *cfg);

/* ── API round grouping ──────────────────────────────────────────────── */

#define MAX_API_ROUNDS 256

typedef struct {
    int start_idx;        /* first message index in this round */
    int end_idx;          /* last message index (inclusive) */
    int token_estimate;   /* rough token estimate for this round */
    bool has_tool_use;    /* whether this round included tool use */
    double timestamp;     /* when this round was created */
} api_round_t;

/* Build a round index from conversation. Returns number of rounds. */
int  conv_build_rounds(conversation_t *c, api_round_t *rounds, int max_rounds);

/* Drop N oldest rounds while maintaining valid message structure. */
void conv_drop_rounds(conversation_t *c, api_round_t *rounds,
                      int n_drop, int total_rounds);

/* ── Token estimation (no API call) ──────────────────────────────────── */

/* Fast local estimate: ~4 chars per token, no network round-trip */
static inline int rough_token_estimate(const char *text) {
    return text ? ((int)strlen(text) + 3) / 4 : 0;
}

static inline int rough_token_estimate_len(const char *text, int len) {
    (void)text;
    return len > 0 ? (len + 3) / 4 : 0;
}

/* Per-image token cost. The API does NOT bill base64 byte length — it
 * downscales any image so the long edge is <=1568px and tokenizes at
 * ~(w*h)/750, which caps a full-screen capture near ~1.6k tokens. Counting
 * base64 length (e.g. strlen/6) overcounts a 1080p screenshot by ~200x and
 * was driving phantom "342% of context" auto-compact loops in computer-use. */
#define IMAGE_TOKEN_ESTIMATE  1600

/* Rough estimate of just the conversation's content (no system prompt,
 * no tool schemas, no cache prefix). Use this to calibrate the
 * non_conv_overhead after an API response reports its true input count. */
int  conv_rough_estimate(conversation_t *c);

/* Estimate total context usage = rough(conv) + non_conv overhead.
 * Reflects what the next API request will roughly look like. */
int  conv_token_estimate(conversation_t *c, session_state_t *s);

/* Output-aware effective context window (subtracts max_output reservation) */
int  effective_context_window(session_state_t *s);

/* Auto-compact threshold (effective window minus safety buffer) */
int  auto_compact_threshold(session_state_t *s);

/* ── Post-compact file restoration ───────────────────────────────────── */

#define POST_COMPACT_MAX_FILES       5
#define POST_COMPACT_TOKEN_BUDGET    50000
#define POST_COMPACT_PER_FILE_CAP    5000

typedef struct {
    char  path[512];
    char *content;          /* truncated to per-file cap */
    int   tokens;
    double last_read_time;
} restored_file_t;

typedef struct {
    restored_file_t files[POST_COMPACT_MAX_FILES + 3]; /* +3 headroom */
    int count;
    int total_tokens;
} post_compact_restore_t;

void  post_compact_restore_init(post_compact_restore_t *r);
void  post_compact_restore_free(post_compact_restore_t *r);
void  post_compact_restore_track(post_compact_restore_t *r,
                                  const char *path, const char *content);
void  post_compact_restore_inject(post_compact_restore_t *r,
                                   conversation_t *c);

/* ── Image/binary stripping ──────────────────────────────────────────── */

/* Strip image_data and doc_data from messages older than keep_recent */
void  conv_strip_binaries(conversation_t *c, int keep_recent);

/* ── Tiered auto-compact pipeline ────────────────────────────────────── */

/* Run the full tiered compaction pipeline. Tries micro → snip → session.
 * Returns result with tier_used and token counts. */
compact_result_t conv_auto_compact(conversation_t *c, session_state_t *s,
                                    compact_config_t *cfg);

/* ── Deferred tool schemas ───────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *one_line_desc;
    const char *group;
    bool schema_loaded;
} deferred_tool_t;

/* Build a compact "menu" of deferred (non-paged) tools.
 * Returns malloc'd string listing name + one-liner only. Caller frees. */
char *tools_build_deferred_catalog(const char **paged_names, int paged_count,
                                    int *out_deferred_count);

bool  conv_save(conversation_t *c, const char *path);
bool  conv_load(conversation_t *c, const char *path);
bool  conv_save_ex(conversation_t *c, const session_state_t *session, const char *path);
bool  conv_load_ex(conversation_t *c, session_state_t *session, const char *path);

char *llm_build_request(conversation_t *c, const char *model, int max_tokens);
char *llm_build_request_ex(conversation_t *c, session_state_t *session, int max_tokens);
char *llm_build_request_for_credential(conversation_t *c, const char *model,
                                       int max_tokens, const char *credential);
char *llm_build_request_ex_for_credential(conversation_t *c,
                                          session_state_t *session,
                                          int max_tokens,
                                          const char *credential);

int   llm_count_tokens(const char *api_key, const char *request_json);
const char *llm_get_custom_system_prompt(void);
void  llm_debug_save_request(const char *request_json, int http_status);
bool  llm_anthropic_uses_claude_code_auth(const char *credential);

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
