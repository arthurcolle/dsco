/* test_agent_consensus.c — multi-agent consensus scenarios.
 * Diverse models, correlated cheap replicas (correlation penalty), learned
 * reliability, and the tiebreak/escalation cascade. */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "agent_consensus.h"

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", m, __LINE__); g_fail++; } } while(0)

/* Jev tiebreak stand-in: prefers the longer label, confident. */
static int tb_confident(const char *a, const char *b, const char *ctx, double *conf, void *c) {
    (void)ctx; (void)c; *conf = 0.9;
    return strlen(a) >= strlen(b) ? 0 : 1;
}
/* Uncertain arbiter (forces escalation). */
static int tb_uncertain(const char *a, const char *b, const char *ctx, double *conf, void *c) {
    (void)a;(void)b;(void)ctx;(void)c; *conf = 0.3; return 0;
}

static char seen_a[AGENT_CONS_LABEL_MAX], seen_b[AGENT_CONS_LABEL_MAX];
static int tb_capture(const char *a, const char *b, const char *ctx, double *conf, void *opaque) {
    (void)ctx; (void)opaque;
    snprintf(seen_a, sizeof(seen_a), "%s", a);
    snprintf(seen_b, sizeof(seen_b), "%s", b);
    *conf = 0.9;
    return 1;
}
static int tb_invalid(const char *a, const char *b, const char *ctx, double *conf, void *opaque) {
    (void)a; (void)b; (void)ctx; (void)opaque;
    *conf = NAN;
    return 99;
}

int main(void) {
    printf("=== agent consensus scenarios ===\n");

    /* ── 1. Clear majority with diverse models ── */
    agent_vote_t v1[] = {
        {.label="Paris", .confidence=0.9, .model_class=1, .domain=0},
        {.label="Paris", .confidence=0.85, .model_class=2, .domain=0},
        {.label="Paris", .confidence=0.8, .model_class=3, .domain=0},
        {.label="Lyon",  .confidence=0.4, .model_class=4, .domain=0},
    };
    agent_consensus_result_t r;
    CHECK(agent_consensus_vote(v1, 4, NULL, 0, &r), "vote runs");
    CHECK(strcmp(r.winning_label, "Paris") == 0, "diverse majority reaches consensus");
    CHECK(r.clear, "clear majority is clear");
    printf("  [1] diverse majority -> '%s' (agreement %.2f, clear)\n", r.winning_label, r.agreement);

    /* ── 2. Correlation penalty: 5 identical cheap replicas vs 1 diverse expert ── */
    /* 5 replicas of the SAME model class all say "cheap"; 1 diverse high-reliability
     * expert says "strong". Without the penalty, 5 replicas dominate. */
    agent_reliability_t rel[8]; size_t nrel = 0;
    /* expert (class 9) is very reliable; cheap replicas (class 5) are not */
    for (int i = 0; i < 8; i++) agent_consensus_learn(rel, &nrel, 8, 9, 0, true);   /* expert 8/8 */
    for (int i = 0; i < 4; i++) agent_consensus_learn(rel, &nrel, 8, 5, 0, i < 2);  /* cheap 2/4 */
    agent_vote_t v2[] = {
        {.label="cheap", .confidence=0.9, .model_class=5, .domain=0},
        {.label="cheap", .confidence=0.9, .model_class=5, .domain=0},
        {.label="cheap", .confidence=0.9, .model_class=5, .domain=0},
        {.label="cheap", .confidence=0.9, .model_class=5, .domain=0},
        {.label="cheap", .confidence=0.9, .model_class=5, .domain=0},
        {.label="strong",.confidence=0.85,.model_class=9, .domain=0},
    };
    CHECK(agent_consensus_vote(v2, 6, rel, nrel, &r), "correlated vote runs");
    printf("  [2] 5 correlated replicas vs 1 expert -> winner='%s' agreement=%.2f (correlation-penalized)\n",
           r.winning_label, r.agreement);
    /* The 5 identical replicas are capped at 50% share, so the expert's voice matters.
     * We assert the cheap class didn't exceed the cap in effect (agreement <= ~0.5+) */
    CHECK(r.agreement <= 0.500001, "correlated class has at most half of final vote weight");
    CHECK(!r.clear, "two opposed classes cannot certify clear agreement");
    agent_vote_t lone[] = {
        {.label="same", .confidence=1, .model_class=5},
        {.label="same", .confidence=1, .model_class=5},
    };
    CHECK(agent_consensus_vote(lone, 2, NULL, 0, &r), "single-class tally exists");
    CHECK(!r.clear, "one correlated class cannot certify independent consensus");
    agent_vote_t three[] = {
        {.label="a", .confidence=1, .model_class=1},
        {.label="a", .confidence=1, .model_class=1},
        {.label="a", .confidence=1, .model_class=1},
        {.label="b", .confidence=1, .model_class=2},
        {.label="c", .confidence=1, .model_class=3},
    };
    CHECK(agent_consensus_vote(three, 5, NULL, 0, &r), "three-class tally exists");
    CHECK(r.agreement <= 0.500001, "three-class dominant class has at most half final weight");

    /* ── 3. Learned reliability shifts the outcome ── */
    double rel_expert = agent_consensus_reliability(rel, nrel, 9, 0);
    double rel_cheap = agent_consensus_reliability(rel, nrel, 5, 0);
    CHECK(rel_expert > rel_cheap, "expert reliability > cheap replica reliability");
    printf("  [3] learned reliability: expert=%.2f cheap=%.2f\n", rel_expert, rel_cheap);

    /* ── 4. Split vote -> tiebreak resolves ── */
    agent_vote_t v3[] = {
        {.label="option-A-longer", .confidence=0.55, .model_class=1, .domain=1},
        {.label="optB",             .confidence=0.55, .model_class=2, .domain=1},
    };
    bool used_tb; double tb_conf;
    int rc = agent_consensus_decide(v3, 2, NULL, 0, "test q", tb_confident, NULL, &r, &used_tb, &tb_conf);
    CHECK(rc == 1, "split vote resolved by tiebreak");
    CHECK(used_tb, "tiebreak was used");
    printf("  [4] split vote -> tiebreak resolved to '%s' (tb_conf %.2f)\n", r.winning_label, tb_conf);

    /* ── 5. Split vote + uncertain arbiter -> escalate to frontier judge ── */
    rc = agent_consensus_decide(v3, 2, NULL, 0, "test q", tb_uncertain, NULL, &r, &used_tb, &tb_conf);
    CHECK(rc == 0, "uncertain arbiter escalates");
    printf("  [5] split vote + uncertain arbiter -> escalate (rc=0, tb_conf %.2f)\n", tb_conf);

    /* Dominant class's RAW runner (a) is not the adjusted runner (c). */
    agent_vote_t biased[] = {
        {.label="b", .confidence=.9, .model_class=1},
        {.label="b", .confidence=.9, .model_class=1},
        {.label="b", .confidence=.9, .model_class=1},
        {.label="a", .confidence=.9, .model_class=1},
        {.label="c", .confidence=.8, .model_class=2},
        {.label="d", .confidence=.4, .model_class=3},
    };
    rc = agent_consensus_decide(biased, 6, NULL, 0, "runner", tb_capture, NULL,
                                &r, &used_tb, &tb_conf);
    CHECK(rc == 1 && used_tb, "adjusted tie calls arbiter");
    CHECK(strcmp(seen_a, "b") == 0 && strcmp(seen_b, "c") == 0,
          "arbiter receives adjusted winner and runner, not raw runner");
    CHECK(strcmp(r.winning_label, "c") == 0 && r.weight > 0 && r.margin < 0,
          "arbiter override updates weight, agreement and signed margin");
    rc = agent_consensus_decide(biased, 6, NULL, 0, "invalid", tb_invalid, NULL,
                                &r, &used_tb, &tb_conf);
    CHECK(rc == 0 && !r.clear, "invalid arbiter output cannot certify consensus");
    agent_vote_t bad[] = {{.label="ok", .confidence=NAN, .model_class=1}};
    CHECK(!agent_consensus_vote(bad, 1, NULL, 0, &r), "NaN confidence rejected");
    memset(bad[0].label, 'x', sizeof(bad[0].label)); bad[0].confidence = 1;
    CHECK(!agent_consensus_vote(bad, 1, NULL, 0, &r), "unterminated label rejected");
    agent_vote_t many[AGENT_CONS_CLASS_MAX + 1] = {0};
    for (size_t i = 0; i < AGENT_CONS_CLASS_MAX + 1; i++) {
        strcpy(many[i].label, "same"); many[i].confidence = 1;
        many[i].model_class = (uint32_t)i;
    }
    CHECK(!agent_consensus_vote(many, AGENT_CONS_CLASS_MAX + 1, NULL, 0, &r),
          "class overflow rejected instead of silently discarding weights");
    /* Sweep skewed, distinct-class votes: no class may exceed the final cap. */
    for (int scenario = 0; scenario < 128; scenario++) {
        agent_vote_t sample[4] = {
            {.label="heavy", .confidence=1, .model_class=1},
            {.label="heavy", .confidence=1, .model_class=1},
            {.label="medium", .confidence=(scenario + 1) / 129.0, .model_class=2},
            {.label="light", .confidence=(scenario + 1) / 1024.0, .model_class=3},
        };
        CHECK(agent_consensus_vote(sample, 4, NULL, 0, &r), "skewed tally valid");
        CHECK(r.agreement <= 0.500001, "dominant class share capped under skew");
    }
    agent_reliability_t corrupt = {.model_class=1, .domain=0, .total=1, .correct=2};
    CHECK(agent_consensus_reliability(&corrupt, 1, 1, 0) == 0.5,
          "invalid reliability counters do not create reliability above one");
    agent_reliability_t large = {.model_class=1, .domain=0,
                                 .total=UINT32_MAX, .correct=UINT32_MAX};
    size_t count = 1;
    agent_consensus_learn(&large, &count, 1, 1, 0, true);
    CHECK(large.total == UINT32_MAX && count == 1, "learning counters do not wrap");

    if (g_fail) { fprintf(stderr, "\nagent_consensus: %d FAILURE(S)\n", g_fail); return 1; }
    printf("\nagent_consensus: all scenarios passed (diverse majority, correlation penalty, learned reliability, tiebreak, escalation)\n");
    return 0;
}
