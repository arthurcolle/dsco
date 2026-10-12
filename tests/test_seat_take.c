/* test_seat_take.c — unit tests for the seat value-capture engine.
 * Validates the principal's worked example: 50 Codex + 20 Claude seats at
 * $200/seat-month = $14,000 seat spend, with a 10–15% take on value
 * orchestrated, and the Managed Agents funnel economics. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "seat_take.h"

static int g_failures = 0;

#define CHECK(cond, msg) do {                                   \
        if (!(cond)) {                                          \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            g_failures++;                                       \
        }                                                       \
    } while (0)

static int approx(double a, double b) { return fabs(a - b) < 0.01; }

int main(void) {
    /* The principal's example: 50 Codex + 20 Claude plans, $200/seat-month. */
    seat_plan_t plans[2];
    memset(plans, 0, sizeof(plans));
    strcpy(plans[0].vendor, "codex");  strcpy(plans[0].plan, "pro");
    plans[0].seats = 50; plans[0].plan_price_usd_mo = 200.0;
    strcpy(plans[1].vendor, "claude"); strcpy(plans[1].plan, "max");
    plans[1].seats = 20; plans[1].plan_price_usd_mo = 200.0;

    /* Seat spend = 70 * 200 = $14,000. */
    double spend = seat_take_seat_spend_usd(plans, 2);
    CHECK(approx(spend, 14000.0), "seat spend is 70 * 200 = 14000");

    /* A period where routing produced measurable avoided cost. Use the
     * blended COGS as the value floor so the take lands on real work. */
    seat_activity_t activity = {
        .queries_routed = 120000.0,
        .seat_tokens_mtok = 400.0,
        .cogs_avoided_usd = 3000.0,   /* routing saved $3k vs always-strong */
        .blended_cogs_usd = 2500.0,   /* dsco's metered cost for the work   */
    };

    seat_take_result_t r;
    CHECK(seat_take_compute(plans, 2, &activity, NULL, &r),
          "compute succeeds with default policy");

    /* Value orchestrated = max(avoided=3000, cogs=2500) = 3000. */
    CHECK(approx(r.value_orchestrated_usd, 3000.0), "value orchestrated = max(avoided, cogs)");

    /* Default take 12.5% of 3000 = 375. Within the 10-15% band on value. */
    CHECK(approx(r.captured_usd, 375.0), "captured = 12.5% of value orchestrated");
    CHECK(r.effective_take_rate >= 0.10 && r.effective_take_rate <= 0.15,
          "effective take is within the 10-15% band");

    /* The wedge: routing savings 30% of 14000 = 4200; net benefit to the
     * customer = 4200 - 375 = 3825 > 0, so the take is defensible. */
    CHECK(approx(r.routing_savings_usd, 4200.0), "routing savings = 30% of seat spend");
    CHECK(r.net_customer_benefit_usd > 0.0, "customer net benefit stays positive");
    CHECK(approx(r.net_customer_benefit_usd, 3825.0), "net benefit = savings - take");

    /* Managed Agents: 70 seats at default $40 = $2800 runtime equivalent. */
    CHECK(approx(r.ma_equivalent_cost_usd, 2800.0), "MA equivalent = 70 * 40");
    /* Expanded take inside MA: 35% of 3000 = 1050; incremental over current
     * 375 = 675. */
    CHECK(approx(r.ma_incremental_capture_usd, 675.0), "MA incremental capture");
    /* current all-in = 14000 - 4200 + 375 = 10175; MA all-in = 2800 + 1050 =
     * 3850 < 10175, so the upgrade is recommended (honest better deal). */
    CHECK(r.ma_upgrade_recommended, "Managed Agents recommended when genuinely cheaper");

    /* Idle account: no activity -> capture nothing, recommend nothing. */
    seat_take_result_t idle;
    CHECK(seat_take_compute(plans, 2, NULL, NULL, &idle), "compute with no activity");
    CHECK(approx(idle.captured_usd, 0.0), "idle account captures nothing");
    CHECK(approx(idle.value_orchestrated_usd, 0.0), "idle account orchestrates nothing");

    /* Policy override: a 10% take and a more conservative 25% savings. */
    seat_take_policy_t p;
    seat_take_default_policy(&p);
    p.take_rate = 0.10;
    p.routing_savings_frac = 0.25;
    seat_take_result_t r2;
    CHECK(seat_take_compute(plans, 2, &activity, &p, &r2), "compute with custom policy");
    CHECK(approx(r2.captured_usd, 300.0), "custom 10% take on 3000");
    CHECK(approx(r2.routing_savings_usd, 3500.0), "custom 25% savings on 14000");

    /* Take rate is clamped to [0,1]. */
    p.take_rate = 5.0;
    CHECK(seat_take_compute(plans, 2, &activity, &p, &r2), "compute with absurd take");
    CHECK(r2.captured_usd <= r2.value_orchestrated_usd + 0.01,
          "take clamped so capture never exceeds value");

    /* JSON rendering round-trips the key fields. */
    char buf[512];
    CHECK(seat_take_result_json(&r, buf, sizeof(buf)) == buf, "json render succeeds");
    CHECK(strstr(buf, "\"captured_usd\":375.00") != NULL, "json contains captured_usd");
    CHECK(strstr(buf, "\"ma_upgrade_recommended\":true") != NULL, "json contains MA flag");

    /* Bad arguments are rejected, not crashes. */
    CHECK(!seat_take_compute(plans, 2, &activity, NULL, NULL), "NULL out rejected");
    CHECK(approx(seat_take_seat_spend_usd(NULL, 5), 0.0), "NULL plans -> 0 spend");

    if (g_failures) {
        fprintf(stderr, "seat_take: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("seat_take: all tests passed "
           "(70 seats x $200 = $14k; take 12.5%% of $3k value = $375; "
           "MA upgrade recommended)\n");
    return 0;
}
