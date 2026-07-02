#ifndef DSCO_COST_BUDGET_H
#define DSCO_COST_BUDGET_H

/* cost_budget.h — tiny header-only helpers for dollar-budget preflight.
 *
 * This intentionally does not own policy or UI.  It only normalizes the
 * current spend / cap / remaining arithmetic so agent, router, and swarm gates
 * can make the same decision before dispatching paid work.
 */

#include <stdbool.h>
#include "config.h"
#include "llm.h"

typedef struct {
    double session_spent_usd;
    double session_budget_usd;      /* 0 = unlimited */
    double session_remaining_usd;   /* large sentinel when unlimited */
    double daily_spent_usd;         /* caller supplies ledger/chronicle value */
    double daily_budget_usd;        /* 0 = unlimited */
    double daily_remaining_usd;     /* large sentinel when unlimited */
    double effective_remaining_usd; /* min(session_remaining, daily_remaining) */
    bool   session_limited;
    bool   daily_limited;
    bool   exhausted;
} cost_budget_snapshot_t;

static inline double cost_budget_session_spent_usd(const session_state_t *session) {
    if (!session)
        return 0.0;
    if (session->total_reported_cost_usd > 0.0)
        return session->total_reported_cost_usd;
    const model_info_t *mi = model_lookup(session->model);
    if (!mi)
        return 0.0;
    return session->total_input_tokens * mi->input_price / 1e6 +
           session->total_output_tokens * mi->output_price / 1e6 +
           session->total_cache_read_tokens * mi->cache_read_price / 1e6 +
           session->total_cache_write_tokens * mi->cache_write_price / 1e6;
}

static inline cost_budget_snapshot_t
cost_budget_snapshot(const session_state_t *session, double session_budget_usd,
                     double daily_spent_usd, double daily_budget_usd) {
    const double unlimited = 1.0e12;
    cost_budget_snapshot_t s;
    s.session_spent_usd = cost_budget_session_spent_usd(session);
    s.session_budget_usd = session_budget_usd > 0.0 ? session_budget_usd : 0.0;
    s.daily_spent_usd = daily_spent_usd > 0.0 ? daily_spent_usd : 0.0;
    s.daily_budget_usd = daily_budget_usd > 0.0 ? daily_budget_usd : 0.0;
    s.session_limited = s.session_budget_usd > 0.0;
    s.daily_limited = s.daily_budget_usd > 0.0;
    s.session_remaining_usd = s.session_limited ? s.session_budget_usd - s.session_spent_usd
                                                : unlimited;
    s.daily_remaining_usd = s.daily_limited ? s.daily_budget_usd - s.daily_spent_usd
                                            : unlimited;
    s.effective_remaining_usd = s.session_remaining_usd < s.daily_remaining_usd
                                    ? s.session_remaining_usd
                                    : s.daily_remaining_usd;
    s.exhausted = (s.session_limited && s.session_remaining_usd <= 0.0) ||
                  (s.daily_limited && s.daily_remaining_usd <= 0.0);
    return s;
}

static inline bool cost_budget_can_spend(const cost_budget_snapshot_t *s,
                                         double estimated_cost_usd) {
    if (!s)
        return true;
    if (estimated_cost_usd <= 0.0)
        return !s->exhausted;
    return !s->exhausted && estimated_cost_usd <= s->effective_remaining_usd;
}

#endif /* DSCO_COST_BUDGET_H */
