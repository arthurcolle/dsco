#ifndef DSCO_FRONTIER_H
#define DSCO_FRONTIER_H

/* frontier.h — Pareto-frontier efficiency ledger.
 *
 * A budget answers "how much have we spent?". The frontier ledger answers
 * "was each dollar necessary?" — i.e. are we ON the cost/progress frontier
 * (every token billed was required to produce the work) or OFF it (paying
 * for waste that produced nothing).
 *
 * Per turn, actual spend is decomposed into PRODUCTIVE cost plus five
 * quantified waste channels, each derived from signals we already have:
 *
 *   cache_waste    — prefix tokens re-WRITTEN because the cache missed.
 *                    In steady state a turn writes only its new suffix
 *                    (≈ prev output + fresh input). Excess write tokens
 *                    should have been 0.1x reads; they billed at 1.25x.
 *   retry_waste    — failed tool calls. Each failure forces a re-issue:
 *                    the failed result was resent and an extra round of
 *                    fresh input was burned.
 *   redundancy_waste — duplicate tool calls (same tool+input) whose
 *                    results are resent in history without new information.
 *   effort_waste   — reasoning tokens (billed as output) spent on turns
 *                    that produced trivial output and called no tools:
 *                    thinking hard about nothing.
 *   drag_waste     — fresh (uncached, full-price) input above the session's
 *                    running baseline: stale history being re-shipped.
 *
 * frontier score  = 1 − waste / total   (1.0 = on the frontier)
 * marginal cost   = $ per 1k productive output tokens, sliding window —
 *                   when this RISES while the success rate falls, spending
 *                   more is buying less: you have left the frontier and
 *                   the correct response is to change strategy, not merely
 *                   to spend slower.
 *
 * Pure module: no globals, no I/O. agent.c owns one ledger per session.
 */

#include <stdbool.h>

#define FRONTIER_WINDOW 32 /* sliding window of turns for marginal stats */

/* Prices for the active model ($ per 1M tokens). */
typedef struct {
    double in_usd;          /* full-price input */
    double out_usd;         /* output */
    double cache_read_usd;  /* ~0.1x in */
    double cache_write_usd; /* ~1.25x in */
} frontier_prices_t;

/* Observable facts about one completed turn. */
typedef struct {
    int input_tokens;       /* fresh, after last cache breakpoint */
    int output_tokens;
    int cache_read_tokens;
    int cache_write_tokens;
    int reasoning_tokens;   /* thinking (subset of output billing) */
    int tool_calls;
    int tool_failures;
    int duplicate_tool_calls; /* same tool+input seen before this session */
    int duplicate_result_tokens; /* est. tokens of duplicated results */
    bool produced_text;     /* non-trivial assistant text emitted */
    double reported_cost_usd; /* provider-reported turn cost, 0 = derive */
} frontier_turn_t;

/* Per-turn decomposition (all USD). */
typedef struct {
    double total;
    double productive;
    double cache_waste;
    double retry_waste;
    double redundancy_waste;
    double effort_waste;
    double drag_waste;
    double waste; /* sum of channels, clamped to total */
    double score; /* 1 − waste/total; 1.0 when total is 0 */
} frontier_decomp_t;

typedef struct {
    /* Cumulative session totals (USD). */
    double total_usd;
    double productive_usd;
    double cache_waste_usd;
    double retry_waste_usd;
    double redundancy_waste_usd;
    double effort_waste_usd;
    double drag_waste_usd;
    int turns;

    /* Sliding window for marginal statistics. */
    double win_cost[FRONTIER_WINDOW];
    int win_out_tokens[FRONTIER_WINDOW];
    int win_tool_calls[FRONTIER_WINDOW];
    int win_tool_failures[FRONTIER_WINDOW];
    double win_score[FRONTIER_WINDOW];
    int win_head; /* next slot */
    int win_fill;

    /* Baselines learned across the session. */
    double avg_fresh_input; /* EWMA of fresh input tokens per turn */
    int prev_output_tokens; /* previous turn's output (expected cache delta) */
} frontier_ledger_t;

/* Verdict from the sliding window. */
typedef struct {
    double score;              /* window-average frontier score */
    double marginal_usd_per_1k_out; /* window cost per 1k output tokens */
    double marginal_trend;     /* >0: cost/1k rising (leaving frontier) */
    double tool_success_rate;  /* window */
    bool on_frontier;          /* score ≥ 0.8 and trend not deteriorating */
    /* Largest waste channel this window (for the one-line diagnosis). */
    const char *dominant_waste;
    double dominant_waste_usd;
    char summary[224];
} frontier_verdict_t;

void frontier_init(frontier_ledger_t *l);

/* Record a completed turn; returns its decomposition. */
frontier_decomp_t frontier_record(frontier_ledger_t *l, const frontier_turn_t *t,
                                  const frontier_prices_t *p);

/* Evaluate the sliding window. */
frontier_verdict_t frontier_verdict(const frontier_ledger_t *l);

/* Session waste ratio 0..1 (cumulative). */
double frontier_waste_ratio(const frontier_ledger_t *l);

/* Render a multi-line report into buf (for /pareto). Returns buf. */
const char *frontier_report(const frontier_ledger_t *l, char *buf, int buf_len);

#endif /* DSCO_FRONTIER_H */
