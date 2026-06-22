#ifndef DSCO_SELF_IMPROVE_H
#define DSCO_SELF_IMPROVE_H

#include <stdbool.h>
#include <stddef.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Self-Improvement Loop System
 *
 * A meta-cognitive layer that observes agent performance across turns and
 * sessions, identifies patterns, and generates actionable improvements.
 *
 * Three-loop design:
 *
 *   LOOP 1 — Turn-level (micro): After each turn, record metrics,
 *            detect immediate inefficiencies (redundant calls, failures).
 *
 *   LOOP 2 — Session-level (meso): Every N turns or at session end,
 *            consolidate patterns, update strategy weights, emit
 *            improvement suggestions to semantic memory.
 *
 *   LOOP 3 — Cross-session (macro): At startup, load historical
 *            learnings and apply them as priors for the current session.
 *
 * Integration points:
 *   - agent.c: self_improve_record_turn() after each turn transition
 *   - agent.c: self_improve_consolidate() at session end
 *   - main.c: self_improve_load_history() at startup
 *   - tools.c: self_improve tool + self_assess tool exposed to LLM
 *
 * Safety:
 *   - All improvements are advisory (suggestions, not auto-patches)
 *   - Improvements are logged to baseline for audit
 *   - Strategy weights are bounded [0.0, 1.0]
 *   - No code mutation — this is observation + adaptation, not self-modification
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SI_MAX_TOOLS_TRACKED    64
#define SI_MAX_PATTERNS         32
#define SI_MAX_SUGGESTIONS      16
#define SI_MAX_PATTERN_LEN      256
#define SI_MAX_SUGGESTION_LEN   512
#define SI_MAX_TOOL_NAME        64
#define SI_HISTORY_FILE         "~/.dsco/self_improve.json"
#define SI_CONSOLIDATION_INTERVAL 5  /* turns between meso-loop runs */

/* ── Tool performance tracking ─────────────────────────────────────────── */

typedef struct {
    char    name[SI_MAX_TOOL_NAME];
    int     calls;
    int     successes;
    int     failures;
    double  total_latency_ms;
    double  total_input_tokens;   /* estimated tokens consumed by this tool's results */
    int     consecutive_failures;
    double  last_latency_ms;
    double  best_latency_ms;
    double  worst_latency_ms;
    double  success_rate;         /* derived: successes / calls */
    double  efficiency_score;     /* 0.0-1.0: success_rate * speed_factor */
} si_tool_metric_t;

/* ── Detected patterns ────────────────────────────────────────────────── */

typedef enum {
    SI_PATTERN_NONE = 0,
    SI_PATTERN_REDUNDANT_CALLS,       /* same tool called 3+ times with similar input */
    SI_PATTERN_FAILING_TOOL,          /* tool success rate < 50% */
    SI_PATTERN_SLOW_TOOL,             /* avg latency > 5s */
    SI_PATTERN_TOOL_SPAM,             /* 10+ calls to same tool in one turn */
    SI_PATTERN_RETRY_STORM,           /* 3+ retries of same tool in sequence */
    SI_PATTERN_HIGH_COST_TURN,        /* turn cost > $0.50 */
    SI_PATTERN_CONTEXT_PRESSURE,      /* context usage > 80% */
    SI_PATTERN_BUDGET_APPROACHING,   /* approaching cost budget */
    SI_PATTERN_SUCCESSFUL_STRATEGY,   /* strategy that worked well */
} si_pattern_type_t;

typedef struct {
    si_pattern_type_t  type;
    char               description[SI_MAX_PATTERN_LEN];
    char               tool_name[SI_MAX_TOOL_NAME];
    double             severity;       /* 0.0-1.0 */
    int                occurrences;    /* how many times seen */
    double             first_seen;     /* timestamp */
    double             last_seen;
} si_pattern_t;

/* ── Improvement suggestions ──────────────────────────────────────────── */

typedef enum {
    SI_SUGGEST_NONE = 0,
    SI_SUGGEST_PREFER_ALTERNATIVE,    /* use tool B instead of A */
    SI_SUGGEST_REDUCE_CALLS,           /* batch or cache tool calls */
    SI_SUGGEST_INCREASE_TIMEOUT,       /* tool is slow but works */
    SI_SUGGEST_DISABLE_TOOL,           /* tool is broken/failing */
    SI_SUGGEST_BATCH_INPUT,            /* combine multiple calls */
    SI_SUGGEST_CACHE_RESULTS,          /* enable caching for deterministic tool */
    SI_SUGGEST_ADJUST_MODEL,           /* switch model for cost/quality */
    SI_SUGGEST_ADJUST_BUDGET,          /* increase/decrease budget */
    SI_SUGGEST_PROMPT_ENGINEERING,    /* system prompt adjustment */
    SI_SUGGEST_STRATEGY_SHIFT,         /* change execution strategy */
} si_suggestion_type_t;

typedef struct {
    si_suggestion_type_t  type;
    char                 description[SI_MAX_SUGGESTION_LEN];
    char                 target_tool[SI_MAX_TOOL_NAME];
    char                 alternative_tool[SI_MAX_TOOL_NAME];
    double               confidence;      /* 0.0-1.0 */
    double               estimated_savings; /* estimated $ savings or time savings */
    bool                 applied;          /* has user/agent acknowledged this? */
} si_suggestion_t;

/* ── Strategy weights (adaptive) ──────────────────────────────────────── */

typedef struct {
    double  parallel_preference;       /* 0.0=serial, 1.0=always parallel */
    double  cache_aggressiveness;     /* 0.0=no cache, 1.0=cache everything */
    double  model_cost_sensitivity;   /* 0.0=quality first, 1.0=cost first */
    double  tool_timeout_aggression;   /* 0.0=patient, 1.0=aggressive timeout */
    double  context_compaction_thresh; /* 0.0-1.0: when to compact */
    double  batch_willingness;         /* 0.0=one-at-a-time, 1.0=always batch */
} si_strategy_weights_t;

/* ── Self-improvement state ───────────────────────────────────────────── */

typedef struct {
    /* Turn-level metrics */
    si_tool_metric_t   tools[SI_MAX_TOOLS_TRACKED];
    int                tool_count;
    int                current_turn;
    int                turns_since_consolidation;

    /* Per-turn tracking */
    char               last_tool_called[SI_MAX_TOOL_NAME];
    int                same_tool_streak;
    double             turn_cost;
    double             turn_start_time;
    int                turn_tool_calls;
    int                turn_failures;

    /* Session-level metrics */
    double             session_start_time;
    double             total_cost;
    int                total_turns;
    int                total_tool_calls;
    int                total_failures;
    int                total_retries;
    int                total_redundant_calls;

    /* Patterns & suggestions */
    si_pattern_t       patterns[SI_MAX_PATTERNS];
    int                pattern_count;
    si_suggestion_t    suggestions[SI_MAX_SUGGESTIONS];
    int                suggestion_count;

    /* Adaptive strategy */
    si_strategy_weights_t weights;

    /* Cross-session learnings */
    int                sessions_seen;
    int                improvements_applied;
    double             historical_accuracy;  /* % of suggestions that helped */

    /* State flags */
    bool               initialized;
    bool               history_loaded;
} self_improve_t;

/* ── Public API ───────────────────────────────────────────────────────── */

/* Initialize the self-improvement system. Called once at startup. */
void self_improve_init(self_improve_t *si);

/* LOOP 1 (micro): Record a single tool execution within a turn. */
void self_improve_record_tool(self_improve_t *si,
                              const char *tool_name,
                              bool success,
                              double latency_ms,
                              int estimated_tokens);

/* LOOP 1 (micro): Record turn-level metrics after a turn completes. */
void self_improve_record_turn(self_improve_t *si,
                              int turn_number,
                              double turn_cost,
                              int input_tokens,
                              int output_tokens,
                              int context_usage_pct,
                              double budget_used_pct);

/* LOOP 2 (meso): Consolidate patterns and generate suggestions.
 * Called every SI_CONSOLIDATION_INTERVAL turns or at session end.
 * Returns the number of new suggestions generated. */
int self_improve_consolidate(self_improve_t *si);

/* LOOP 3 (macro): Load historical learnings from disk.
 * Called at startup to apply cross-session improvements. */
bool self_improve_load_history(self_improve_t *si);

/* Save current learnings to disk for future sessions. */
bool self_improve_save_history(const self_improve_t *si);

/* Get a human-readable summary of current performance and suggestions.
 * Caller provides buffer; returns pointer to same buffer. */
const char *self_improve_summary(const self_improve_t *si,
                                  char *buf, size_t buf_len);

/* Get the current strategy weights (for agent loop to consult). */
const si_strategy_weights_t *self_improve_weights(const self_improve_t *si);

/* Mark a suggestion as applied (acknowledged by agent or user). */
void self_improve_acknowledge(self_improve_t *si, int suggestion_idx);

/* Get tool efficiency score for a specific tool (0.0-1.0).
 * Returns 0.5 (neutral) for untracked tools. */
double self_improve_tool_score(const self_improve_t *si, const char *tool_name);

/* Check if a tool is currently in a failure streak.
 * Returns true if the tool has failed 3+ times consecutively. */
bool self_improve_tool_failing(const self_improve_t *si, const char *tool_name);

/* Record a redundant call (same tool, similar input, within same turn). */
void self_improve_record_redundant(self_improve_t *si, const char *tool_name);

/* Get the recommended alternative for a tool, if any.
 * Returns true if an alternative was found, fills alt_name. */
bool self_improve_suggest_alternative(const self_improve_t *si,
                                       const char *tool_name,
                                       char *alt_name, size_t alt_len);

/* Reset per-turn counters (call at start of each turn). */
void self_improve_turn_reset(self_improve_t *si);

/* ── Tool implementations (for tools.c registration) ──────────────────── */

/* Tool: self_improve — Run the self-improvement loop and return suggestions.
 * Input: { "action": "summary"|"consolidate"|"acknowledge"|"history"|"save"|
 *                    "curriculum"|"skill"|"promotion_gate",
 *           "suggestion_id": N (for acknowledge),
 *           "skill_id": "SP07" (for skill),
 *           ...promotion metrics for promotion_gate }
 * Output: summary text or JSON with suggestions, patterns, strategy weights,
 *         RSI curriculum, and promotion gate decisions. */
bool tool_self_improve(const char *input_json, char *result, size_t result_len);

/* Tool: self_assess — Quick self-evaluation of current session performance.
 * No input required. Returns efficiency score, top issues, and recommendations. */
bool tool_self_assess(const char *input_json, char *result, size_t result_len);

/* ── Global instance ──────────────────────────────────────────────────── */

extern self_improve_t g_self_improve;

/* Convenience macros for agent.c integration */
#define SI_RECORD_TOOL(name, ok, ms, tokens) \
    self_improve_record_tool(&g_self_improve, (name), (ok), (ms), (tokens))

#define SI_RECORD_TURN(n, cost, in_tok, out_tok, ctx_pct, budget_pct) \
    self_improve_record_turn(&g_self_improve, (n), (cost), (in_tok), \
                             (out_tok), (ctx_pct), (budget_pct))

#define SI_CONSOLIDATE() \
    self_improve_consolidate(&g_self_improve)

#define SI_TURN_RESET() \
    self_improve_turn_reset(&g_self_improve)

#endif /* DSCO_SELF_IMPROVE_H */

/* ── Goal / swarm / strategy hooks (new) ─────────────────────────────── */

void self_improve_record_goal_state(self_improve_t *si,
                                    const char *goal_id,
                                    int state,   /* 0=nascent,1=stalking,2=striking,3=gripping,4=captured,5=escaped */
                                    int grip_strength,
                                    double elapsed_s);

void self_improve_record_swarm_outcome(self_improve_t *si,
                                       const char *topology,
                                       int agents,
                                       bool success,
                                       double quality,
                                       double elapsed_s);

void self_improve_record_strategy_result(self_improve_t *si,
                                         const char *strategy,
                                         const char *goal_type,
                                         bool success,
                                         int grip_escalations,
                                         double elapsed_s);

void self_improve_record_tournament_result(self_improve_t *si,
                                           const char *winner_strategy,
                                           int competitors,
                                           double margin,
                                           double elapsed_s);
