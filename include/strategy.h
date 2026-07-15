#ifndef DSCO_STRATEGY_H
#define DSCO_STRATEGY_H

/* strategy.h — strategic_assess: model-invocable deliberative strategy layer.
 *
 * executive.c actuates bounded SESSION decisions (budget, pause, exit).
 * frontier.c observes whether spend is efficient. This module sits above
 * both: it gives the model a first-class tool for STRATEGIC reasoning —
 * durable objectives, explicit option evaluation, bounded bet sizing, and
 * pivot/persevere verdicts — so strategy is auditable state, not vibes.
 *
 * Actions (input {"action": "...", ...}):
 *   set_objective    — register/update an objective: name, acceptance
 *                      criteria, priority 1..5, budget_usd (0 = none)
 *   list_objectives  — dump the objective register with status
 *   complete_objective / abandon_objective — terminal-state an objective
 *                      (abandon requires a substantive reason)
 *   size_bet         — bounded Kelly-style allocation: given win probability
 *                      p, payoff ratio b, and bankroll, returns a HALF-Kelly
 *                      fraction clamped to [0, STRAT_BET_FRACTION_MAX].
 *                      Never advises betting on p <= 1/(b+1) (negative edge).
 *   pivot_check      — pivot/persevere verdict for an objective from spend
 *                      fraction vs. progress fraction (+ optional frontier
 *                      score via hook): persevere / warn / pivot.
 *
 * Pure core + injected hooks (audit sink, frontier snapshot); no direct
 * knowledge of agent.c internals. Unit-testable without a session.
 */

#include <stdbool.h>
#include <stddef.h>

#define STRAT_MAX_OBJECTIVES 32
#define STRAT_MAX_OPTIONS 8
#define STRAT_NAME_MAX 96
#define STRAT_TEXT_MAX 256
#define STRAT_BET_FRACTION_MAX 0.25 /* never allocate >25% of bankroll */

typedef enum {
    STRAT_OBJ_ACTIVE = 0,
    STRAT_OBJ_COMPLETE,
    STRAT_OBJ_ABANDONED,
} strat_obj_status_t;

typedef struct {
    char name[STRAT_NAME_MAX];
    char acceptance[STRAT_TEXT_MAX]; /* falsifiable acceptance criteria */
    int priority;                    /* 1 (highest) .. 5 */
    double budget_usd;               /* 0 = no explicit budget */
    double spent_usd;                /* advisory, updated via pivot_check */
    double progress;                 /* 0..1, updated via pivot_check */
    strat_obj_status_t status;
    char status_reason[STRAT_TEXT_MAX];
} strat_objective_t;

typedef struct {
    /* Optional frontier snapshot (score 0..1, on_frontier). */
    bool (*frontier_snapshot)(double *score, bool *on_frontier, char *summary,
                              size_t summary_len);
    /* Audit sink (category, title, detail). */
    void (*audit)(const char *category, const char *title, const char *detail);
} strategy_hooks_t;

void strategy_set_hooks(const strategy_hooks_t *hooks);

/* Reset all objective state (tests / session init). */
void strategy_reset(void);

/* Parse + validate + execute. Writes a JSON result. Returns true when the
 * action was accepted. */
bool strategy_assess(const char *input_json, char *result, size_t rlen);

/* Introspection for tests. */
int strategy_objective_count(void);
const strat_objective_t *strategy_objective_at(int idx);

/* Pure helpers, exposed for tests. */
double strategy_kelly_fraction(double p, double b); /* clamped half-Kelly */

/* Register the strategic_assess external tool (host calls once at session
 * start, alongside executive wiring). */
void strategy_register_tool(void);

#endif /* DSCO_STRATEGY_H */
