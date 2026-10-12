/* Unit test for the empirical capability layer.
   Verifies Wilson lower-bound behavior, sample gating, and tier derivation.
   Build: cc -Iinclude -o /tmp/tct tests/test_capability_tiering.c src/governance.c \
          src/pheromone.o src/ooda.o ... (see run below; we link only what we need
          via the pure functions, so a minimal TU is used). */
#include "governance.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int fails = 0;
#define CHECK(cond, msg)                                                                            \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            printf("FAIL: %s\n", msg);                                                             \
            fails++;                                                                               \
        } else {                                                                                    \
            printf("ok:   %s\n", msg);                                                             \
        }                                                                                          \
    } while (0)

int main(void) {
    /* No evidence → bound is 0. */
    CHECK(governance_wilson_lower_bound(0, 0) == 0.0, "wilson(0,0)==0");

    /* A perfect but tiny sample must NOT reach the raw 1.0 — that's the point. */
    double lb2 = governance_wilson_lower_bound(2, 0);
    CHECK(lb2 > 0.0 && lb2 < 0.6, "wilson(2,0) pulled toward center (<0.6)");

    /* More perfect evidence tightens the bound upward. */
    double lb20 = governance_wilson_lower_bound(20, 0);
    CHECK(lb20 > lb2, "wilson(20,0) > wilson(2,0)");
    CHECK(lb20 > 0.80, "wilson(20,0) clears expert threshold");

    /* Monotonic: failures lower the bound. */
    double a = governance_wilson_lower_bound(15, 0);
    double b = governance_wilson_lower_bound(15, 5);
    CHECK(a > b, "failures reduce wilson bound");

    /* Sample gating: 2/2 perfect cannot be EXPERT (below sample floor). */
    double lb;
    capability_tier_t t = governance_derive_capability(2, 0, &lb);
    CHECK(t != CAPABILITY_EXPERT && t != CAPABILITY_PROFICIENT,
          "2/2 gated below proficient (sample floor)");

    /* 20/0 → EXPERT once evidence is sufficient. */
    t = governance_derive_capability(20, 0, &lb);
    CHECK(t == CAPABILITY_EXPERT, "20/0 derives EXPERT");

    /* Mostly-failing agent → NOVICE. */
    t = governance_derive_capability(1, 20, &lb);
    CHECK(t == CAPABILITY_NOVICE, "1/20 derives NOVICE");

    /* Middling record → COMPETENT/PROFICIENT band, never EXPERT. */
    t = governance_derive_capability(12, 8, &lb);
    CHECK(t != CAPABILITY_EXPERT, "12/8 not EXPERT");

    /* End-to-end through the engine: register, feed outcomes, watch tier rise. */
    static governance_engine_t g;
    governance_init(&g);
    bool reg = governance_register_agent(&g, "worker", PRINCIPAL_TIER_2, 0);
    CHECK(reg, "agent registered");

    const agent_envelope_t *e0 = governance_get_agent(&g, "worker");
    CHECK(e0 && e0->claimed_capability == CAPABILITY_COMPETENT,
          "fresh agent starts COMPETENT (provisional)");

    /* A fresh agent must not be able to claim EXPERT. */
    CHECK(!governance_can_claim_capability(&g, "worker", CAPABILITY_EXPERT),
          "fresh agent cannot claim EXPERT");
    CHECK(governance_can_claim_capability(&g, "worker", CAPABILITY_COMPETENT),
          "fresh agent may claim COMPETENT");

    /* Feed many successes; tier climbs (one rung at a time, but cooldown default
       may gate — we disable it for the test). */
    governance_set_param(&g, "cap.upgrade_cooldown", 0, PRINCIPAL_TIER_0);
    for (int i = 0; i < 30; i++)
        governance_record_outcome(&g, "worker", true);
    const agent_envelope_t *e1 = governance_get_agent(&g, "worker");
    CHECK(e1 && e1->claimed_capability <= CAPABILITY_PROFICIENT,
          "sustained success promotes toward EXPERT/PROFICIENT");
    CHECK(e1 && e1->evidence_successes == 30, "successes recorded");

    /* Now a burst of failures must downgrade INSTANTLY (safety asymmetry). */
    capability_tier_t before = e1->claimed_capability;
    for (int i = 0; i < 40; i++)
        governance_record_outcome(&g, "worker", false);
    const agent_envelope_t *e2 = governance_get_agent(&g, "worker");
    CHECK(e2 && e2->claimed_capability > before, "failure burst downgrades tier");

    if (fails == 0)
        printf("\nALL CAPABILITY TESTS PASSED\n");
    else
        printf("\n%d TEST(S) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
