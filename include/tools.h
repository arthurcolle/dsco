#ifndef DSCO_TOOLS_H
#define DSCO_TOOLS_H

#include <stdbool.h>
#include "env_config.h"
#include <stddef.h>
#include <pthread.h>
#include "swarm.h"
#include "vm.h"

/* Tool definition */
typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;
    bool (*execute)(const char *input_json, char *result, size_t result_len);
    bool core;           /* true = always pageable; false = only via load_tools */
    bool is_read_only;   /* true = no side effects (safe for streaming exec) */
    bool is_concurrent;  /* true = no shared state (safe for parallel exec) */
    bool is_interactive; /* true = owns the terminal/user turn; no cache/spinner/parallel */
    const char *output_schema_json;
} tool_def_t;

typedef enum {
    TOOLS_CORE = 0,
    TOOLS_AGENT,
    TOOLS_FULL,
    /* Fixed builtin allowlist: Bash, curl_raw, and ssh_command only. */
    TOOLS_RESTRICTED,
} tools_init_profile_t;

void tools_init_profile(tools_init_profile_t profile);
void tools_init(void);
/* Local-only fast init for metadata and direct tool execution paths.
 * Skips plugin, browser profile, IPC, MCP, VFS, and daemon-facing setup. */
void tools_init_local_only(void);
tools_init_profile_t tools_current_profile(void);
bool tools_profile_allows_index(int index);
/* §8: VFS-backed tool result cache for deterministic tools */
struct vfs_db;
void tools_set_vfs(struct vfs_db *vfs);
void tools_set_runtime_api_key(const char *api_key);
void tools_set_runtime_model(const char *model);
const char *tools_runtime_api_key(void);
const char *tools_runtime_model(void);
/* self_exit is disabled during normal conversational turns. It may be enabled
 * only for explicit autonomous goal/supervisor runs. */
void tools_set_self_exit_allowed(bool allowed);
bool tools_self_exit_allowed(void);
/* Context-aware offload: set the model's context window so offload threshold
 * is computed as a ratio of available context, not a fixed byte count. */
void tools_set_context_window(int tokens);
int tools_context_window(void);
/* Pass current token usage so inline budget is based on remaining context */
void tools_set_context_usage(int input_tokens, int output_tokens);
/* Pass actual serialized tool-schema overhead from the latest request. */
void tools_set_tool_schema_usage(int active_tools, int schema_tokens);
/* Toggle inline tool-result truncation. Off = full output (human/raw dumps). */
void tools_set_inline_truncation(bool enabled);
/* Per-thread trace context for tool internals that emit mechanism events. */
void tools_set_trace_context(const char *trace_id, const char *chronicle_parent_span_id,
                             const char *chronicle_tool_span_id, const char *tool_name);
void tools_clear_trace_context(void);
void tools_context_turn_begin(void);
swarm_t *tools_swarm_instance(void);
const tool_def_t *tools_get_all(int *count);
bool tools_invoke_by_name(const char *name, const char *input, char *result, size_t rlen);
bool tools_is_offload_safe(const char *name);
/* Report a builtin tool's declared read-only flag; *found = registered. */
bool tools_meta_is_read_only(const char *name, bool *found);
int tools_get_core_count(void); /* only .core=true tools */
int tools_builtin_count(void);
bool tools_execute(const char *name, const char *input_json, char *result, size_t result_len);
#ifdef DSCO_INTERNAL_TESTS
bool tools_execute_raw_for_test(const char *name, const char *input_json, char *result,
                                size_t result_len);
#endif
bool tools_execute_for_tier(const char *name, const char *input_json, const char *tier,
                            char *result, size_t result_len);
/* Governance-model A/B experiment counters: how many times the governance gate
   ran, how many times it was bypassed (DSCO_GOV_MODEL=none), and cumulative
   gate latency in ms. Lets a harness measure governance overhead empirically. */
void tools_governance_experiment_stats(unsigned long *gate_calls,
                                       unsigned long *bypassed,
                                       double *gate_ms_total);
bool tools_is_allowed_for_tier(const char *name, const char *tier, char *reason, size_t reason_len);
char *tools_normalize_input(const char *name, const char *input_json);

/* Run the interactive AskUserQuestion dialog from a JSON spec. Shared by the
 * AskUserQuestion tool and the /dialog chat command so both paths use one
 * engine + serializer. `result` receives the JSON answer object
 * ({status, answers:[...]}). Returns true on success. */
bool dsco_run_ask_dialog(const char *spec_json, char *result, size_t result_len);
bool dsco_tool_is_interactive(const char *name);

/* ── Live agent loop constructs ──────────────────────────────────────── */

#define DSCO_LOOP_PROMPT_MAX 1024
#define DSCO_LOOP_REASON_MAX 256

typedef struct {
    bool force_continue;     /* inject prompt and keep the agent turn alive */
    bool force_done;         /* explicit construct break/complete */
    int effective_max_turns; /* max-turn override; 0 means unchanged */
    char prompt[DSCO_LOOP_PROMPT_MAX];
    char reason[DSCO_LOOP_REASON_MAX];
} loop_control_decision_t;

void tools_loop_control_reset(void);
bool tools_loop_control_has_active(void);
int tools_loop_control_effective_max_turns(int default_max_turns);
void tools_loop_control_decide(int current_turn, bool model_done, bool has_followup,
                               loop_control_decision_t *out);

/* ── VM dispatch registration (§3: bytecode VM) ───────────────────────── */

void tools_register_vm_dispatch(vm_t *vm);

/* ── Tool hash map for O(1) lookup ─────────────────────────────────────── */

#define TOOL_MAP_BUCKETS 256

typedef struct tool_map_entry {
    const char *name;
    int index; /* index into s_tools[] or negative for MCP/plugin */
    struct tool_map_entry *next;
} tool_map_entry_t;

typedef struct {
    tool_map_entry_t *buckets[TOOL_MAP_BUCKETS];
    int count;
} tool_map_t;

void tool_map_init(tool_map_t *m);
void tool_map_free(tool_map_t *m);
void tool_map_insert(tool_map_t *m, const char *name, int index);
int tool_map_lookup(tool_map_t *m, const char *name); /* returns index or -1 */
int tools_lookup_index(const char *name);             /* locked lookup in global tool registry */

#ifdef DSCO_INTERNAL_TESTS
/* Test-only inspection of the global tool map. Production code must use the
 * locked registry APIs below. */
extern tool_map_t g_tool_map;
#endif
void tools_registry_map_free(void);

/* ── MCP tool registration ─────────────────────────────────────────────── */

/* Register an external tool (e.g., from MCP) that will be included in
   tool listings and available for execution via a custom callback.
   The callback receives the tool name and input JSON, returns result. */
typedef char *(*external_tool_cb)(const char *name, const char *input_json, void *ctx);

void tools_register_external(const char *name, const char *description,
                             const char *input_schema_json, external_tool_cb cb, void *ctx);
void tools_register_external_with_output(const char *name, const char *description,
                                         const char *input_schema_json,
                                         const char *output_schema_json,
                                         external_tool_cb cb, void *ctx);
void tools_register_external_metadata(const char *name, const char *integration_id,
                                      const char *display_name, const char *distribution_channel,
                                      const char *categories, const char *labels, const char *scope,
                                      unsigned action_flags, const char *catalog_status);
void tools_reset_external(void);
const char *tools_default_input_schema_json(void);
const char *tools_default_output_schema_json(void);
const char *tools_output_schema_for_def(const tool_def_t *tool);
const char *tools_output_schema_for_name(const char *name);

#define MAX_EXTERNAL_TOOLS 4096

typedef struct {
    char name[256];
    char description[1024];
    char *input_schema_json;
    char *output_schema_json;
    external_tool_cb cb;
    void *ctx;
    bool loaded;
    char integration_id[256];
    char display_name[256];
    char distribution_channel[64];
    char categories[256];
    char labels[256];
    char scope[64];
    unsigned action_flags;
    char catalog_status[64];
} external_tool_t;

#ifdef DSCO_INTERNAL_TESTS
extern external_tool_t g_external_tools[];
extern int g_external_tool_count;
#endif

typedef struct {
    external_tool_t *items;
    int count;
} external_tool_snapshot_t;

int tools_external_count(void);
external_tool_snapshot_t tools_external_snapshot(void);
void tools_external_snapshot_free(external_tool_snapshot_t *snapshot);
int tools_rank_external_snapshot(const external_tool_snapshot_t *snapshot, const char *context,
                                 int *out_indices, int max_indices);

/* Unified capability retrieval across builtin and external/MCP contracts.
 * Coarse recall is local BM25/TF-IDF over full contracts; when JINA_API_KEY is
 * present, the candidate pool is cross-encoder reranked by jina-reranker-v3. */
typedef struct {
    char name[256];
    char source[16]; /* builtin | mcp */
    int index;
    double recall_score;
    double rerank_score;
    bool loaded;
} tool_retrieval_hit_t;

int tools_retrieve_capabilities(const char *query, int candidate_limit,
                                tool_retrieval_hit_t *out_hits, int max_hits,
                                bool *out_reranked);

/* ── Concurrency locks ────────────────────────────────────────────────── */

typedef struct {
    pthread_rwlock_t ctx_lock;      /* context store mutations */
    pthread_rwlock_t mcp_lock;      /* g_mcp access */
    pthread_rwlock_t provider_lock; /* provider access */
    pthread_rwlock_t toolmap_lock;  /* tool map lookups */
    pthread_mutex_t metrics_lock;   /* tool_metrics_record */
    pthread_mutex_t cache_lock;     /* tool_cache get/put */
    pthread_mutex_t budget_lock;    /* cost budget check */
    pthread_mutex_t swarm_lock;     /* swarm operations */
} dsco_locks_t;

void dsco_locks_init(dsco_locks_t *l);
void dsco_locks_destroy(dsco_locks_t *l);

/* Global lock instance */
extern dsco_locks_t g_locks;

/* ── Tool execution watchdog ──────────────────────────────────────────── */

typedef struct {
    pthread_t thread;
    volatile int cancelled;   /* set by watchdog_stop to terminate watcher */
    volatile int timed_out;   /* set by watcher when deadline expires */
    volatile double deadline; /* absolute epoch time; renewable via watchdog_renew */
    volatile double grace_end; /* deadline + grace period */
    pthread_t target;         /* thread to cancel on hard kill */
    char tool_name[64];
    int timeout_s;
    /* Renewable-lifetime fields — managed by the process-lifecycle supervisor.
       The watchdog registers itself on start so a supervisor thread can reach
       an in-flight call by name/id to extend or inspect its deadline. */
    double started_at;        /* absolute epoch time watchdog_start ran */
    int max_lifetime_s;       /* absolute cap across renewals; 0 = unlimited */
    volatile int renew_count; /* times the deadline has been extended */
    unsigned long call_id;    /* monotonic id for registry lookup */
} tool_watchdog_t;

void watchdog_start(tool_watchdog_t *wd, pthread_t target, const char *name, int timeout_s);
void watchdog_stop(tool_watchdog_t *wd);

/* Renewable deadlines. watchdog_renew extends an in-flight watchdog's deadline
   to (max(now, deadline) + extra_s), never shortening it, clearing a pending
   soft-timeout if the grace window has not yet elapsed, and honoring
   max_lifetime_s when set. Returns 1 if renewed, 0 if the call already ended /
   passed grace / hit its absolute cap. watchdog_renew_by_name renews every
   active watchdog whose tool_name matches and returns the count renewed. */
int watchdog_renew(tool_watchdog_t *wd, int extra_s);
int watchdog_renew_by_name(const char *name, int extra_s);

/* Supervisory snapshot of an in-flight tool watchdog. */
typedef struct {
    unsigned long call_id;
    char tool_name[64];
    double remaining_s; /* deadline - now (negative once inside grace) */
    int timeout_s;
    int renew_count;
    int timed_out;
} watchdog_info_t;
/* Fill out[] with up to max active watchdogs; returns the number written. */
int watchdog_active_snapshot(watchdog_info_t *out, int max);

/* Cooperative cancel flag for long-running tools (e.g., bash poll loop) */
extern _Thread_local volatile int tl_tool_cancelled;
/* Shared flag set by watchdog thread when tool times out */
extern volatile int g_tool_timed_out;

/* Default and per-tool timeout configuration */
#define TOOL_DEFAULT_TIMEOUT_S 30
#define TOOL_GRACE_PERIOD_S 5

static inline int dsco_tool_default_timeout_s(void) {
    return dsco_env_int("DSCO_TOOL_DEFAULT_TIMEOUT", TOOL_DEFAULT_TIMEOUT_S, 1, 7200);
}
static inline int dsco_tool_grace_period_s(void) {
    return dsco_env_int("DSCO_TOOL_GRACE_PERIOD_S", TOOL_GRACE_PERIOD_S, 0, 300);
}

typedef struct {
    const char *name;
    int timeout_s;
} tool_timeout_cfg_t;

/* Lookup per-tool timeout (returns default if not overridden) */
int tool_timeout_for(const char *name);

/* JSON schema validation before tool dispatch */
bool tools_validate_input(const char *name, const char *input_json, char *error_buf,
                          size_t error_len);

/* ── Tool retrieval: context-aware subset selection ─────────────────── */

/* Score and select tools relevant to the conversation context.
 * Returns indices into s_tools[] array sorted by relevance.
 * `context` is the last user message or task description.
 * `max_tools` caps the output. Always includes core tools.
 * Returns number of tools selected. */
int tools_retrieve(const char *context, int *out_indices, int max_tools);

/* Get a filtered subset of tools based on context. Returns a malloc'd
 * array of tool_def_t pointers. Caller frees the array (not the tools). */
const tool_def_t **tools_get_filtered(const char *context, int max_tools, int *out_count);

/* ── Dynamic Tool Paging ─────────────────────────────────────────────── */

typedef enum {
    HINT_USER = 0,  /* explicit: /hint trading */
    HINT_CONV = 1,  /* extracted from conversation context */
    HINT_PLAN = 2,  /* from OODA/plan phase */
    HINT_TOOL = 3,  /* co-occurrence: tool X implies tool Y */
    HINT_SWARM = 4, /* subagent broadcasts specialization */
} hint_source_t;

#define HINT_MAX_TOOLS 8
#define HINT_MAX_GROUPS 4
#define MAX_HINTS 32
#define HINT_DEFAULT_TTL 5

typedef struct {
    char domain[64];
    char tools[HINT_MAX_TOOLS][128];
    int tool_count;
    int groups[HINT_MAX_GROUPS];
    int group_count;
    float weight;
    int ttl_turns;
    hint_source_t source;
    int turn_created;
} tool_hint_t;

/* Hint accumulator (module-level state in tools.c) */
void tools_hint_init(void);
void tools_hint_add(const tool_hint_t *h);
void tools_hint_add_user(const char *input);
void tools_hint_decay(void);
void tools_hint_clear(void);
int tools_hint_count(void);

/* Co-occurrence matrix — tracks tool-tool succession patterns */
void tools_cooc_init(void);
void tools_cooc_update(const char **tool_names, int n);
void tools_cooc_persist(void);
void tools_cooc_load(void);
void tools_cooc_free(void);

typedef struct {
    char from[64];
    char to[64];
    unsigned count;
} tools_cooc_edge_t;

int tools_cooc_top_edges(tools_cooc_edge_t *out, int max);

/* Tiered retrieval result */
typedef struct {
    const tool_def_t **pinned; /* Tier 0: stable, cacheable */
    int pinned_count;
    const tool_def_t **working; /* Tier 1: slow-evolving */
    int working_count;
    const tool_def_t **discovery; /* Tier 2: volatile per-turn */
    int discovery_count;
} tool_page_result_t;

/* Per-turn paging telemetry */
typedef struct {
    int pinned_count;
    int working_count;
    int discovery_count;
    int hint_count;
    int cooc_predictions; /* tools added via co-occurrence */
    int centroid_matches; /* tools added via embedding match */
    float budget_ratio;
    double retrieval_ms;     /* wall-clock time for tools_get_paged */
    int schema_tokens_saved; /* estimated tokens saved by progressive schema */
} page_telemetry_t;

/* Last paging telemetry (readable after tools_get_paged call) */
extern page_telemetry_t g_page_telemetry;

/* Retrieve tools in three tiers for cache-aware serialization.
 * budget_ratio: 0.0–1.0, fraction of budget remaining.
 * Low ratios reduce tool set aggressively. */
tool_page_result_t tools_get_paged(const char *context, int max_tools, float budget_ratio);
void tool_page_result_free(tool_page_result_t *r);

/* Explicit dynamic schema bank populated by load_tools and drained by evict_tools.
 * These are serialized after the frozen core register so the stable prefix stays
 * cacheable while newly loaded capabilities become callable on the next turn. */
int tools_loaded_builtin_indices(int *out_indices, int max_indices);
int tools_loaded_builtin_count(void);
bool tools_is_builtin_loaded(const char *name);
void tools_loaded_builtin_clear(void);

/* ── Co-occurrence → Hint bridge ─────────────────────────────────────── */

/* After tool execution, predict successors from co-occurrence matrix
 * and inject them as HINT_TOOL hints for the next turn. */
void tools_cooc_inject_hints(const char **tool_names, int n);

/* ── Co-occurrence temporal decay ────────────────────────────────────── */

/* Apply global decay (multiply all counters by factor, e.g. 0.95).
 * Call periodically (e.g. every 10 turns) to forget stale patterns. */
void tools_cooc_decay(float factor);

/* ── Register-file quorum telemetry ───────────────────────────────────── */

typedef struct {
    int candidates_scored; /* total candidates evaluated */
    int quorum_admitted;   /* passed quorum (>= QUORUM_MIN_SIGNALS) */
    int quorum_vetoed;     /* failed quorum */
    int signal_hot;        /* candidates with hot-cache signal */
    int signal_cooc;       /* candidates with co-occurrence signal */
    int signal_embed;      /* candidates with embedding signal */
    int signal_hint;       /* candidates with hint-group signal */
    double quorum_ms;      /* wall-clock for quorum scoring */
} quorum_telemetry_t;

extern quorum_telemetry_t g_quorum_telemetry;

/* API-based quorum gate (opt-in: DSCO_QUORUM_GATE=1).
 * Fires a cheap model call to pre-filter tool groups before the main
 * API request. Results injected as HINT_PLAN hints. */
void tool_quorum_gate_api(const char *context, const char *api_key);

/* ── Compact tool catalog for system prompt ─────────────────────────── */

/* Build a compact text catalog of all tools with signatures.
 * Returns a malloc'd string (caller frees). Thread-safe after tools_init(). */
char *tools_build_compact_catalog(void);

/* ── Progressive schema: name+description only (no input_schema) ───── */

/* Returns true if this tool should use compact schema (Tier 2 discovery).
 * Compact = name + description only, saving ~200 tokens per tool. */
bool tool_is_progressive_schema(const tool_def_t *t, const tool_page_result_t *r);

/* ── ACE Playbook + Context Management ──────────────────────────────── */

/* Wire the active conversation for context_compact (takes void* to avoid llm.h dep) */
void tools_set_active_conversation(void *conv);

/* Advance the playbook turn counter (call once per agent turn) */
void tools_playbook_advance_turn(void);

/* ── Agent profile tool filter ───────────────────────────────────────── */

/* Callback: returns true if tool_name is allowed by the active agent profile.
 * group_hint may be NULL. Set to NULL to disable filtering. */
typedef bool (*tool_profile_filter_fn_t)(const char *tool_name, const char *group_hint);

void tools_set_profile_filter(tool_profile_filter_fn_t fn);
void tools_clear_profile_filter(void);
bool tools_is_parent_specified_core_tool(const char *tool_name);

/* ── Safe subprocess exec ────────────────────────────────────────────── */

/* fork()+execvp() without a shell — eliminates command injection. argv must be
 * NULL-terminated. Captures stdout+stderr to out. Returns exit status (0-255)
 * or -1 on error. */
int safe_exec_argv(const char *const argv[], char *out, size_t out_len);

/* ── Embedding API ─────────────────────────────────────────────────── */

/* Embed text via Jina v4 API. Returns malloc'd float[*out_dim] or NULL.
 * Caller frees. Returns NULL if JINA_API_KEY is not set. */
float *tools_embed_text(const char *text, int *out_dim);

/* Set agent context for context-aware tool retrieval.
 * Both params may be NULL to clear. Strings are copied internally. */
void tools_set_agent_context(const char *recent_results, const char *working_memory_summary);

/* ── Process execution ─────────────────────────────────────────────────
 * fork()+execvp() without a shell — no argument is ever interpreted by a
 * shell, eliminating command injection. Returns the child exit status (0 on
 * success); stderr (truncated) is written to `out`. Shared by trading.c and
 * integrations.c for their curl/openssl/pdftotext invocations. */
int safe_exec_argv(const char *const argv[], char *out, size_t out_len);

#ifdef DSCO_INTERNAL_TESTS
/* Test hook: JSON structure-aware tool-result truncation. */
size_t tools_test_truncate_json(const char *json, size_t json_len, char *out, size_t out_len);
#endif

#endif
