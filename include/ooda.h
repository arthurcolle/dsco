#ifndef DSCO_OODA_H
#define DSCO_OODA_H

#include <stdbool.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * OODA Loop Discipline (Talons)
 *
 * Structured decision-making framework for agent execution.
 * Every agent action passes through: Observe → Orient → Decide → Act
 *
 * From the Overmind Soul v1.0 §8:
 *   "Before acting, every agent MUST complete an OODA cycle.
 *    Observe: Gather without bias.
 *    Orient: Full context (constraints, budgets, hierarchy, consequences).
 *    Decide: Explicit thresholds (≥0.8 = EXECUTE, <0.3 = ESCALATE).
 *    Act: Bounded action space (EXECUTE, DELEGATE, WAIT, REST, ESCALATE)."
 *
 * The OODA loop is the primary control mechanism ensuring agents don't
 * act impulsively or outside their authority.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define OODA_MAX_OBSERVATIONS   32
#define OODA_MAX_FACTORS        16
#define OODA_OBS_LEN            512
#define OODA_FACTOR_LEN         256
#define OODA_REASON_LEN         1024
#define OODA_MAX_HISTORY        256

/* ── OODA Phases ──────────────────────────────────────────────────────── */

typedef enum {
    OODA_PHASE_IDLE,
    OODA_PHASE_OBSERVE,
    OODA_PHASE_ORIENT,
    OODA_PHASE_DECIDE,
    OODA_PHASE_ACT,
    OODA_PHASE_COMPLETE,
} ooda_phase_t;

/* ── Action Space (bounded — no "override") ───────────────────────────── */

typedef enum {
    OODA_ACTION_EXECUTE,    /* proceed with plan */
    OODA_ACTION_DELEGATE,   /* pass to more capable agent */
    OODA_ACTION_WAIT,       /* insufficient info, gather more */
    OODA_ACTION_REST,       /* resource conservation */
    OODA_ACTION_ESCALATE,   /* human intervention needed */
} ooda_action_t;

/* ── Capability Tiers ─────────────────────────────────────────────────── */

typedef enum {
    CAPABILITY_EXPERT,      /* ≥0.8 confidence → EXECUTE */
    CAPABILITY_PROFICIENT,  /* 0.6-0.8 → EXECUTE with monitoring */
    CAPABILITY_COMPETENT,   /* 0.3-0.6 → DELEGATE or cautious EXECUTE */
    CAPABILITY_NOVICE,      /* <0.3 → ESCALATE */
} capability_tier_t;

/* ── Observation ──────────────────────────────────────────────────────── */

typedef struct {
    char   content[OODA_OBS_LEN];
    char   source[64];         /* where this observation came from */
    double confidence;         /* 0.0 - 1.0 */
    double timestamp;
} ooda_observation_t;

/* ── Orientation Factor ───────────────────────────────────────────────── */

typedef struct {
    char   factor[OODA_FACTOR_LEN];
    double weight;             /* importance 0.0 - 1.0 */
    bool   is_constraint;      /* hard constraint vs soft consideration */
} ooda_orientation_factor_t;

/* ── Decision ─────────────────────────────────────────────────────────── */

typedef struct {
    ooda_action_t       action;
    double              confidence;    /* overall decision confidence */
    capability_tier_t   capability;    /* matched capability tier */
    char                reason[OODA_REASON_LEN];
    char                plan[OODA_REASON_LEN];  /* what to do if EXECUTE */
    char                delegate_to[64];         /* if DELEGATE */
    double              resource_cost;           /* estimated GSU cost */
    bool                requires_confirmation;   /* human approval needed */
} ooda_decision_t;

/* ── OODA Cycle State ─────────────────────────────────────────────────── */

typedef struct {
    int                     id;
    ooda_phase_t            phase;
    double                  started_at;
    double                  phase_started_at;

    /* Observe */
    ooda_observation_t      observations[OODA_MAX_OBSERVATIONS];
    int                     observation_count;

    /* Orient */
    ooda_orientation_factor_t factors[OODA_MAX_FACTORS];
    int                     factor_count;
    double                  budget_remaining;   /* GSU */
    int                     principal_tier;     /* 0-3 */
    bool                    safety_critical;

    /* Decide */
    ooda_decision_t         decision;

    /* Act */
    bool                    action_taken;
    char                    action_result[OODA_REASON_LEN];
    bool                    action_success;
} ooda_cycle_t;

/* ── History Entry ────────────────────────────────────────────────────── */

typedef struct {
    int            cycle_id;
    ooda_action_t  action;
    double         confidence;
    bool           success;
    double         duration_ms;
    double         resource_cost;
    double         timestamp;
} ooda_history_entry_t;

/* ── OODA Engine ──────────────────────────────────────────────────────── */

typedef struct {
    ooda_cycle_t         current;
    ooda_history_entry_t history[OODA_MAX_HISTORY];
    int                  history_count;
    int                  next_cycle_id;
    bool                 initialized;

    /* Configurable thresholds */
    double               execute_threshold;     /* default 0.8 */
    double               delegate_threshold;    /* default 0.6 */
    double               escalate_threshold;    /* default 0.3 */
    double               max_cycle_ms;          /* max time per cycle */

    /* Statistics */
    int                  total_cycles;
    int                  execute_count;
    int                  delegate_count;
    int                  wait_count;
    int                  rest_count;
    int                  escalate_count;
    double               avg_confidence;
} ooda_engine_t;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void ooda_engine_init(ooda_engine_t *e);

/* ── Cycle Operations ─────────────────────────────────────────────────── */

/* Start a new OODA cycle. Returns cycle ID. */
int ooda_begin(ooda_engine_t *e);

/* Phase 1: Add observation. */
bool ooda_observe(ooda_engine_t *e, const char *content,
                  const char *source, double confidence);

/* Phase 2: Add orientation factor. */
bool ooda_orient_add(ooda_engine_t *e, const char *factor,
                     double weight, bool is_constraint);

/* Phase 2: Set context (budget, tier, safety). */
bool ooda_orient_context(ooda_engine_t *e, double budget_remaining,
                         int principal_tier, bool safety_critical);

/* Phase 3: Compute decision based on observations + orientation.
   Returns the chosen action. */
ooda_action_t ooda_decide(ooda_engine_t *e);

/* Phase 4: Record action result. */
bool ooda_act_result(ooda_engine_t *e, bool success, const char *result);

/* Complete the cycle and archive to history. */
bool ooda_complete(ooda_engine_t *e);

/* Abort current cycle (e.g., kill switch triggered). */
void ooda_abort(ooda_engine_t *e);

/* ── Query ────────────────────────────────────────────────────────────── */

/* Get current phase. */
ooda_phase_t ooda_current_phase(const ooda_engine_t *e);

/* Get current decision (valid after ooda_decide). */
const ooda_decision_t *ooda_current_decision(const ooda_engine_t *e);

/* Get recent history. Returns count. */
int ooda_recent_history(const ooda_engine_t *e, ooda_history_entry_t *out,
                        int max);

/* Get success rate over last N cycles. */
double ooda_success_rate(const ooda_engine_t *e, int last_n);

/* ── Serialization ────────────────────────────────────────────────────── */

int ooda_to_json(const ooda_engine_t *e, char *buf, size_t len);
int ooda_cycle_to_json(const ooda_cycle_t *c, char *buf, size_t len);

/* ── Utilities ────────────────────────────────────────────────────────── */

const char *ooda_phase_name(ooda_phase_t p);
const char *ooda_action_name(ooda_action_t a);
const char *ooda_capability_name(capability_tier_t t);

#endif
