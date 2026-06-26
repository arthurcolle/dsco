#ifndef DSCO_SWARM_H
#define DSCO_SWARM_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include "env_config.h"

/* ── Sub-dsco process handle ──────────────────────────────────────────── */

#define SWARM_MAX_CHILDREN  64  /* structural array/bitset cap; not env-resizable */
#define SWARM_MAX_GROUPS    16  /* structural array cap; not env-resizable */
#define SWARM_MAX_OUTPUT    (512 * 1024)
#define SWARM_LABEL_LEN     128
#define SWARM_GROUP_NAME_LEN 64
#define SWARM_MAX_DEPTH     6
#define SWARM_READ_BUF      (64 * 1024) /* 64KB read buffer (was 4KB) */

/* Runtime caps. These cannot exceed the structural compile-time maxima above
 * without changing struct layouts and the 64-bit active-child bitset. */
static inline int dsco_swarm_max_children(void) {
    return dsco_env_int("DSCO_SWARM_MAX_CHILDREN", SWARM_MAX_CHILDREN, 1, SWARM_MAX_CHILDREN);
}
static inline int dsco_swarm_max_groups(void) {
    return dsco_env_int("DSCO_SWARM_MAX_GROUPS", SWARM_MAX_GROUPS, 1, SWARM_MAX_GROUPS);
}
static inline int dsco_swarm_max_depth(void) {
    return dsco_env_int("DSCO_SWARM_MAX_DEPTH", SWARM_MAX_DEPTH, 0, SWARM_MAX_DEPTH);
}
static inline size_t dsco_swarm_max_output(void) {
    return dsco_env_size("DSCO_SWARM_MAX_OUTPUT", SWARM_MAX_OUTPUT, 4096, SWARM_MAX_OUTPUT);
}
static inline size_t dsco_swarm_read_buf(void) {
    return dsco_env_size("DSCO_SWARM_READ_BUF", SWARM_READ_BUF, 1024, SWARM_READ_BUF);
}

typedef enum {
    SWARM_PENDING,
    SWARM_RUNNING,
    SWARM_STREAMING,
    SWARM_DONE,
    SWARM_ERROR,
    SWARM_KILLED,
} swarm_status_t;

/* ── Executor backends ────────────────────────────────────────────────── */

typedef enum {
    EXECUTOR_DSCO   = 0,  /* default: fork dsco binary                  */
    EXECUTOR_CLAUDE = 1,  /* Claude Code CLI: claude -p --output-format json */
    EXECUTOR_CODEX  = 2,  /* OpenAI Codex CLI: codex exec --json        */
} executor_type_t;

/* Executor availability (detected at init) */
typedef struct {
    bool     claude_available;   /* claude CLI found and authenticated     */
    bool     codex_available;    /* codex CLI found and authenticated      */
    char     claude_path[512];   /* resolved path to claude binary         */
    char     codex_path[512];    /* resolved path to codex binary          */
    char     claude_model[128];  /* default claude model (from --version)  */
    char     codex_model[128];   /* default codex model (from config)      */
} executor_registry_t;

typedef void (*swarm_stream_cb)(int child_id, const char *data, size_t len, void *ctx);

typedef struct {
    int            id;
    pid_t          pid;
    int            pipe_fd;        /* read end of child's stdout */
    int            err_fd;         /* read end of child's stderr */
    swarm_status_t status;
    int            exit_code;
    char           task[SWARM_LABEL_LEN];
    char           model[128];

    /* Accumulated output */
    char          *output;
    size_t         output_len;
    size_t         output_cap;

    /* Streaming */
    char          *stream_buf;     /* partial line buffer */
    size_t         stream_buf_len;

    /* Timing */
    double         start_time;
    double         end_time;
    int            depth;

    /* Cost tracking */
    double         est_cost_usd;
    double         budget_usd;         /* allocated budget partition (0 = unlimited) */
    int            est_input_tokens;
    int            est_output_tokens;
    double         reported_cost_usd;  /* actual cost parsed from executor output  */

    /* Executor */
    executor_type_t executor;          /* which backend spawned this child */
    char           provider[32];       /* native provider name (e.g. "openai", "groq") */

    /* Group membership */
    int            group_id;       /* -1 if ungrouped */
} swarm_child_t;

typedef struct {
    int   id;
    char  name[SWARM_GROUP_NAME_LEN];
    int   child_ids[SWARM_MAX_CHILDREN];
    int   child_count;
    char  coordinator_task[SWARM_LABEL_LEN];
    bool  active;
} swarm_group_t;

/* ── Completion queue — O(1) push/pop for finished children ───────────── */

typedef struct {
    int   ids[SWARM_MAX_CHILDREN]; /* ring buffer of completed child IDs */
    int   head;                    /* read pointer  */
    int   tail;                    /* write pointer */
    int   count;                   /* number queued */
} swarm_completion_q_t;

/* ── Active bitset — O(1) membership test, fast iteration ────────────── */

typedef struct {
    unsigned long long bits;       /* 64-bit bitset — 1 bit per child */
    int count;                     /* popcount cache */
} swarm_bitset_t;

typedef struct {
    swarm_child_t  children[SWARM_MAX_CHILDREN];
    int            child_count;
    swarm_group_t  groups[SWARM_MAX_GROUPS];
    int            group_count;

    /* Global stream callback */
    swarm_stream_cb stream_cb;
    void           *stream_ctx;

    /* API config (inherited by children) */
    const char    *api_key;
    const char    *default_model;
    const char    *dsco_path;     /* path to dsco binary */

    /* Budget system. The budget represents REAL credit dollars (OpenRouter /
     * metered API draws). Children running on flat-rate subscriptions (Claude
     * Code / Codex $200-mo plans) are "subsidized": their notional API cost is
     * tracked in subsidized_usd for visibility but does NOT draw the budget. */
    double         swarm_budget_usd;  /* total real-dollar budget (0=unlimited) */
    double         spent_usd;         /* metered real-dollar spend (draws budget) */
    double         subsidized_usd;    /* notional cost covered by flat-rate plans */

    /* External executor registry */
    executor_registry_t executors;

    /* ── Fast-path data structures ────────────────────────────────────── */
    swarm_completion_q_t done_q;   /* O(1) completion notifications          */
    swarm_bitset_t       active;   /* bitset of running/streaming children   */
    int                  kq_fd;    /* kqueue fd (-1 if unavailable)          */
    double               first_completion_time; /* timestamp of first child done */
} swarm_t;

/* ── Lifecycle ────────────────────────────────────────────────────────── */
void swarm_init(swarm_t *s, const char *api_key, const char *model);
void swarm_destroy(swarm_t *s);

/* ── Spawn sub-dsco ───────────────────────────────────────────────────── */
int  swarm_spawn(swarm_t *s, const char *task, const char *model);
int  swarm_spawn_in_group(swarm_t *s, int group_id, const char *task, const char *model);

/* Set the model-instance spec applied to the NEXT swarm_spawn*() child, so a
 * spawned process wraps a fully distinct model instance (not just a model id).
 * The freshly-forked child exports these as DSCO_* env and session_state_init()
 * picks them up. Pass -1 / NULL to leave a field at its default. The spec is
 * consumed (cleared) by the next spawn — set it immediately before spawning. */
void swarm_set_next_instance(const char *effort, double temperature,
                             double top_p, int top_k, int thinking_budget,
                             const char *tool_choice, const char *system_prompt);

/* Spawn a sub-dsco forced to a specific native provider (e.g. "openai", "groq").
 * The child process gets --exec <provider> -m <model> so it routes through
 * that provider's API directly, completely decoupled from the parent's provider. */
int  swarm_spawn_provider(swarm_t *s, int group_id, const char *task,
                           const char *model, const char *provider);

/* ── External executor spawn ─────────────────────────────────────────── */
int  swarm_spawn_executor(swarm_t *s, int group_id, const char *task,
                           const char *model, executor_type_t executor);
void swarm_detect_executors(swarm_t *s);
void swarm_prepare_executor_env(swarm_t *s, executor_type_t executor);
const char *executor_type_name(executor_type_t t);

/* ── Budget partitioning ─────────────────────────────────────────────── */
void swarm_set_budget(swarm_t *s, double budget_usd);
double swarm_budget_remaining(swarm_t *s);
/* True when a child's tokens are covered by a flat-rate subscription (Claude
 * Code / Codex $200-mo plans) and therefore do NOT draw the real-dollar budget.
 * Defaults to the claude+codex executors; override via DSCO_SUBSIDIZED_EXECUTORS. */
bool swarm_child_is_subsidized(const swarm_child_t *c);
double swarm_estimate_task_cost(swarm_t *s, const char *model);
void swarm_enforce_budgets(swarm_t *s);  /* kill over-budget children */

/* ── Groups ───────────────────────────────────────────────────────────── */
int  swarm_group_create(swarm_t *s, const char *name);
int  swarm_group_dispatch(swarm_t *s, int group_id, const char **tasks, int task_count,
                          const char *model);
bool swarm_group_complete(swarm_t *s, int group_id);

/* ── Streaming & polling ──────────────────────────────────────────────── */
/* Poll all children for output. timeout_ms=-1 blocks, 0=nonblock */
int  swarm_poll(swarm_t *s, int timeout_ms);

/* Poll and invoke stream callback for each chunk received */
int  swarm_poll_stream(swarm_t *s, int timeout_ms, swarm_stream_cb cb, void *ctx);

/* ── Fast completion primitives ──────────────────────────────────────── */

/* Wait for ANY child to complete. Returns its ID, or -1 on timeout.
 * Use this for race patterns (first-to-finish wins). */
int  swarm_wait_any(swarm_t *s, int timeout_ms);

/* Wait for the FIRST N children to complete. Fills `out_ids` with their IDs.
 * Returns how many completed within timeout. Use for quorum patterns. */
int  swarm_wait_n(swarm_t *s, int n, int *out_ids, int timeout_ms);

/* Pop next completed child from the completion queue. Returns -1 if empty. */
int  swarm_completion_pop(swarm_t *s);

/* Number of completions queued but not yet consumed. */
int  swarm_completion_pending(swarm_t *s);

/* ── Status & results ─────────────────────────────────────────────────── */
swarm_child_t  *swarm_get(swarm_t *s, int child_id);
const char     *swarm_status_str(swarm_status_t st);
int             swarm_active_count(swarm_t *s);
int             swarm_group_active_count(swarm_t *s, int group_id);
int             swarm_group_done_count(swarm_t *s, int group_id);
int             swarm_group_error_count(swarm_t *s, int group_id);
int             swarm_group_killed_count(swarm_t *s, int group_id);
double          swarm_group_est_cost_usd(swarm_t *s, int group_id);
double          swarm_child_elapsed_sec(const swarm_child_t *c);

/* Kill a child */
bool swarm_kill(swarm_t *s, int child_id);

/* Kill all children in a group */
void swarm_group_kill(swarm_t *s, int group_id);

/* ── Formatted output ─────────────────────────────────────────────────── */
int  swarm_status_json(swarm_t *s, char *buf, size_t len);
int  swarm_child_output(swarm_t *s, int child_id, char *buf, size_t len);
int  swarm_group_status_json(swarm_t *s, int group_id, char *buf, size_t len);

#endif
