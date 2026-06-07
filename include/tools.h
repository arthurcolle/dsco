#ifndef DSCO_TOOLS_H
#define DSCO_TOOLS_H

#include <stdbool.h>
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
} tool_def_t;

void             tools_init(void);
void             tools_set_runtime_api_key(const char *api_key);
void             tools_set_runtime_model(const char *model);
const char      *tools_runtime_api_key(void);
const char      *tools_runtime_model(void);
/* Context-aware offload: set the model's context window so offload threshold
 * is computed as a ratio of available context, not a fixed byte count. */
void             tools_set_context_window(int tokens);
int              tools_context_window(void);
void             tools_context_turn_begin(void);
swarm_t         *tools_swarm_instance(void);
const tool_def_t *tools_get_all(int *count);
int              tools_builtin_count(void);
bool             tools_execute(const char *name, const char *input_json,
                               char *result, size_t result_len);
bool             tools_execute_for_tier(const char *name, const char *input_json,
                                        const char *tier,
                                        char *result, size_t result_len);
bool             tools_is_allowed_for_tier(const char *name, const char *tier,
                                           char *reason, size_t reason_len);

/* ── VM dispatch registration (§3: bytecode VM) ───────────────────────── */

void             tools_register_vm_dispatch(vm_t *vm);

/* ── Tool hash map for O(1) lookup ─────────────────────────────────────── */

#define TOOL_MAP_BUCKETS 256

typedef struct tool_map_entry {
    const char             *name;
    int                     index;    /* index into s_tools[] or negative for MCP/plugin */
    struct tool_map_entry  *next;
} tool_map_entry_t;

typedef struct {
    tool_map_entry_t *buckets[TOOL_MAP_BUCKETS];
    int               count;
} tool_map_t;

void  tool_map_init(tool_map_t *m);
void  tool_map_free(tool_map_t *m);
void  tool_map_insert(tool_map_t *m, const char *name, int index);
int   tool_map_lookup(tool_map_t *m, const char *name);  /* returns index or -1 */

/* Global tool map — initialized in tools_init() */
extern tool_map_t g_tool_map;

/* ── MCP tool registration ─────────────────────────────────────────────── */

/* Register an external tool (e.g., from MCP) that will be included in
   tool listings and available for execution via a custom callback.
   The callback receives the tool name and input JSON, returns result. */
typedef char *(*external_tool_cb)(const char *name, const char *input_json, void *ctx);

void  tools_register_external(const char *name, const char *description,
                                const char *input_schema_json,
                                external_tool_cb cb, void *ctx);

#define MAX_EXTERNAL_TOOLS 128

typedef struct {
    char   name[128];
    char   description[512];
    char  *input_schema_json;
    external_tool_cb cb;
    void  *ctx;
} external_tool_t;

extern external_tool_t g_external_tools[];
extern int             g_external_tool_count;

/* ── Concurrency locks ────────────────────────────────────────────────── */

typedef struct {
    pthread_rwlock_t  ctx_lock;       /* context store mutations */
    pthread_rwlock_t  mcp_lock;       /* g_mcp access */
    pthread_rwlock_t  provider_lock;  /* provider access */
    pthread_rwlock_t  toolmap_lock;   /* tool map lookups */
    pthread_mutex_t   metrics_lock;   /* tool_metrics_record */
    pthread_mutex_t   cache_lock;     /* tool_cache get/put */
    pthread_mutex_t   budget_lock;    /* cost budget check */
    pthread_mutex_t   swarm_lock;     /* swarm operations */
} dsco_locks_t;

void dsco_locks_init(dsco_locks_t *l);
void dsco_locks_destroy(dsco_locks_t *l);

/* Global lock instance */
extern dsco_locks_t g_locks;

/* ── Tool execution watchdog ──────────────────────────────────────────── */

typedef struct {
    pthread_t    thread;
    volatile int cancelled;    /* set by watchdog_stop to terminate watcher */
    volatile int timed_out;    /* set by watcher when deadline expires */
    double       deadline;     /* absolute epoch time */
    double       grace_end;    /* deadline + grace period */
    pthread_t    target;       /* thread to cancel on hard kill */
    char         tool_name[64];
    int          timeout_s;
} tool_watchdog_t;

void watchdog_start(tool_watchdog_t *wd, pthread_t target,
                    const char *name, int timeout_s);
void watchdog_stop(tool_watchdog_t *wd);

/* Cooperative cancel flag for long-running tools (e.g., bash poll loop) */
extern _Thread_local volatile int tl_tool_cancelled;
/* Shared flag set by watchdog thread when tool times out */
extern volatile int g_tool_timed_out;

/* Default and per-tool timeout configuration */
#define TOOL_DEFAULT_TIMEOUT_S  30
#define TOOL_GRACE_PERIOD_S     5

typedef struct {
    const char *name;
    int         timeout_s;
} tool_timeout_cfg_t;

/* Lookup per-tool timeout (returns default if not overridden) */
int tool_timeout_for(const char *name);

/* JSON schema validation before tool dispatch */
bool tools_validate_input(const char *name, const char *input_json,
                          char *error_buf, size_t error_len);

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
    HINT_USER   = 0,   /* explicit: /hint trading */
    HINT_CONV   = 1,   /* extracted from conversation context */
    HINT_PLAN   = 2,   /* from OODA/plan phase */
    HINT_TOOL   = 3,   /* co-occurrence: tool X implies tool Y */
    HINT_SWARM  = 4,   /* subagent broadcasts specialization */
} hint_source_t;

#define HINT_MAX_TOOLS   8
#define HINT_MAX_GROUPS  4
#define MAX_HINTS       32
#define HINT_DEFAULT_TTL 5

typedef struct {
    char          domain[64];
    char          tools[HINT_MAX_TOOLS][64];
    int           tool_count;
    int           groups[HINT_MAX_GROUPS];
    int           group_count;
    float         weight;
    int           ttl_turns;
    hint_source_t source;
    int           turn_created;
} tool_hint_t;

/* Hint accumulator (module-level state in tools.c) */
void  tools_hint_init(void);
void  tools_hint_add(const tool_hint_t *h);
void  tools_hint_add_user(const char *input);
void  tools_hint_decay(void);
void  tools_hint_clear(void);
int   tools_hint_count(void);

/* Co-occurrence matrix — tracks tool-tool succession patterns */
void  tools_cooc_init(void);
void  tools_cooc_update(const char **tool_names, int n);
void  tools_cooc_persist(void);
void  tools_cooc_load(void);
void  tools_cooc_free(void);

/* Tiered retrieval result */
typedef struct {
    const tool_def_t **pinned;      /* Tier 0: stable, cacheable */
    int pinned_count;
    const tool_def_t **working;     /* Tier 1: slow-evolving */
    int working_count;
    const tool_def_t **discovery;   /* Tier 2: volatile per-turn */
    int discovery_count;
} tool_page_result_t;

/* Per-turn paging telemetry */
typedef struct {
    int    pinned_count;
    int    working_count;
    int    discovery_count;
    int    hint_count;
    int    cooc_predictions;      /* tools added via co-occurrence */
    int    centroid_matches;      /* tools added via embedding match */
    float  budget_ratio;
    double retrieval_ms;          /* wall-clock time for tools_get_paged */
    int    schema_tokens_saved;   /* estimated tokens saved by progressive schema */
} page_telemetry_t;

/* Last paging telemetry (readable after tools_get_paged call) */
extern page_telemetry_t g_page_telemetry;

/* Retrieve tools in three tiers for cache-aware serialization.
 * budget_ratio: 0.0–1.0, fraction of budget remaining.
 * Low ratios reduce tool set aggressively. */
tool_page_result_t tools_get_paged(const char *context, int max_tools,
                                    float budget_ratio);
void tool_page_result_free(tool_page_result_t *r);

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
    int    candidates_scored;   /* total candidates evaluated */
    int    quorum_admitted;     /* passed quorum (>= QUORUM_MIN_SIGNALS) */
    int    quorum_vetoed;       /* failed quorum */
    int    signal_hot;          /* candidates with hot-cache signal */
    int    signal_cooc;         /* candidates with co-occurrence signal */
    int    signal_embed;        /* candidates with embedding signal */
    int    signal_hint;         /* candidates with hint-group signal */
    double quorum_ms;           /* wall-clock for quorum scoring */
} quorum_telemetry_t;

extern quorum_telemetry_t g_quorum_telemetry;

/* API-based quorum gate (opt-in: DSCO_QUORUM_GATE=1).
 * Fires a cheap model call to pre-filter tool groups before the main
 * API request. Results injected as HINT_PLAN hints. */
void tool_quorum_gate_api(const char *context, const char *api_key);

/* ── Progressive schema: name+description only (no input_schema) ───── */

/* Returns true if this tool should use compact schema (Tier 2 discovery).
 * Compact = name + description only, saving ~200 tokens per tool. */
bool tool_is_progressive_schema(const tool_def_t *t, const tool_page_result_t *r);

#endif
