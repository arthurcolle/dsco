#ifndef DSCO_SPEND_GOVERNOR_H
#define DSCO_SPEND_GOVERNOR_H

/* spend_governor.h — graduated cost/context control plane.
 *
 * Problem: budgets defaulted to OFF and the only enforcement was a binary
 * hard-stop at 100%. Everything between $0 and the cliff ran at maximum
 * parameters (effort, output caps, tool schemas, full history, 5m cache).
 *
 * This module is a pure decision engine: feed it the observable spend/context
 * signals for the session, get back a parameter plan for the next turn.
 * It has no globals, no I/O, and no provider knowledge, so it is trivially
 * unit-testable and callable from agent.c, swarm children, and sub-agents.
 *
 * Phases (by the most-constrained of session/daily budget ratio and
 * turn-runway):
 *   GREEN     < 50%   — full parameters. Do the work.
 *   YELLOW    50–75%  — trim stale tool results, mild tool-page pressure.
 *   ORANGE    75–90%  — cap effort at medium, halve output budget, tighter
 *                        trimming, recommend a model downshift for leaf work.
 *   RED       90–100% — effort low, quarter output budget, aggressive trim +
 *                        binary strip, strong downshift recommendation.
 *   EXHAUSTED ≥ 100%  — block the turn (same contract as check_cost_budget).
 *
 * Quality-critical work (governance/doctrine/self-modification) must not be
 * silently degraded in ORANGE/RED. The governor preserves reasoning/output
 * quality and asks the caller to checkpoint user intent before spending.
 *
 * Cache economics: the governor watches the session cache hit ratio and the
 * inter-turn cadence. Slow cadence (> ~4 min between requests) with a poor
 * hit ratio means 5-minute cache entries are expiring between turns — it then
 * recommends the 1-hour TTL (2x write, 0.1x read: breaks even after one hit).
 */

#include <stdbool.h>

typedef enum {
    SPEND_GREEN = 0,
    SPEND_YELLOW,
    SPEND_ORANGE,
    SPEND_RED,
    SPEND_EXHAUSTED,
} spend_phase_t;

/* Observable inputs. Zero-init is safe: unknown signals read as "no data"
 * and the governor degrades to budget-ratio-only behavior. */
typedef struct {
    double session_spent_usd;
    double session_budget_usd; /* 0 = unlimited */
    double daily_spent_usd;    /* cross-session (baseline DB) + session */
    double daily_budget_usd;   /* 0 = unlimited */
    double last_turn_cost_usd; /* most recent turn, for runway estimate */
    double avg_turn_cost_usd;  /* rolling average, 0 = unknown */
    double cache_hit_ratio;    /* session-level, 0..1 */
    bool cache_telemetry_seen; /* any cache read/write observed yet */
    double avg_turn_interval_sec; /* wall-clock between requests, 0 = unknown */
    int context_used_tokens;
    int context_window_tokens; /* effective (output-aware) window */
    int turns;
    bool quality_critical_work; /* high-stakes self-mod/governance work */
} spend_signals_t;

/* Parameter plan for the next turn. */
typedef struct {
    spend_phase_t phase;
    /* Budget pressure 0..1+ (max of session/daily ratios; >1 = over). */
    double pressure;
    /* Estimated turns remaining at current burn (-1 = unknown/unlimited). */
    double runway_turns;
    /* Effort ceiling: "" = leave session effort untouched, otherwise the
     * maximum effort tier the session should use this turn. Only ever
     * downshifts — never raises a user-chosen lower tier. */
    char effort_ceiling[16];
    /* Tool paging budget ratio 0..1 (drives adaptive tool-set sizing). */
    float tool_budget_ratio;
    /* Output token cap for this turn. 0 = model default. */
    int max_output_tokens;
    /* Conversation hygiene. */
    bool trim_old_results;  /* conv_trim_old_results(...) */
    int trim_keep_recent;   /* messages kept verbatim */
    int trim_max_chars;     /* older tool results clipped to this */
    bool strip_binaries;    /* conv_strip_binaries(...) */
    /* Cache economics. */
    bool recommend_1h_cache; /* slow cadence + poor hit ratio on 5m TTL */
    /* Escalation. */
    bool suggest_model_downshift; /* leaf-work should move to a cheaper model */
    bool preserve_quality;        /* don't downshift/cap reasoning silently */
    bool require_user_checkpoint; /* pause before high-stakes spend */
    bool block_turn;              /* EXHAUSTED: refuse to spend */
    char reason[192];             /* one-line human explanation */
} spend_plan_t;

/* Pure: signals in, plan out. Never blocks below 100% of any budget. */
spend_plan_t spend_governor_plan(const spend_signals_t *sig);

/* Learned-behavior adjustment from the usage-stats strategy weights.
 * Pure and tighten-only: may enable trimming, the 1h-cache recommendation,
 * or the downshift suggestion earlier than the phase defaults — never
 * loosens a plan. Default (unlearned) weights leave the plan unchanged. */
typedef struct {
    double cache_aggressiveness;      /* >0.6 → recommend 1h TTL on poor hit ratio */
    double model_cost_sensitivity;    /* >0.7 → downshift suggestion from YELLOW   */
    double context_compaction_thresh; /* context ratio that triggers early trim    */
} spend_learned_t;

void spend_plan_apply_learned(spend_plan_t *plan, const spend_learned_t *lw,
                              const spend_signals_t *sig);

/* Effort tier ranking for downshift-only comparisons.
 * none < minimal < low < medium < high < xhigh < max. Unknown/auto = high. */
int spend_effort_rank(const char *effort);

/* True when `candidate` is a strictly lower tier than `current`
 * (both non-empty wire values; auto/empty treated as high). */
bool spend_effort_is_downshift(const char *current, const char *candidate);

/* Default budget policy: if neither session nor daily budget was configured,
 * return the built-in safety defaults (out params). Explicitly-configured
 * zeros ("off") must be respected by the caller — pass had_explicit=true to
 * get no defaults. Values overridable via DSCO_DEFAULT_SESSION_BUDGET /
 * DSCO_DEFAULT_DAILY_BUDGET; DSCO_NO_DEFAULT_BUDGET=1 disables entirely. */
void spend_governor_default_budgets(bool had_explicit, double *session_usd,
                                    double *daily_usd);

const char *spend_phase_label(spend_phase_t phase);

#endif /* DSCO_SPEND_GOVERNOR_H */
