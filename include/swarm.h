#ifndef DSCO_SWARM_H
#define DSCO_SWARM_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include "env_config.h"

/* ── Sub-dsco process handle ──────────────────────────────────────────── */

#define SWARM_DEFAULT_MAX_CHILDREN 256 /* enough recovery headroom for 37+ logical workers */
#define SWARM_MAX_CHILDREN  1024 /* structural array/bitset cap */
#define SWARM_BITSET_WORDS ((SWARM_MAX_CHILDREN + 63) / 64)
#define SWARM_MAX_GROUPS    16  /* structural array cap; not env-resizable */
#define SWARM_MAX_OUTPUT    (512 * 1024)
#define SWARM_LABEL_LEN     128
#define SWARM_GROUP_NAME_LEN 64
#define SWARM_MAX_DEPTH     7
#define SWARM_READ_BUF      (64 * 1024) /* 64KB read buffer (was 4KB) */

/* Runtime caps. The default stays conservative, while the static pool can be
 * raised to SWARM_MAX_CHILDREN for high-throughput sessions. */
static inline int dsco_swarm_max_children(void) {
    return dsco_env_int("DSCO_SWARM_MAX_CHILDREN", SWARM_DEFAULT_MAX_CHILDREN, 1, SWARM_MAX_CHILDREN);
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
    EXECUTOR_GROK   = 3,  /* xAI Grok subscription CLI                  */
    EXECUTOR_KIMI   = 4,  /* Moonshot Kimi Code subscription CLI        */
} executor_type_t;

/* Executor availability (detected at init) */
typedef struct {
    bool     claude_available;   /* claude CLI found and authenticated     */
    bool     codex_available;    /* codex CLI found and authenticated      */
    bool     grok_available;     /* Grok CLI found and authenticated       */
    bool     kimi_available;     /* Kimi Code CLI found and authenticated  */
    char     claude_path[512];   /* resolved path to claude binary         */
    char     codex_path[512];    /* resolved path to codex binary          */
    char     grok_path[512];     /* resolved path to grok binary           */
    char     kimi_path[512];     /* resolved path to kimi binary           */
    char     claude_model[128];  /* default claude model (from --version)  */
    char     codex_model[128];   /* default codex model (from config)      */
    char     grok_model[128];    /* default Grok CLI model                  */
    char     kimi_model[128];    /* default Kimi Code model                 */
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

    /* UI emission is rate-limited independently of lossless pipe draining. */
    size_t         ui_bytes_emitted;
    double         ui_last_emit_time;

    /* Timing */
    double         start_time;
    double         end_time;
    int            depth;

    /* Cost tracking */
    double         est_cost_usd;
    double         reserve_cost_usd;   /* conservative admission reservation       */
    double         reserve_confidence; /* 0..1; learned interval confidence         */
    bool           reserve_calibrated; /* true when learned high-side bound exists  */
    double         budget_usd;         /* allocated budget partition (0 = unlimited) */
    int            est_input_tokens;
    int            est_output_tokens;
    double         reported_cost_usd;  /* actual cost parsed from executor output  */
    bool           reported_cost_known;
    bool           estimated_cost_known;
    bool           budget_cost_known;
    double         budget_accounted_usd;
    int            cost_samples;
    int            unpriced_responses;
    int            cost_fd; /* private unlinked accounting stream, never model output */
    off_t          cost_offset;
    bool           cost_transport;
    bool           cost_class_explicit; /* provider fabric supplied billing class */
    bool           scale_sampled;       /* service time already fed to autoscaler */
    bool           subsidized;          /* flat-rate/local: not real-dollar draw   */

    /* Executor */
    executor_type_t executor;          /* which backend spawned this child */
    char           provider[32];       /* native provider name (e.g. "openai", "groq") */

    /* Group membership */
    int            group_id;       /* -1 if ungrouped */

    /* Slot lifecycle: true once this slot's result has been durably
     * collected (via a completed collect()/status() call or the
     * RESULT.json envelope write) AND its process is fully reaped/terminal.
     * Only reclaimable slots may be handed back out by swarm_spawn*(). */
    bool           reclaimable;
} swarm_child_t;

typedef struct {
    int   id;
    char  name[SWARM_GROUP_NAME_LEN];
    int   child_ids[SWARM_MAX_CHILDREN];
    int   child_count;
    char  coordinator_task[SWARM_LABEL_LEN];
    bool  active;

    /* Durable SwarmRun v2: stable identity for this logical group. */
    char  durable_run_id[128];
    char  durable_run_dir[512];
    char  topology[64];
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
    unsigned long long words[SWARM_BITSET_WORDS]; /* 1 bit per child */
    int count;                     /* popcount cache */
} swarm_bitset_t;

typedef struct {
    swarm_child_t  children[SWARM_MAX_CHILDREN];
    int            child_count;
    swarm_group_t  groups[SWARM_MAX_GROUPS];
    int            group_count;

    /* ── Slot recycling ────────────────────────────────────────────────
     * child_count/group_count were historically monotonic allocators: once
     * a session spawned SWARM_MAX_CHILDREN children over its lifetime,
     * every subsequent spawn silently failed forever, even though zero
     * children were actually running (all terminal + already collected).
     * These free-lists let terminal, already-collected slots be reclaimed
     * so a long-lived session can spawn far more than 64 children total
     * without ever exceeding the configured concurrent-child limit. */
    int            free_child_ids[SWARM_MAX_CHILDREN];
    int            free_child_count;
    int            free_group_ids[SWARM_MAX_GROUPS];
    int            free_group_count;

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
    double         retired_spent_usd;
    double         retired_subsidized_usd;
    double         subsidized_usd;    /* notional cost covered by flat-rate plans */

    /* External executor registry */
    executor_registry_t executors;

    /* ── Fast-path data structures ────────────────────────────────────── */
    swarm_completion_q_t done_q;   /* O(1) completion notifications          */
    swarm_bitset_t       active;   /* bitset of running/streaming children   */
    int                  kq_fd;    /* kqueue fd (-1 if unavailable)          */
    double               last_budget_enforcement; /* per-instance poll cadence */
    double               first_completion_time; /* timestamp of first child done */
} swarm_t;

/* Private native-worker accounting transport and presentation. */
int swarm_accounting_open(void);
void swarm_accounting_export(int fd);
void swarm_child_budget_export(double cap);
void swarm_accounting_read(swarm_child_t *child);
double swarm_child_accounted_cost(const swarm_child_t *child);
char *swarm_child_accounting_json(const swarm_child_t *child);

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
                             const char *tool_choice, const char *system_prompt,
                             int max_agent_turns);
void swarm_set_next_structured_output(const char *name, const char *schema_json, bool strict,
                                      int max_repairs);
/* Give the next child an explicit session-dollar ceiling. This is independent
 * of the global swarm budget so callers can reserve heterogeneous lane costs. */
void swarm_set_next_budget_usd(double budget_usd);
void swarm_set_next_max_tokens(int max_tokens);

/* Spawn a sub-dsco forced to a specific native provider (e.g. "openai", "groq").
 * The child process gets --exec <provider> -m <model> so it routes through
 * that provider's API directly, completely decoupled from the parent's provider. */
int  swarm_spawn_provider(swarm_t *s, int group_id, const char *task,
                           const char *model, const char *provider);
/* Credential-class specialization for providers exposing independent billing
 * pools through one endpoint (currently Sakana subscription vs PAYG). */
int swarm_spawn_provider_auth_lane(swarm_t *s, int group_id, const char *task,
                                   const char *model, const char *provider,
                                   const char *auth_class);
/* OpenRouter specialization: pins a concrete upstream provider/quantization
 * for one model lane. Empty upstream preserves normal OpenRouter routing. */
int swarm_spawn_openrouter_lane(swarm_t *s, int group_id, const char *task,
                                const char *model, const char *upstream,
                                const char *quantization);

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
typedef struct {
    int input_tokens;
    int output_tokens;
    double expected_cost_usd;
    double reserved_cost_usd;
    double latency_sec;
    double confidence;
    bool calibrated;
} swarm_cost_reserve_t;
void swarm_estimate_prompt_reserve(swarm_t *s, const char *model, int input_tokens,
                                   int output_tokens, double reserve_multiplier,
                                   swarm_cost_reserve_t *out);
void swarm_enforce_budgets(swarm_t *s);  /* kill over-budget children */

/* ── Groups ───────────────────────────────────────────────────────────── */
int  swarm_group_create(swarm_t *s, const char *name);
int  swarm_group_dispatch(swarm_t *s, int group_id, const char **tasks, int task_count,
                          const char *model);
bool swarm_group_complete(swarm_t *s, int group_id);

/* ── Slot recycling ───────────────────────────────────────────────────────
 * Mark a group's children as reclaimable (their durable RESULT.json envelope
 * is already on disk, or the caller has captured everything it needs) and
 * return the group + child slots to the free-lists for reuse by future
 * spawns. This is what lets a long-lived session (many collect() calls
 * across many groups) spawn far more than SWARM_MAX_CHILDREN children over
 * its lifetime — only *concurrently active* children are structurally
 * capped. Safe to call multiple times; a no-op on an already-reclaimed
 * group. Refuses to reclaim a group with any RUNNING/STREAMING child. */
bool swarm_group_reclaim(swarm_t *s, int group_id);
/* Total children ever spawned (child_count) vs. currently reclaimable slots
 * available for reuse — exposed for diagnostics/observability. */
int  swarm_reclaimable_count(swarm_t *s);
bool swarm_active_test(const swarm_t *s, int child_id);
/* Sweep every active group whose children are ALL terminal (done/error/
 * killed — swarm_group_complete()==true) and reclaim it. Called
 * automatically by swarm_spawn*() when a spawn would otherwise fail due to
 * exhausted child capacity, so a long-lived session self-heals instead of
 * being permanently wedged by groups the caller forgot to retire after
 * collecting. Returns the number of groups reclaimed. */
int  swarm_reclaim_all_complete(swarm_t *s);

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

/* ── Swarm Mode v1 observability/persistence ─────────────────────────────
 * Persist a group as a first-class SwarmRun record in the flat artifact
 * store. The returned out_dir is .swarm, containing latest.json and the
 * append-only runs.jsonl ledger.
 * Returns 0 on success, -1 on validation/IO failure. */
int  swarm_group_persist_run(swarm_t *s, int group_id, const char *run_id,
                             const char *topology, const char *user_prompt,
                             const char *coordinator_output, bool run_complete,
                             const char *reason,
                             char *out_dir, size_t out_dir_len);

/* Render a compact live-observability frame for a group into buf. */
int  swarm_group_render_frame(swarm_t *s, int group_id, const char *run_id,
                              const char *topology, char *buf, size_t len);

/* Ensure a stable durable run id/dir exists for a group. */
int  swarm_group_ensure_durable_run(swarm_t *s, int group_id, const char *topology,
                                    const char *suggested_run_id);

#endif
