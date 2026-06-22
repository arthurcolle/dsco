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
#define TALONS_MAX_DEPS         8     /* prerequisite goals a goal can block on */

/* ── Goal States ──────────────────────────────────────────────────────── */

typedef enum {
    GOAL_NASCENT,       /* just identified, not yet pursued */
    GOAL_STALKING,      /* gathering information, planning approach */
    GOAL_STRIKING,      /* actively executing toward goal */
    GOAL_GRIPPING,      /* close to completion, holding tight */
    GOAL_CAPTURED,      /* goal achieved — victory (terminal) */
    GOAL_ESCAPED,       /* goal failed — prey got away (terminal) */
    GOAL_ABANDONED,     /* deliberately released (terminal) */
    /* Appended after the terminal trio so existing terminality checks
     * (== CAPTURED/ESCAPED/ABANDONED) keep treating these as non-terminal. */
    GOAL_BLOCKED,       /* waiting on prerequisite goals to be captured */
    GOAL_REGROUPING,    /* a strike failed; preparing another attempt */
    GOAL_STATE_COUNT,
} goal_state_t;

/* True for the three end-states a goal can never leave. */
static inline bool goal_state_is_terminal(goal_state_t s) {
    return s == GOAL_CAPTURED || s == GOAL_ESCAPED || s == GOAL_ABANDONED;
}

/* ── Strategy Types ───────────────────────────────────────────────────── */

typedef enum {
    STRATEGY_DIRECT,        /* head-on approach, fastest path */
    STRATEGY_FLANKING,      /* indirect approach, avoid obstacles */
    STRATEGY_TOURNAMENT,    /* race N approaches, pick winner */
    STRATEGY_ESCALATION,    /* start simple, escalate if blocked */
    STRATEGY_DIVIDE,        /* decompose into sub-goals */
    STRATEGY_AMBUSH,        /* prepare thoroughly, execute suddenly */
    STRATEGY_ATTRITION,     /* repeated small attempts, wear down resistance */
    STRATEGY_PINCER,        /* two approaches at once, converge on the target */
    STRATEGY_BLITZ,         /* overwhelming speed + force, commit everything now */
    STRATEGY_SIEGE,         /* encircle, cut off options, apply sustained pressure */
    STRATEGY_FEINT,         /* fake one approach to draw out resistance, strike elsewhere */
    STRATEGY_OPPORTUNISTIC, /* wait patiently, strike the moment a weakness opens */

    /* ── Canon from military history ──────────────────────────────────────
     * Named doctrines/maneuvers, roughly Sun Tzu → Clausewitz → Liddell
     * Hart → Boyd, plus classic battlefield maneuvers. */
    STRATEGY_ENVELOPMENT,    /* sweep around one flank into the enemy rear */
    STRATEGY_ENCIRCLEMENT,   /* close a ring around the target (Kesselschlacht/Cannae) */
    STRATEGY_GUERRILLA,      /* irregular hit-and-run, never hold ground */
    STRATEGY_SCORCHED_EARTH, /* deny the adversary every resource as you yield */
    STRATEGY_FABIAN,         /* refuse decisive battle; delay, harass, outlast */
    STRATEGY_DEFENSE_IN_DEPTH,/* layered defense: absorb, slow, then counter */
    STRATEGY_OBLIQUE,        /* refuse one flank, mass on the other (Leuthen) */
    STRATEGY_INFILTRATION,   /* bypass strongpoints, attack soft rear (Hutier) */
    STRATEGY_INTERIOR_LINES, /* central position; shift force to beat each part */
    STRATEGY_DEFEAT_IN_DETAIL,/* engage the enemy piecemeal before they mass */
    STRATEGY_TURNING_MOVEMENT,/* maneuver deep to make the position untenable */
    STRATEGY_BREAKTHROUGH,   /* mass at one point (Schwerpunkt), punch through */
    STRATEGY_SHOCK,          /* one overwhelming blow to shatter cohesion */
    STRATEGY_DECAPITATION,   /* strike command/leadership, collapse the system */
    STRATEGY_BLOCKADE,       /* sever supply and reinforcement, then squeeze */
    STRATEGY_RAID,           /* fast strike and withdrawal, no intent to hold */
    STRATEGY_INDIRECT,       /* Liddell Hart: dislocate before engaging */
    STRATEGY_TEMPO,          /* Boyd/OODA: act faster than they can react */
    STRATEGY_DETERRENCE,     /* hold a credible threat so the fight never starts */
    STRATEGY_COUNTERATTACK,  /* absorb the blow, then riposte at the overextension */
    STRATEGY_MANEUVER,       /* dislocate by movement rather than destroy by fire */
    STRATEGY_HEDGEHOG,       /* all-around strongpoint that breaks momentum */
    STRATEGY_SCREEN,         /* conceal intent / strip the enemy's reconnaissance */
    STRATEGY_ASYMMETRIC,     /* refuse their strength, fight in a different domain */

    /* ── Extended canon (researched: ancient → modern doctrine) ──────────── */
    STRATEGY_OTHISMOS,                /* hoplite shield-shove */
    STRATEGY_HAMMER_AND_ANVIL,        /* pin + decisive shock arm */
    STRATEGY_PELTAST_SOFTENING,       /* skirmish-attrit before contact */
    STRATEGY_MANIPULAR_TACTICS,       /* flexible legion maniples */
    STRATEGY_CIRCUMVALLATION,         /* double siege ring */
    STRATEGY_CASTRAMETATION,          /* fortified marching camps */
    STRATEGY_WIN_WITHOUT_FIGHTING,    /* Sun Tzu: prevail before battle */
    STRATEGY_BESIEGE_WEI_TO_RESCUE_ZHAO,/* strike what they must defend */
    STRATEGY_FEIGNED_RETREAT,         /* lure pursuit, wheel and counter */
    STRATEGY_TULUGHMA,                /* hold + double flank envelop */
    STRATEGY_CHEVAUCHEE,              /* raid to burn economy/morale */
    STRATEGY_CASTELLATION,            /* network of mutually-supporting forts */
    STRATEGY_FLEET_IN_BEING,          /* intact reserve as deterrent */
    STRATEGY_GUERRE_DE_COURSE,        /* commerce raiding */
    STRATEGY_CROSSING_THE_T,          /* rake the line broadside */
    STRATEGY_BREAKING_THE_LINE,       /* pierce + isolate segments */
    STRATEGY_WOLFPACK,                /* coordinated massed pack attack */
    STRATEGY_CENTRAL_POSITION,        /* split foes, beat in sequence */
    STRATEGY_GRAND_BATTERY,           /* mass artillery at one point */
    STRATEGY_HARD_WAR,                /* target enemy war economy/will */
    STRATEGY_ANACONDA_PLAN,           /* blockade + constrict */
    STRATEGY_STORMTROOPER_TACTICS,    /* infiltrate, bypass strongpoints */
    STRATEGY_ELASTIC_DEFENSE,         /* yield, absorb, then counter */
    STRATEGY_BITE_AND_HOLD,           /* seize limited, consolidate, repeat */
    STRATEGY_AUFTRAGSTAKTIK,          /* mission command, decentralized initiative */
    STRATEGY_DEEP_OPERATION,          /* simultaneous depth, shatter rear */
    STRATEGY_KESSELSCHLACHT,          /* cauldron: encircle and annihilate */
    STRATEGY_MASKIROVKA,              /* deception + concealment doctrine */
    STRATEGY_DOUHET_DOCTRINE,         /* strategic bombing of will */
    STRATEGY_MAOIST_PROTRACTED_WAR,   /* phased insurgency, outlast */
    STRATEGY_CLEAR_HOLD_BUILD,        /* COIN: secure, hold, develop */
    STRATEGY_MUTUAL_ASSURED_DESTRUCTION,/* deterrence by guaranteed retaliation */
    STRATEGY_FLEXIBLE_RESPONSE,       /* graduated, proportionate escalation */
    STRATEGY_REFLEXIVE_CONTROL,       /* shape the enemy's own decisions */
    STRATEGY_HYBRID_WARFARE,          /* blend conventional/irregular/info */
    STRATEGY_NETWORK_CENTRIC_WARFARE, /* info-linked distributed force */
    STRATEGY_COUNT,
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
    int             deps[TALONS_MAX_DEPS]; /* goal ids that must be captured first */
    int             dep_count;
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
    double          strategy_success[STRATEGY_COUNT];    /* per strategy_type_t */
    int             strategy_attempts[STRATEGY_COUNT];
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

/* ── Dependencies & scheduling ────────────────────────────────────────────
 * Declare that `goal_id` cannot be struck until `dep_goal_id` is captured.
 * Returns false if either id is unknown or the dep table is full. */
bool talons_goal_depends_on(talons_engine_t *t, int goal_id, int dep_goal_id);

/* True if every prerequisite of `goal_id` is captured (or it has none). */
bool talons_goal_deps_met(const talons_engine_t *t, int goal_id);

/* Move any BLOCKED goal whose prerequisites are now all captured back to
 * STALKING. Returns the number of goals unblocked. */
int talons_resolve_blocked(talons_engine_t *t);

/* Periodic engine tick at wall-clock `now` (epoch seconds; pass 0 to use the
 * current time). Resolves blocked goals, escalates grip as deadlines approach,
 * and force-escapes goals whose deadline has passed. Returns the number of
 * goals whose state/grip changed. */
int talons_tick(talons_engine_t *t, double now);

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

/* Case-insensitive name → strategy. Returns `fallback` if unrecognized. */
strategy_type_t talons_strategy_from_name(const char *name, strategy_type_t fallback);

/* ── VFS Persistence ──────────────────────────────────────────────────── */

struct vfs_db;
typedef struct vfs_db vfs_db_t;

void talons_set_vfs(vfs_db_t *vfs);
void talons_persist_strategy_history(void);
void talons_restore_strategy_history(talons_engine_t *t);

#endif
