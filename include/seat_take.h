#ifndef DSCO_SEAT_TAKE_H
#define DSCO_SEAT_TAKE_H

/* seat_take.h — external-seat orchestration and value-capture model.
 *
 * Business model (principal-stated, 2026-09): customers already pay for
 * consumer agent seats (ChatGPT Codex, Claude Max/Code) at roughly
 * $200/seat-month. dsco turns those existing seats into coordinated cloud
 * executors on the dsco substrate (router.distributed.systems + fleet-mesh),
 * and captures a take-rate on the *value orchestrated* — on the order of
 * 10-15% — because the routing intelligence saves the customer more than the
 * take costs (cheap-vs-strong per-query routing cuts wasted seat burn; see
 * the CDR finding, ~34% cost reduction on simple queries).
 *
 * The same take is the funnel: pricing external seat lanes so that the
 * first-party "Managed Agents" runtime (where dsco owns the agent loop, not
 * just the routing) is visibly the better deal. Seat executors are the
 * wedge; Managed Agents is the expansion.
 *
 * This module is a pure decision engine in the spend_governor tradition:
 * no globals, no I/O, no provider network knowledge. Feed it an account's
 * seat inventory and an observed period of orchestrated activity, get back
 * the value-capture and the managed-agents upgrade calculus. It is trivially
 * unit-testable and callable from tools, the router, and reporting.
 *
 * The take is computed on value-orchestrated, not on raw seat spend, so the
 * model never over-charges idle accounts: an executor that ran nothing
 * contributes nothing. The default rates are planning constants, not gospel;
 * they are explicit fields so the operator can move them without a rebuild.
 */

#include <stdbool.h>
#include <stddef.h>

/* A single external seat the customer brings (e.g. one Codex or Claude plan).
 * plan_price_usd_mo is what the *customer* pays the vendor per seat-month;
 * dsco does not see that money — it anchors the value the seat represents. */
typedef struct {
    char vendor[32];          /* "codex" | "claude" | free-form */
    char plan[48];            /* "pro" | "max" | "team" | ...   */
    int  seats;               /* count of identical seats       */
    double plan_price_usd_mo; /* vendor price per seat-month    */
} seat_plan_t;

/* Observed orchestrated activity for one seat class over a period. This is
 * what dsco actually routed/executed through those seats. */
typedef struct {
    double queries_routed;      /* queries dispatched to these seats      */
    double seat_tokens_mtok;    /* seat-tokens orchestrated (millions)    */
    double cogs_avoided_usd;    /* $ the routing saved vs always-strong   */
    double blended_cogs_usd;    /* dsco's metered cost to run the period  */
} seat_activity_t;

/* Per-plan economics knobs (defaults below are planning constants). */
typedef struct {
    double take_rate;             /* fraction of value-orchestrated captured  */
    double routing_savings_frac;  /* fraction of seat spend routing saves     */
    double managed_agents_price_usd_mo; /* first-party runtime price/seat-mo  */
    double managed_agents_take_rate;    /* expanded take inside Managed Agents */
} seat_take_policy_t;

/* Result of the value-capture computation for a period. */
typedef struct {
    double seat_spend_usd_mo;     /* customer's vendor seat bill (context)   */
    double value_orchestrated_usd;/* $ of work dsco routed through seats     */
    double captured_usd;          /* dsco's take for the period              */
    double effective_take_rate;   /* captured / value_orchestrated           */
    double routing_savings_usd;   /* $ routing saved the customer            */
    double net_customer_benefit_usd; /* savings minus the take (the wedge)   */
    /* Managed Agents funnel: */
    double ma_equivalent_cost_usd;    /* cost to run same seats on Managed Agents */
    double ma_incremental_capture_usd;/* extra $ if account upgrades to MA        */
    bool  ma_upgrade_recommended;     /* is MA the better deal for the customer?  */
} seat_take_result_t;

/* Planning constants (override via the policy struct, not by editing). */
#define SEAT_TAKE_DEFAULT_TAKE_RATE            0.125  /* 12.5% mid of 10-15%  */
#define SEAT_TAKE_DEFAULT_ROUTING_SAVINGS      0.30   /* CDR ~34%, be conservative */
#define SEAT_TAKE_DEFAULT_MA_PRICE_USD_MO      40.0   /* first-party runtime seat  */
#define SEAT_TAKE_DEFAULT_MA_TAKE_RATE         0.35   /* expansion take inside MA  */
#define SEAT_TAKE_DEFAULT_PLAN_PRICE_USD_MO    200.0  /* Codex / Claude Max seat   */

/* Fill a policy with the planning defaults. */
void seat_take_default_policy(seat_take_policy_t *policy);

/* Compute the value-capture for one account over a period.
 *
 *   plans:    the seat inventory (n_plans entries).
 *   activity: the observed orchestrated activity aggregated over the seats.
 *   policy:   economics knobs (NULL -> planning defaults).
 *   out:      result (required).
 *
 * Returns true on success; false on a NULL/bad argument. Pure: no I/O. */
bool seat_take_compute(const seat_plan_t *plans, size_t n_plans,
                       const seat_activity_t *activity,
                       const seat_take_policy_t *policy,
                       seat_take_result_t *out);

/* Convenience: monthly vendor seat spend for an inventory. */
double seat_take_seat_spend_usd(const seat_plan_t *plans, size_t n_plans);

/* Render a result as a compact JSON object into buf (for tool/report output).
 * Returns buf, or NULL on truncation. buf is always NUL-terminated on entry
 * cap >= 2. */
char *seat_take_result_json(const seat_take_result_t *r, char *buf, size_t cap);

#endif /* DSCO_SEAT_TAKE_H */
