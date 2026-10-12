#include "seat_take.h"

#include <stdio.h>
#include <string.h>

/* seat_take.c — pure value-capture decision engine. See seat_take.h for the
 * model. No globals, no I/O, no provider knowledge: same inputs, same outputs,
 * trivially unit-testable. The economics mirror the swarm_router revenue model
 * (margin floors, blended COGS) but at the *seat* layer: dsco orchestrates a
 * customer's existing external seats and captures a take on the value routed,
 * using routing-driven savings as the wedge that makes the take defensible. */

void seat_take_default_policy(seat_take_policy_t *policy) {
    if (!policy) return;
    policy->take_rate = SEAT_TAKE_DEFAULT_TAKE_RATE;
    policy->routing_savings_frac = SEAT_TAKE_DEFAULT_ROUTING_SAVINGS;
    policy->managed_agents_price_usd_mo = SEAT_TAKE_DEFAULT_MA_PRICE_USD_MO;
    policy->managed_agents_take_rate = SEAT_TAKE_DEFAULT_MA_TAKE_RATE;
}

double seat_take_seat_spend_usd(const seat_plan_t *plans, size_t n_plans) {
    double total = 0.0;
    if (!plans) return 0.0;
    for (size_t i = 0; i < n_plans; i++) {
        if (plans[i].seats <= 0 || plans[i].plan_price_usd_mo < 0.0) continue;
        total += (double)plans[i].seats * plans[i].plan_price_usd_mo;
    }
    return total;
}

/* The value dsco orchestrated through the seats for a period. This is the
 * larger of (a) the avoided cost the routing produced and (b) the customer's
 * blended metered COGS for the work — the take applies to real work, and an
 * idle account (no routed work) orchestrates nothing and is charged nothing.
 * Avoided cost is the honest measure when routing is active; blended COGS is
 * the floor so a period with work but no measured savings still captures. */
static double value_orchestrated(const seat_activity_t *a) {
    double avoided = a->cogs_avoided_usd;
    double cogs = a->blended_cogs_usd;
    double v = avoided > cogs ? avoided : cogs;
    return v > 0.0 ? v : 0.0;
}

bool seat_take_compute(const seat_plan_t *plans, size_t n_plans,
                       const seat_activity_t *activity,
                       const seat_take_policy_t *policy,
                       seat_take_result_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    seat_take_policy_t p;
    if (policy) {
        p = *policy;
    } else {
        seat_take_default_policy(&p);
    }
    /* Clamp the take and savings into sane bounds; never trust the caller to
     * pass a rate that would over-capture or produce a negative benefit. */
    if (p.take_rate < 0.0) p.take_rate = 0.0;
    if (p.take_rate > 1.0) p.take_rate = 1.0;
    if (p.routing_savings_frac < 0.0) p.routing_savings_frac = 0.0;
    if (p.routing_savings_frac > 1.0) p.routing_savings_frac = 1.0;

    double spend = seat_take_seat_spend_usd(plans, n_plans);
    out->seat_spend_usd_mo = spend;

    /* No activity -> an idle account. Capture nothing, recommend nothing. */
    if (!activity) {
        out->effective_take_rate = 0.0;
        return true;
    }

    out->value_orchestrated_usd = value_orchestrated(activity);

    /* The take on value-orchestrated. */
    out->captured_usd = out->value_orchestrated_usd * p.take_rate;
    out->effective_take_rate =
        out->value_orchestrated_usd > 0.0
            ? out->captured_usd / out->value_orchestrated_usd
            : 0.0;

    /* The wedge: routing saves the customer a fraction of their seat spend by
     * not wasting seat-tokens on always-strong routing. The take is defensible
     * only while the net benefit to the customer stays positive. */
    out->routing_savings_usd = spend * p.routing_savings_frac;
    out->net_customer_benefit_usd = out->routing_savings_usd - out->captured_usd;

    /* Managed Agents funnel: the equivalent cost of running the same seats on
     * the first-party runtime, and the incremental capture if they upgrade.
     * MA replaces the per-seat vendor bill with a (cheaper) runtime price and
     * applies the expanded take to the value, so the customer sees a lower
     * total while dsco captures more — the classic win-win repositioning. */
    int total_seats = 0;
    if (plans) {
        for (size_t i = 0; i < n_plans; i++)
            if (plans[i].seats > 0) total_seats += plans[i].seats;
    }
    out->ma_equivalent_cost_usd =
        (double)total_seats * p.managed_agents_price_usd_mo;
    double ma_capture = out->value_orchestrated_usd * p.managed_agents_take_rate;
    out->ma_incremental_capture_usd = ma_capture - out->captured_usd;

    /* Recommend the upgrade when the customer's all-in cost on Managed Agents
     * (runtime price + expanded take) beats their current all-in cost (vendor
     * seat bill minus routing savings), i.e. it is genuinely the better deal —
     * the funnel must be honest or it erodes the trust the take depends on. */
    double current_all_in = spend - out->routing_savings_usd + out->captured_usd;
    double ma_all_in = out->ma_equivalent_cost_usd + ma_capture;
    out->ma_upgrade_recommended =
        total_seats > 0 && ma_all_in < current_all_in;

    return true;
}

char *seat_take_result_json(const seat_take_result_t *r, char *buf, size_t cap) {
    if (!r || !buf || cap < 2) return NULL;
    int n = snprintf(
        buf, cap,
        "{\"seat_spend_usd_mo\":%.2f,"
        "\"value_orchestrated_usd\":%.2f,"
        "\"captured_usd\":%.2f,"
        "\"effective_take_rate\":%.4f,"
        "\"routing_savings_usd\":%.2f,"
        "\"net_customer_benefit_usd\":%.2f,"
        "\"ma_equivalent_cost_usd\":%.2f,"
        "\"ma_incremental_capture_usd\":%.2f,"
        "\"ma_upgrade_recommended\":%s}",
        r->seat_spend_usd_mo, r->value_orchestrated_usd, r->captured_usd,
        r->effective_take_rate, r->routing_savings_usd,
        r->net_customer_benefit_usd, r->ma_equivalent_cost_usd,
        r->ma_incremental_capture_usd,
        r->ma_upgrade_recommended ? "true" : "false");
    if (n < 0 || (size_t)n >= cap) {
        buf[cap - 1] = '\0';
        return NULL;
    }
    return buf;
}
