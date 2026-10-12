#ifndef DSCO_WORK_UNIT_H
#define DSCO_WORK_UNIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The typed, addressable unit of work.
 *
 * dsco has no Task type: the "Task" and "Agent" tools are aliases to one
 * function that flattens everything to {"task": <string>}, swarm children
 * carry char task[128], and per-worker personas are impossible in a group
 * because system_prompt is read at group scope. Because work has no identity,
 * there is nothing to record, score, or replay.
 *
 * work_unit_t fixes the root: every unit has a stable id, its own routing and
 * instance parameters, optional dependency edges, and an optional
 * machine-checkable acceptance command. A set of units plus a join policy is
 * a swarm — Swarm(Set<work_unit_t>) — rather than Swarm(Set<char[128]>).
 *
 * The name dsco_task_t was unavailable: include/dsco_pool.h claims it for
 * thread-pool jobs. */

#define WORK_ID_LEN 40 /* uuid_v4 (36) + NUL, padded */

typedef enum {
    WORK_UNIT_PROMPT = 0, /* natural-language task for a model             */
    WORK_UNIT_TOOL,       /* direct tool invocation: payload is input JSON */
    WORK_UNIT_EDIT,       /* file modification described by payload        */
    WORK_UNIT_COMPOSITE,  /* parent of a set; results are joined           */
} work_unit_kind_t;

typedef enum {
    WORK_JOIN_ALL = 0,  /* every child must succeed                        */
    WORK_JOIN_ANY,      /* first success wins, losers are cancelled        */
    WORK_JOIN_QUORUM,   /* at least quorum children must succeed           */
    WORK_JOIN_REDUCE,   /* a coordinator synthesizes child outputs         */
} work_join_t;

typedef struct {
    char id[WORK_ID_LEN];       /* stable identity — set once at create    */
    work_unit_kind_t kind;

    /* What to do. */
    char *title;                /* short human label, never truncated task */
    char *payload;              /* prompt text / tool input / edit spec    */
    char *tool;                 /* WORK_UNIT_TOOL: tool name               */

    /* Routing — per unit, not per group. */
    char *provider;
    char *model;
    char *executor;
    char *effort;

    /* Instance parameters — per unit. This is what makes "worker A reviews,
     * worker B implements" expressible in one group. */
    char *system_prompt;
    char *tool_choice;
    char *cwd;
    double temperature;         /* < 0 means unset                         */
    double top_p;               /* < 0 means unset                         */
    int top_k;                  /* <= 0 means unset                        */
    int thinking_budget;        /* <= 0 means unset                        */
    int max_turns;              /* <= 0 means unset                        */

    /* Dependencies: ids of units that must complete first. */
    char (*depends_on)[WORK_ID_LEN];
    size_t depends_count;

    /* Machine-checkable acceptance: a shell command run after completion;
     * exit 0 accepts the work. NULL means self-reported completion (the
     * status quo, but now explicit rather than implicit). */
    char *acceptance_cmd;

    /* Composite units. */
    work_join_t join;
    int quorum;                 /* WORK_JOIN_QUORUM only                   */

    /* Economics. 0 means unbounded (explicitly, not accidentally). */
    double max_cost_usd;
    uint64_t max_duration_ms;
} work_unit_t;

/* Lifecycle. init assigns a fresh id; free releases all owned members and is
 * safe to call twice. */
void work_unit_init(work_unit_t *w, work_unit_kind_t kind);
void work_unit_free(work_unit_t *w);

/* Field setters copy their argument. Return false on allocation failure. */
bool work_unit_set_title(work_unit_t *w, const char *s);
bool work_unit_set_payload(work_unit_t *w, const char *s);
bool work_unit_set_route(work_unit_t *w, const char *provider, const char *model,
                         const char *executor, const char *effort);
bool work_unit_set_instance(work_unit_t *w, const char *system_prompt,
                            const char *tool_choice, const char *cwd);
bool work_unit_add_dependency(work_unit_t *w, const char *id);

/* JSON round-trip. Parse accepts the object form used by swarm tasks arrays
 * ({"task": "...", "model": "...", ...}) plus the new fields, so existing
 * call sites upgrade without a wire break. Caller frees the serialized
 * string; parse overwrites *w (init'd or not — it re-inits). */
char *work_unit_to_json(const work_unit_t *w);
bool work_unit_from_json(work_unit_t *w, const char *json);

/* Deterministic content hash of the unit's semantic fields (id and title are
 * excluded; two units with identical work hash identically). FNV-1a 64.
 * The replay ledger keys on (id, work_hash) to detect drift. */
uint64_t work_unit_hash(const work_unit_t *w);

const char *work_unit_kind_name(work_unit_kind_t k);
const char *work_join_name(work_join_t j);

#endif /* DSCO_WORK_UNIT_H */
