#ifndef DSCO_TOPOLOGY_H
#define DSCO_TOPOLOGY_H

#include <stdbool.h>
#include <stddef.h>

/* ── Model tier shorthand ─────────────────────────────────────────────── */

typedef enum {
    TIER_HAIKU  = 0,   /* fast / cheap / classifier / executor */
    TIER_SONNET = 1,   /* mid-tier / implementer / analyst */
    TIER_OPUS   = 2,   /* strategic / planner / synthesizer */
} model_tier_t;

static inline const char *tier_model_id(model_tier_t t) {
    switch (t) {
        case TIER_HAIKU:  return "claude-haiku-4-5-20251001";
        case TIER_SONNET: return "claude-sonnet-4-6";
        case TIER_OPUS:   return "claude-opus-4-6";
    }
    return "claude-sonnet-4-6";
}

static inline const char *tier_label(model_tier_t t) {
    switch (t) {
        case TIER_HAIKU:  return "H";
        case TIER_SONNET: return "S";
        case TIER_OPUS:   return "O";
    }
    return "?";
}

/* ── Node roles ───────────────────────────────────────────────────────── */

typedef enum {
    ROLE_COORDINATOR = 0,   /* orchestrates other nodes */
    ROLE_DELEGATOR,         /* breaks work into sub-tasks */
    ROLE_WORKER,            /* executes a concrete task */
    ROLE_CLASSIFIER,        /* routes / triages input */
    ROLE_CRITIC,            /* reviews / critiques output */
    ROLE_SYNTHESIZER,       /* merges multiple outputs */
    ROLE_SPECIALIST,        /* domain-specific expert */
    ROLE_GENERATOR,         /* produces candidate outputs */
    ROLE_VALIDATOR,         /* checks correctness */
    ROLE_REDUCER,           /* reduces / summarizes data */
    ROLE_SCOUT,             /* gathers / searches for info */
    ROLE_JUDGE,             /* picks winner / makes final call */
} node_role_t;

static inline const char *node_role_label(node_role_t r) {
    static const char *labels[] = {
        "coordinator", "delegator", "worker", "classifier", "critic",
        "synthesizer", "specialist", "generator", "validator", "reducer",
        "scout", "judge",
    };
    return (r >= 0 && r <= ROLE_JUDGE) ? labels[r] : "unknown";
}

/* ── Topology node ────────────────────────────────────────────────────── */

#define TOPO_MAX_NODES 32
#define TOPO_MAX_EDGES 64
#define TOPO_MAX_TAG   32

typedef struct {
    int           id;                  /* 0-indexed node ID */
    model_tier_t  tier;                /* which model to use */
    node_role_t   role;                /* what this node does */
    char          tag[TOPO_MAX_TAG];   /* human-readable label */
    int           replicas;            /* how many parallel copies (>=1) */
} topo_node_t;

/* ── Topology edge ────────────────────────────────────────────────────── */

typedef enum {
    EDGE_SEQUENCE = 0,  /* A completes, then B starts */
    EDGE_FANOUT,        /* A fans out to B (parallel) */
    EDGE_FANIN,         /* A feeds into B (merge point) */
    EDGE_FEEDBACK,      /* B's output feeds back to A */
    EDGE_CONDITIONAL,   /* A routes to B based on classifier */
    EDGE_COMPETE,       /* A and B both attempt, best wins */
} edge_type_t;

typedef struct {
    int         from;   /* source node ID */
    int         to;     /* target node ID */
    edge_type_t type;
} topo_edge_t;

/* ── Execution strategy ───────────────────────────────────────────────── */

typedef enum {
    EXEC_LINEAR = 0,       /* nodes execute in topological order */
    EXEC_PARALLEL_STAGES,  /* parallel within stages, sequential between */
    EXEC_FULL_PARALLEL,    /* maximum parallelism respecting edges */
    EXEC_ITERATIVE,        /* may loop through feedback edges N times */
    EXEC_TOURNAMENT,       /* competitive: multiple attempts, pick best */
    EXEC_CONSENSUS,        /* all vote, majority wins */
} exec_strategy_t;

/* ── Topology category ────────────────────────────────────────────────── */

typedef enum {
    CAT_CHAIN = 0,         /* linear pipeline */
    CAT_FANOUT,            /* fan-out / fan-in */
    CAT_HIERARCHY,         /* tree delegation */
    CAT_MESH,              /* peer / network */
    CAT_SPECIALIST,        /* router / expert */
    CAT_FEEDBACK,          /* iterative refinement */
    CAT_COMPETITIVE,       /* redundant / best-of-N */
    CAT_DOMAIN,            /* domain-specific workflow */
} topo_category_t;

static inline const char *topo_category_label(topo_category_t c) {
    static const char *labels[] = {
        "chain", "fanout", "hierarchy", "mesh",
        "specialist", "feedback", "competitive", "domain",
    };
    return (c >= 0 && c <= CAT_DOMAIN) ? labels[c] : "unknown";
}

/* ── Full topology definition ─────────────────────────────────────────── */

typedef struct {
    int              id;                  /* unique topology ID (0-59) */
    char             name[48];            /* short name */
    char             description[256];    /* what it does */
    topo_category_t  category;
    exec_strategy_t  strategy;
    int              max_iterations;      /* for EXEC_ITERATIVE */
    /* Nodes */
    topo_node_t      nodes[TOPO_MAX_NODES];
    int              node_count;
    /* Edges */
    topo_edge_t      edges[TOPO_MAX_EDGES];
    int              edge_count;
    /* Cost profile */
    double           est_cost_1k;        /* estimated $/1K tokens throughput */
    double           est_latency_mult;   /* latency multiplier vs single call */
    int              total_agents;       /* total agent count including replicas */
} topology_t;

typedef struct {
    int    iterations;
    int    nodes_executed;
    int    agents_spawned;
    double est_cost_usd;
    char   final_node_tag[TOPO_MAX_TAG];
} topology_run_stats_t;

typedef enum {
    TOPO_TASK_GENERAL = 0,
    TOPO_TASK_CODE,
    TOPO_TASK_RESEARCH,
    TOPO_TASK_REVIEW,
    TOPO_TASK_CREATIVE,
    TOPO_TASK_INCIDENT,
} topology_task_kind_t;

typedef struct {
    topology_task_kind_t kind;
    int                  complexity;          /* 1-5 */
    int                  desired_parallelism; /* 1-8 */
    bool                 needs_iteration;
    bool                 needs_validation;
    bool                 prefers_breadth;
} topology_task_profile_t;

typedef struct {
    topology_t              topology;
    topology_task_profile_t profile;
    bool                    is_dynamic;
    char                    rationale[256];
} topology_plan_t;

/* ── Registry API ─────────────────────────────────────────────────────── */

#define TOPOLOGY_COUNT 60

void             topology_registry_init(void);
const topology_t *topology_get(int id);
const topology_t *topology_find(const char *name);
const topology_t *topology_registry(int *count);
const topology_t *topology_auto_select(const char *task);
bool             topology_is_runnable(const topology_t *t);
bool             topology_profile_task(const char *task, topology_task_profile_t *profile);
bool             topology_plan_build(const char *preferred_topology,
                                     bool auto_mode,
                                     const char *task,
                                     topology_plan_t *plan);
bool             topology_plan_run(const topology_plan_t *plan,
                                   const char *api_key,
                                   const char *coordinator_model,
                                   const char *task,
                                   char *result, size_t rlen,
                                   topology_run_stats_t *stats);

/* Utility: render ASCII diagram of a topology */
int  topology_render_ascii(const topology_t *t, char *buf, size_t buflen);
/* Utility: estimate cost for a given input size */
double topology_estimate_cost(const topology_t *t, int input_tokens, int output_tokens);
/* Utility: resolve a concrete model for a tier using the coordinator model/provider */
const char *topology_resolve_model_for_tier(const char *coordinator_model,
                                            const char *api_key,
                                            model_tier_t tier,
                                            char *buf, size_t buflen);
/* Execute a topology against a task using spawned sub-agents */
bool topology_run(const topology_t *t,
                  const char *api_key,
                  const char *coordinator_model,
                  const char *task,
                  char *result, size_t rlen,
                  topology_run_stats_t *stats);

#endif
