#ifndef DSCO_PLAN_OPTIMIZER_H
#define DSCO_PLAN_OPTIMIZER_H

/* ── Plan-time topology optimizer ─────────────────────────────────────────
 *
 * Given a task string and optional budget, returns a ranked list of
 * topology options with cost + latency estimates so the user can see
 * cost before execution starts.
 *
 * Usage:
 *   plan_options_t *opts = plan_analyze("analyze MSFT and AAPL", 200);
 *   const plan_option_t *best = plan_options_best(opts);
 *   topology_run(topology_find(best->topology_name), ...);
 *   plan_options_free(opts);
 */

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    COST_SOURCE_STATIC  = 0,  /* from topology.est_cost_1k */
    COST_SOURCE_LEARNED = 1,  /* from cost_model weighted average */
} cost_source_t;

typedef struct {
    const char   *topology_name;    /* heap-allocated */
    double        fit_score;        /* 0-1 (higher = better match) */
    double        est_cost_usd;
    double        est_latency_s;
    bool          over_budget;
    cost_source_t cost_source;
    char          rationale[256];
} plan_option_t;

#define PLAN_MAX_OPTIONS 8

typedef struct {
    const char   *task;             /* heap-allocated */
    int           budget_cents;     /* 0 = no limit */
    plan_option_t options[PLAN_MAX_OPTIONS];
    int           count;
} plan_options_t;

/* Analyze task and return ranked options. Caller must free with plan_options_free(). */
plan_options_t       *plan_analyze(const char *task, int budget_cents);
void                  plan_options_free(plan_options_t *opts);
int                   plan_options_json(const plan_options_t *opts, char *buf, size_t buflen);
const plan_option_t  *plan_options_best(const plan_options_t *opts);

#endif /* DSCO_PLAN_OPTIMIZER_H */
