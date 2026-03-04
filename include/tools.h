#ifndef DSCO_TOOLS_H
#define DSCO_TOOLS_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

/* Tool definition */
typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;
    bool (*execute)(const char *input_json, char *result, size_t result_len);
} tool_def_t;

void             tools_init(void);
const tool_def_t *tools_get_all(int *count);
int              tools_builtin_count(void);
bool             tools_execute(const char *name, const char *input_json,
                               char *result, size_t result_len);
bool             tools_execute_for_tier(const char *name, const char *input_json,
                                        const char *tier,
                                        char *result, size_t result_len);
bool             tools_is_allowed_for_tier(const char *name, const char *tier,
                                           char *reason, size_t reason_len);

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

#endif
