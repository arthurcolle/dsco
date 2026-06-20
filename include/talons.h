#ifndef DSCO_TALONS_H
#define DSCO_TALONS_H

#include <stdbool.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Talons — Competitive Execution Engine
 *
 * The ability to grip, capture, and WIN. Talons are what make the system
 * effective, not what make it safe (that's the immune system).
 *
 * Core capabilities:
 *   - Tournament selection: race N strategies, keep the winner
 *   - Goal pursuit: aggressive task tracking with deadline enforcement
 *   - Resource capture: claim tasks, acquire capabilities, expand influence
 *   - Precision strikes: targeted execution with confidence scoring
 *   - Grip strength: persistence, retry escalation, never-give-up
 *   - Competitive advantage: learn from wins/losses, adapt strategy
 *
 * A bird of prey doesn't survive by being cautious.
 * It survives by being precise, fast, and relentless.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define TALONS_MAX_GOALS        64
#define TALONS_MAX_STRATEGIES   16
#define TALONS_MAX_COMPETITORS  32
#define TALONS_MAX_HISTORY      256
#define TALONS_GOAL_LEN         512
#define TALONS_STRATEGY_LEN     256
#define TALONS_RESULT_LEN       1024

/* ── Goal States ──────────────────────────────────────────────────────── */

typedef enum {
    GOAL_NASCENT,       /* just identified, not yet pursued */
    GOAL_STALKING,      /* gathering information, planning approach */
    GOAL_STRIKING,      /* actively executing toward goal */
    GOAL_GRIPPING,      /* close to completion, holding tight */
    GOAL_CAPTURED,      /* goal achieved — victory */
    GOAL_ESCAPED,       /* goal failed — prey got away */
    GOAL_ABANDONED,     /* deliberately released */
} goal_state_t;

/* ── Strategy Types ───────────────────────────────────────────────────── */

typedef enum {
    STRATEGY_DIRECT,        /* head-on approach, fastest path */
    STRATEGY_FLANKING,      /* indirect approach, avoid obstacles */
    STRATEGY_TOURNAMENT,    /* race N approaches, pick winner */
    STRATEGY_ESCALATION,    /* start simple, escalate if blocked */
    STRATEGY_DIVIDE,        /* decompose into sub-goals */
    STRATEGY_AMBUSH,        /* prepare thoroughly, execute suddenly */
} strategy_type_t;

/* ── Grip Strength (persistence model) ────────────────────────────────── */

typedef enum {
    GRIP_TENTATIVE,     /* first attempt, may release easily */
    GRIP_HOLDING,       /* committed, will retry on failure */
    GRIP_LOCKED,        /* deeply invested, escalate before releasing */
    GRIP_DEATH_GRIP,    /* critical goal, exhaust all options before failure */
} grip_strength_t;

/* ── Tournament Competitor ────────────────────────────────────────────── */

typedef struct {
    int            id;
    char           label[TALONS_STRATEGY_LEN];
    strategy_type_t strategy;
    int            agent_id;        /* swarm child if parallel */
    double         started_at;
    double         finished_at;
    double         score;           /* quality of result 0.0-1.0 */
    double         cost;            /* resource cost */
    double         speed;           /* time to completion (lower=better) */
    bool           finished;
    bool           winner;
    char           result[TALONS_RESULT_LEN];
} tournament_competitor_t;

/* ── Tournament ───────────────────────────────────────────────────────── */

typedef struct {
    int            id;
    char           objective[TALONS_GOAL_LEN];
    tournament_competitor_t competitors[TALONS_MAX_COMPETITORS];
    int            competitor_count;
    int            winner_id;       /* -1 if undecided */
    bool           active;
    bool           decided;
    double         started_at;
    double         decided_at;
    double         deadline;        /* max time before forced decision */
    /* Scoring weights */
    double         weight_quality;  /* how much quality matters */
    double         weight_speed;    /* how much speed matters */
    double         weight_cost;     /* how much cost matters */
} tournament_t;

/* ── Goal ─────────────────────────────────────────────────────────────── */

typedef struct {
    int             id;
    char            description[TALONS_GOAL_LEN];
    goal_state_t    state;
    grip_strength_t grip;
    strategy_type_t strategy;
    double          priority;       /* 0.0-1.0, higher = more important */
    double          confidence;     /* current estimated chance of success */
    double          deadline;       /* epoch time, 0 = no deadline */
    double          created_at;
    double          started_at;
    double          completed_at;
    int             attempts;       /* total execution attempts */
    int             max_attempts;   /* 0 = unlimited */
    int             parent_goal_id; /* -1 if top-level */
    int             sub_goal_count;
    int             sub_goals_completed;
    int             tournament_id;  /* -1 if no tournament */
    char            last_result[TALONS_RESULT_LEN];
    double          total_cost;     /* accumulated resource cost */
} goal_t;

/* ── Win/Loss Record ──────────────────────────────────────────────────── */

typedef struct {
    int             goal_id;
    bool            won;
    strategy_type_t strategy_used;
    int             attempts;
    double          duration;
    double          cost;
    double          confidence_at_start;
    double          timestamp;
} hunt_record_t;

/* ── Talons Engine ────────────────────────────────────────────────────── */

typedef struct {
    goal_t          goals[TALONS_MAX_GOALS];
    int             goal_count;
    int             next_goal_id;

    tournament_t    tournaments[TALONS_MAX_STRATEGIES];
    int             tournament_count;
    int             next_tournament_id;

    hunt_record_t   history[TALONS_MAX_HISTORY];
    int             history_count;

    bool            initialized;

    /* Aggregate stats */
    int             total_hunts;
    int             wins;
    int             losses;
    int             active_goals;
    double          win_rate;
    double          avg_attempts_to_win;
    double          avg_cost_per_win;

    /* Adaptive strategy weights (learned from history) */
    double          strategy_success[6];    /* per strategy_type_t */
    int             strategy_attempts[6];
} talons_engine_t;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void talons_init(talons_engine_t *t);

/* ── Goal Management ──────────────────────────────────────────────────── */

/* Create a new goal. Returns goal ID. */
int talons_goal_create(talons_engine_t *t, const char *description,
                       double priority, grip_strength_t grip,
                       strategy_type_t strategy, double deadline);

/* Create a sub-goal. Returns goal ID. */
int talons_goal_create_sub(talons_engine_t *t, int parent_id,
                           const char *description, double priority);

/* Begin pursuing a goal (NASCENT -> STALKING). */
bool talons_goal_stalk(talons_engine_t *t, int goal_id);

/* Start active execution (STALKING -> STRIKING). */
bool talons_goal_strike(talons_engine_t *t, int goal_id);

/* Transition to close-grip (STRIKING -> GRIPPING). */
bool talons_goal_grip(talons_engine_t *t, int goal_id);

/* Mark goal as captured (success). */
bool talons_goal_capture(talons_engine_t *t, int goal_id,
                         const char *result, double cost);

/* Mark goal as escaped (failure). May retry based on grip strength. */
bool talons_goal_escaped(talons_engine_t *t, int goal_id,
                         const char *reason, double cost);

/* Deliberately abandon a goal. */
bool talons_goal_abandon(talons_engine_t *t, int goal_id, const char *reason);

/* Update goal confidence estimate. */
bool talons_goal_update_confidence(talons_engine_t *t, int goal_id,
                                    double new_confidence);

/* Get goal by ID. */
const goal_t *talons_goal_get(const talons_engine_t *t, int goal_id);

/* Get active goals sorted by priority. Returns count. */
int talons_active_goals(const talons_engine_t *t, const goal_t **out, int max);

/* ── Tournament Execution ─────────────────────────────────────────────── */

/* Start a tournament: race N strategies for an objective.
   Returns tournament ID. */
int talons_tournament_begin(talons_engine_t *t, const char *objective,
                            double deadline,
                            double weight_quality, double weight_speed,
                            double weight_cost);

/* Add a competitor to a tournament. Returns competitor ID. */
int talons_tournament_add(talons_engine_t *t, int tournament_id,
                          const char *label, strategy_type_t strategy);

/* Record a competitor's result. */
bool talons_tournament_result(talons_engine_t *t, int tournament_id,
                              int competitor_id, double score,
                              double cost, const char *result);

/* Force a decision (pick winner based on current results). */
int talons_tournament_decide(talons_engine_t *t, int tournament_id);

/* Get tournament status. */
const tournament_t *talons_tournament_get(const talons_engine_t *t,
                                           int tournament_id);

/* ── Strategy Selection ───────────────────────────────────────────────── */

/* Recommend best strategy based on history and context.
   Returns the strategy most likely to succeed. */
strategy_type_t talons_recommend_strategy(const talons_engine_t *t,
                                           double time_pressure,
                                           double resource_budget,
                                           double complexity);

/* ── Retry & Escalation ───────────────────────────────────────────────── */

/* Check if a failed goal should be retried based on grip strength.
   Returns true if retry recommended, with suggested new strategy. */
bool talons_should_retry(const talons_engine_t *t, int goal_id,
                         strategy_type_t *suggested_strategy);

/* Escalate grip strength for a goal. */
bool talons_escalate_grip(talons_engine_t *t, int goal_id);

/* ── Analytics ────────────────────────────────────────────────────────── */

/* Get win rate over last N hunts. */
double talons_win_rate(const talons_engine_t *t, int last_n);

/* Get best strategy for given conditions. */
strategy_type_t talons_best_strategy(const talons_engine_t *t);

/* ── Serialization ────────────────────────────────────────────────────── */

int talons_to_json(const talons_engine_t *t, char *buf, size_t len);
int talons_status_json(const talons_engine_t *t, char *buf, size_t len);
int talons_goal_to_json(const goal_t *g, char *buf, size_t len);

/* ── Utilities ────────────────────────────────────────────────────────── */

const char *talons_goal_state_name(goal_state_t s);
const char *talons_strategy_name(strategy_type_t s);
const char *talons_grip_name(grip_strength_t g);

/* ── VFS Persistence ──────────────────────────────────────────────────── */

struct vfs_db;
typedef struct vfs_db vfs_db_t;

void talons_set_vfs(vfs_db_t *vfs);
void talons_persist_strategy_history(void);
void talons_restore_strategy_history(talons_engine_t *t);

#endif
