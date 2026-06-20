#ifndef DSCO_LEGION_H
#define DSCO_LEGION_H

/*
 * Legion: Angel/Demon Agent Registry
 *
 * Angels (777 variants) — thorough, verified, goal-backward agents
 * Demons (888 variants) — fast, parallel, skip-ceremony agents
 *
 * 16 base agent roles (from GSD architecture):
 *   executor, planner, plan_checker, verifier, debugger,
 *   researcher, codebase_mapper, synthesizer, roadmapper,
 *   integration_checker, auditor, ui_researcher, ui_checker,
 *   profiler, reducer, scout
 *
 * Each role × {angel, demon} × parameter sweep = 777 + 888 = 1665 configs
 */

#include <stdbool.h>
#include <stddef.h>
#include "topology.h"

/* ── Agent class (angel vs demon) ─────────────────────────────────────── */

typedef enum {
    AGENT_CLASS_ANGEL  = 0,   /* thorough: full verification, goal-backward, careful */
    AGENT_CLASS_DEMON  = 1,   /* fast: skip verification, parallel execution, aggressive */
} agent_class_t;

/* ── 16 base agent roles ─────────────────────────────────────────────── */

typedef enum {
    LEGION_ROLE_EXECUTOR         = 0,
    LEGION_ROLE_PLANNER          = 1,
    LEGION_ROLE_PLAN_CHECKER     = 2,
    LEGION_ROLE_VERIFIER         = 3,
    LEGION_ROLE_DEBUGGER         = 4,
    LEGION_ROLE_RESEARCHER       = 5,
    LEGION_ROLE_CODEBASE_MAPPER  = 6,
    LEGION_ROLE_SYNTHESIZER      = 7,
    LEGION_ROLE_ROADMAPPER       = 8,
    LEGION_ROLE_INTEGRATION_CHK  = 9,
    LEGION_ROLE_AUDITOR          = 10,
    LEGION_ROLE_UI_RESEARCHER    = 11,
    LEGION_ROLE_UI_CHECKER       = 12,
    LEGION_ROLE_PROFILER         = 13,
    LEGION_ROLE_REDUCER          = 14,
    LEGION_ROLE_SCOUT            = 15,
    LEGION_ROLE_COUNT            = 16,
} legion_role_t;

static inline const char *legion_role_name(legion_role_t r) {
    static const char *names[] = {
        "executor", "planner", "plan_checker", "verifier",
        "debugger", "researcher", "codebase_mapper", "synthesizer",
        "roadmapper", "integration_checker", "auditor", "ui_researcher",
        "ui_checker", "profiler", "reducer", "scout",
    };
    return (r >= 0 && r < LEGION_ROLE_COUNT) ? names[r] : "unknown";
}

/* Map legion roles to topology node roles */
static inline node_role_t legion_to_node_role(legion_role_t r) {
    switch (r) {
        case LEGION_ROLE_EXECUTOR:        return ROLE_WORKER;
        case LEGION_ROLE_PLANNER:         return ROLE_COORDINATOR;
        case LEGION_ROLE_PLAN_CHECKER:    return ROLE_VALIDATOR;
        case LEGION_ROLE_VERIFIER:        return ROLE_VALIDATOR;
        case LEGION_ROLE_DEBUGGER:        return ROLE_SPECIALIST;
        case LEGION_ROLE_RESEARCHER:      return ROLE_SCOUT;
        case LEGION_ROLE_CODEBASE_MAPPER: return ROLE_SCOUT;
        case LEGION_ROLE_SYNTHESIZER:     return ROLE_SYNTHESIZER;
        case LEGION_ROLE_ROADMAPPER:      return ROLE_DELEGATOR;
        case LEGION_ROLE_INTEGRATION_CHK: return ROLE_VALIDATOR;
        case LEGION_ROLE_AUDITOR:         return ROLE_CRITIC;
        case LEGION_ROLE_UI_RESEARCHER:   return ROLE_SCOUT;
        case LEGION_ROLE_UI_CHECKER:      return ROLE_CRITIC;
        case LEGION_ROLE_PROFILER:        return ROLE_CLASSIFIER;
        case LEGION_ROLE_REDUCER:         return ROLE_REDUCER;
        case LEGION_ROLE_SCOUT:           return ROLE_SCOUT;
        default:                          return ROLE_WORKER;
    }
}

/* ── Agent variant parameters ─────────────────────────────────────────── */

typedef struct {
    int              id;                  /* global variant ID */
    agent_class_t    cls;                 /* angel or demon */
    legion_role_t    role;                /* base role */
    model_tier_t     tier;                /* preferred model tier */
    int              max_turns;           /* turn budget */
    int              max_tools_per_turn;  /* parallel tool limit */
    int              context_budget_pct;  /* target context usage (10-90%) */
    bool             verify_output;       /* run verification pass? */
    bool             auto_fix;            /* auto-fix deviations (Rules 1-3)? */
    bool             paralysis_guard;     /* force action after N reads? */
    int              paralysis_threshold; /* reads before forced action */
    bool             plan_aware_compress; /* PAACE-style compression? */
    bool             goal_backward;       /* goal-backward verification? */
    bool             hypothesis_driven;   /* hypothesis-testing debug? */
    bool             nyquist_check;       /* automated verify <60s? */
    double           budget_usd;          /* per-agent cost cap */
    int              wave;                /* preferred wave (0=auto) */
    int              replicas;            /* parallel copies */
    char             name[64];            /* human-readable name */
    char             prompt_prefix[512];  /* role-specific system prompt fragment */
} legion_variant_t;

/* ── Variant counts ───────────────────────────────────────────────────── */

#define LEGION_ANGEL_COUNT  777
#define LEGION_DEMON_COUNT  888
#define LEGION_TOTAL_COUNT  (LEGION_ANGEL_COUNT + LEGION_DEMON_COUNT)

/* ── Registry API ─────────────────────────────────────────────────────── */

void                    legion_init(void);
const legion_variant_t *legion_get(int id);
const legion_variant_t *legion_find(const char *name);
const legion_variant_t *legion_registry(int *count);

/* Find variants by class and role */
int  legion_find_by_role(legion_role_t role, agent_class_t cls,
                         const legion_variant_t **out, int max_out);

/* Auto-select best variant for a task */
const legion_variant_t *legion_auto_select(const char *task, agent_class_t cls);

/* Spawn a legion agent as a swarm child */
int  legion_spawn(int variant_id, const char *task);

/* Spawn a squad (multiple variants in parallel) */
int  legion_spawn_squad(const int *variant_ids, int count,
                        const char *task, int *out_agent_ids);

/* Format variant info as JSON */
int  legion_variant_json(const legion_variant_t *v, char *buf, size_t len);

/* Stats */
int  legion_angel_count(void);
int  legion_demon_count(void);
int  legion_count_by_role(legion_role_t role);

#endif /* DSCO_LEGION_H */
