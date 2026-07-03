#ifndef DSCO_EXECUTIVE_H
#define DSCO_EXECUTIVE_H

/* executive.h — executive_decision: model-invocable session control.
 *
 * The frontier ledger and spend governor OBSERVE efficiency; this module
 * gives the model (and the operator) an ACTUATOR: a first-class tool for
 * making bounded executive decisions about the session itself —
 *
 *   executive_decision(decision: "end_conversation",
 *                      reason:   "gap budget at $48")
 *
 * Decisions:
 *   end_conversation   — schedule a graceful agent-loop exit after this turn
 *   pause_spending     — force the spend governor to RED (effort low, output
 *                        quartered, aggressive hygiene) without ending work
 *   resume_spending    — lift a prior pause
 *   downshift_model    — request routing to a cheaper model for leaf work
 *   raise_budget       — raise the session budget (bounded: ≤ 2x current,
 *                        ≤ hard ceiling; requires quantified reason)
 *   lower_budget       — lower the session budget (always allowed)
 *   escalate_to_user   — stop autonomous progress and put a question to the
 *                        operator (renders reason prominently)
 *
 * Every decision is validated against live state (frontier verdict, budget
 * arithmetic), executed through hooks the host registers, and audited to
 * baseline.db (category=executive). Decisions the state contradicts are
 * REJECTED with the evidence (e.g. raise_budget while off the frontier:
 * "72% of recent spend was waste — fix the waste channel first").
 *
 * Pure core + injected hooks: executive.c has no direct knowledge of
 * agent.c internals and is unit-testable without a session.
 */

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    EXEC_END_CONVERSATION = 0,
    EXEC_PAUSE_SPENDING,
    EXEC_RESUME_SPENDING,
    EXEC_DOWNSHIFT_MODEL,
    EXEC_RAISE_BUDGET,
    EXEC_LOWER_BUDGET,
    EXEC_ESCALATE_TO_USER,
    EXEC_DECISION_COUNT,
} exec_decision_t;

/* Host hooks — all optional; missing hooks make the decision unavailable. */
typedef struct {
    /* Schedule a graceful exit after the current turn. */
    void (*request_exit)(const char *reason);
    /* Session budget accessors (USD; 0 = unlimited). */
    double (*get_session_budget)(void);
    void (*set_session_budget)(double usd);
    double (*get_session_spent)(void);
    /* Governor overrides. */
    void (*force_phase_red)(bool on);
    /* Model routing request (advisory). */
    void (*request_model_downshift)(const char *reason);
    /* Operator escalation: surface reason to the user prominently. */
    void (*escalate)(const char *question);
    /* Frontier snapshot (may be NULL). Fills score 0..1 and on_frontier. */
    bool (*frontier_snapshot)(double *score, bool *on_frontier, char *summary,
                              size_t summary_len);
    /* Audit sink (category, title, detail). */
    void (*audit)(const char *category, const char *title, const char *detail);
} executive_hooks_t;

/* Bounds for raise_budget. */
#define EXEC_RAISE_FACTOR_MAX 2.0   /* new budget ≤ 2x current */
#define EXEC_BUDGET_HARD_CEILING_DEFAULT 200.0 /* absolute cap, env-overridable */

/* Register host hooks (agent.c calls once at session start). */
void executive_set_hooks(const executive_hooks_t *hooks);

/* Parse + validate + execute. `input_json` carries
 *   {"decision": "...", "reason": "...", "amount_usd": <for budget ops>}
 * Writes a JSON result (status accepted|rejected|unavailable + evidence).
 * Returns true when the decision was ACCEPTED. */
bool executive_decide(const char *input_json, char *result, size_t rlen);

/* Tool-registry adapter (matches tool execute signature). */
bool tool_executive_decision(const char *input, char *result, size_t rlen);

/* Introspection for tests. */
exec_decision_t executive_parse_decision(const char *name, bool *ok);
const char *executive_decision_name(exec_decision_t d);

/* Whether a spending pause is currently in force. */
bool executive_spending_paused(void);

#endif /* DSCO_EXECUTIVE_H */
