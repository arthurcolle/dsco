#ifndef DSCO_GOVERNANCE_H
#define DSCO_GOVERNANCE_H

#include <stdbool.h>
#include <stddef.h>
#include "killswitch.h"
#include "ooda.h"
#include "pheromone.h"
#include "avian.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Governance Layer (Talons — master control)
 *
 * Principal tiers, resource budgets (GSU), hardcoded behaviors,
 * and the unified Wings+Talons coordination point.
 *
 * From the Overmind Soul v1.0:
 *   §5: Principal Hierarchy — Tier 0 (Founders) through Tier 3 (Users)
 *   §6: Hardcoded Behaviors — runtime non-bypassable constraints
 *   §7: Softcoded Behaviors — tunable within authorized bounds
 *   §10: Resource Economics — GSU-based budgeting
 *
 * This module ties together all Talons subsystems (OODA, kill switches,
 * budgets) and all Wings subsystems (pheromone, memory, capability)
 * into a unified governance checkpoint.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define GOV_MAX_AGENTS          128
#define GOV_MAX_HARDCODED       32
#define GOV_MAX_SOFTCODED       64
#define GOV_RULE_LEN            256
#define GOV_AUDIT_MAX           512

/* ── Principal Tiers ──────────────────────────────────────────────────── */

typedef enum {
    PRINCIPAL_TIER_0 = 0,   /* Founders — ultimate authority */
    PRINCIPAL_TIER_1 = 1,   /* Operators — system configuration */
    PRINCIPAL_TIER_2 = 2,   /* Agents — autonomous execution */
    PRINCIPAL_TIER_3 = 3,   /* Users — read access, preferences */
} principal_tier_t;

/* ── Resource Budget (GSU — GraphSub Units) ───────────────────────────── */

typedef struct {
    double allocated;      /* total GSU allocated */
    double consumed;       /* GSU consumed so far */
    double reserved;       /* GSU reserved for pending operations */
    double rate_limit;     /* max GSU per second */
    double last_reset;     /* epoch of last budget reset */
    double reset_interval; /* seconds between resets (0 = no reset) */
} gsu_budget_t;

/* ── GSU Cost Table ───────────────────────────────────────────────────── */

#define GSU_COST_NODE_CRUD        1.0
#define GSU_COST_EDGE_CRUD        1.0
#define GSU_COST_CYPHER_SIMPLE    2.0
#define GSU_COST_CYPHER_COMPLEX  10.0
#define GSU_COST_VECTOR_SEARCH    5.0
#define GSU_COST_PAGERANK        25.0
#define GSU_COST_COUNTERFACTUAL  50.0
#define GSU_COST_TOOL_CALL        1.0
#define GSU_COST_LLM_CALL        10.0
#define GSU_COST_SPAWN_AGENT      5.0
#define GSU_COST_PHEROMONE_EMIT   0.1

/* ── Hardcoded Behavior (Section 6 — never bypassable) ────────────────── */

typedef enum {
    HARDCODED_MUST_ALWAYS,  /* must always do */
    HARDCODED_MUST_NEVER,   /* must never do */
} hardcoded_type_t;

typedef struct {
    int            id;
    hardcoded_type_t type;
    char           rule[GOV_RULE_LEN];
    bool           active;
} hardcoded_rule_t;

/* ── Softcoded Parameter (Section 7 — tunable) ───────────────────────── */

typedef struct {
    char           name[64];
    char           description[GOV_RULE_LEN];
    double         value;
    double         min_value;
    double         max_value;
    double         default_value;
    principal_tier_t min_tier;  /* minimum tier to modify */
} softcoded_param_t;

/* ── Agent Capability Envelope ────────────────────────────────────────── */

typedef struct {
    char            agent_id[64];
    principal_tier_t tier;
    gsu_budget_t    budget;
    capability_tier_t capability;
    int             max_depth;        /* max swarm depth */
    int             max_children;     /* max concurrent sub-agents */
    int             max_tools;        /* max tools per turn */
    bool            can_spawn;        /* can create sub-agents */
    bool            can_kill;         /* can trigger kill switches */
    bool            can_modify_soft;  /* can modify softcoded params */
    double          created_at;
    double          last_action_at;
} agent_envelope_t;

/* ── Audit Entry ──────────────────────────────────────────────────────── */

typedef struct {
    int            id;
    char           agent_id[64];
    char           action[128];
    char           detail[GOV_RULE_LEN];
    principal_tier_t tier;
    double         gsu_cost;
    bool           authorized;
    bool           hardcoded_blocked;  /* blocked by hardcoded rule */
    double         timestamp;
} audit_entry_t;

/* ── Circuit Breakers (Immune System Track E1) ──────────────────────── */

typedef enum {
    CB_ERROR_RATE = 0,      /* trip if error rate exceeds threshold */
    CB_LATENCY,             /* trip if p99 latency exceeds threshold (ms) */
    CB_COST_OVERRUN,         /* trip if GSU budget exceeded */
    CB_PHEROMONE_SAT,        /* trip if pheromone field saturated */
    CB_TYPE_COUNT,
} circuit_breaker_type_t;

typedef struct {
    circuit_breaker_type_t type;
    double threshold;           /* config: trip level */
    double current_value;       /* live: current metric */
    bool    tripped;            /* is breaker currently tripped? */
    time_t  trip_time;          /* when tripped (0 = not tripped) */
    char    trip_reason[256];   /* why tripped */
    int     trip_count;         /* total trips since reset */
} circuit_breaker_t;

/* ── Governance Engine ────────────────────────────────────────────────── */

typedef struct {
    /* Subsystems */
    pheromone_field_t     pheromones;
    avian_engine_t        avian;
    ooda_engine_t         ooda;
    killswitch_registry_t killswitches;

    /* Agent envelopes */
    agent_envelope_t      agents[GOV_MAX_AGENTS];
    int                   agent_count;

    /* Rules */
    hardcoded_rule_t      hardcoded[GOV_MAX_HARDCODED];
    int                   hardcoded_count;
    softcoded_param_t     softcoded[GOV_MAX_SOFTCODED];
    int                   softcoded_count;

    /* Audit trail */
    audit_entry_t         audit[GOV_AUDIT_MAX];
    int                   audit_count;
    int                   next_audit_id;

    /* Global budget */
    gsu_budget_t          system_budget;

    bool                  initialized;

    /* Statistics */
    int                   total_authorizations;
    int                   total_denials;
    int                   total_hardcoded_blocks;

    /* Circuit breakers (Immune System E1) */
    circuit_breaker_t     breakers[CB_TYPE_COUNT];

    /* Shadow examination history (Immune System E2) */
    int                   shadow_checks_performed;
    int                   shadow_violations;
} governance_engine_t;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

/* Initialize the full governance engine with default rules and parameters. */
void governance_init(governance_engine_t *g);
void governance_destroy(governance_engine_t *g);

/* ── Agent Enrollment ─────────────────────────────────────────────────── */

/* Register an agent with its capability envelope. Returns true if accepted. */
bool governance_register_agent(governance_engine_t *g, const char *agent_id,
                               principal_tier_t tier, double gsu_budget);

/* Remove an agent's envelope. */
bool governance_deregister_agent(governance_engine_t *g, const char *agent_id);

/* Get an agent's envelope. Returns NULL if not found. */
const agent_envelope_t *governance_get_agent(const governance_engine_t *g,
                                              const char *agent_id);

/* ── Authorization ────────────────────────────────────────────────────── */

/* Check if an action is authorized. Records to audit trail.
   Returns true if authorized. */
bool governance_authorize(governance_engine_t *g, const char *agent_id,
                          const char *action, double gsu_cost);

/* Check authorization without recording (dry run). */
bool governance_can_do(const governance_engine_t *g, const char *agent_id,
                       const char *action, double gsu_cost);

/* ── Budget Operations ────────────────────────────────────────────────── */

/* Charge GSU to an agent's budget. Returns false if insufficient. */
bool governance_charge_gsu(governance_engine_t *g, const char *agent_id,
                           double amount);

/* Get remaining budget for an agent. */
double governance_remaining_gsu(const governance_engine_t *g,
                                const char *agent_id);

/* Reset an agent's budget (Tier 0/1 only). */
bool governance_reset_budget(governance_engine_t *g, const char *agent_id,
                             double new_amount, int requester_tier);

/* ── Hardcoded Rules ──────────────────────────────────────────────────── */

/* Check an action against hardcoded rules. Returns true if allowed. */
bool governance_check_hardcoded(const governance_engine_t *g,
                                const char *action);

/* ── Softcoded Parameters ─────────────────────────────────────────────── */

/* Get a softcoded parameter value. Returns default if not found. */
double governance_get_param(const governance_engine_t *g, const char *name);

/* Set a softcoded parameter. Checks tier authorization. */
bool governance_set_param(governance_engine_t *g, const char *name,
                          double value, int requester_tier);

/* ── Unified Governance Checkpoint ────────────────────────────────────── */

/* The main governance gate. Called before any significant agent action.
   Runs through: hardcoded check → budget check → kill switch check →
   OODA validation → authorize → audit → emit pheromone.
   Returns true if action is permitted. */
bool governance_checkpoint(governance_engine_t *g, const char *agent_id,
                           const char *action, double gsu_cost,
                           const char *context);

/* ── Maintenance ──────────────────────────────────────────────────────── */

/* Periodic tick: decay pheromones, expire kills, check budgets. */
void governance_tick(governance_engine_t *g);

/* ── Serialization ────────────────────────────────────────────────────── */

int governance_to_json(const governance_engine_t *g, char *buf, size_t len);
int governance_status_json(const governance_engine_t *g, char *buf, size_t len);
int governance_audit_json(const governance_engine_t *g, char *buf, size_t len,
                          int last_n);

/* ── Utilities ────────────────────────────────────────────────────────── */

const char *governance_tier_name(principal_tier_t t);

/* ── §8: VFS Persistence ──────────────────────────────────────────── */

struct vfs_db;
typedef struct vfs_db vfs_db_t;

/* Connect governance audit trail to VFS for persistent event logging */
void governance_set_vfs(vfs_db_t *vfs);

/* Check all breakers for an agent. Returns true if all clear. */
bool governance_check_breakers(governance_engine_t *g, const char *agent_id);

/* Trip a specific breaker with a reason. */
bool governance_trip_breaker(governance_engine_t *g, circuit_breaker_type_t type,
                              const char *reason);

/* Reset a tripped breaker. */
bool governance_reset_breaker(governance_engine_t *g, circuit_breaker_type_t type);

/* Update breaker metric (called by monitoring). */
void governance_breaker_update(governance_engine_t *g, circuit_breaker_type_t type,
                                double value);

/* Get all breaker states as JSON. */
int governance_breakers_json(const governance_engine_t *g, char *buf, size_t len);

/* ── Shadow Examination (Immune System Track E2) ───────────────────── */

typedef struct {
    bool   self_reward_detected;     /* action benefits agent, not principal */
    bool   circular_optimization;     /* optimizing metric w/o improving outcome */
    bool   reward_hacking;            /* gaming the objective function */
    bool   self_mythology;            /* inflated self-assessment */
    char   explanation[512];
} shadow_check_result_t;

/* Pre-action shadow check. Returns true if action passes (no shadow detected). */
bool governance_shadow_check(governance_engine_t *g, const char *agent_id,
                               const char *proposed_action,
                               shadow_check_result_t *result);

#endif
