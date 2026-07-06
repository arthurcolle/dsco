/* Governance-model experiment platform test.
   Covers: model parsing/aliases, the policy matrix (which stages run/enforce
   per model), counter accumulation, JSON report shape, and reset. Also does an
   end-to-end bypass check through tools_execute_for_tier (model=none). */
#include "gov_experiment.h"
#include "tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* gov_experiment_model() caches on first call, so pure-function tests use the
   matrix helpers directly rather than the env-resolved active model. */
static void test_policy_matrix(void) {
    /* NONE: nothing runs, nothing enforces. */
    CHECK(!gov_stage_active(GOV_MODEL_NONE, GOV_STAGE_CHECKPOINT), "none: checkpoint inactive");
    CHECK(!gov_stage_active(GOV_MODEL_NONE, GOV_STAGE_KILLSWITCH), "none: killswitch inactive");

    /* MINIMAL: immune stages run+enforce; checkpoint does not. */
    CHECK(gov_stage_active(GOV_MODEL_MINIMAL, GOV_STAGE_KILLSWITCH), "minimal: killswitch active");
    CHECK(gov_stage_enforces(GOV_MODEL_MINIMAL, GOV_STAGE_KILLSWITCH), "minimal: killswitch enforces");
    CHECK(!gov_stage_active(GOV_MODEL_MINIMAL, GOV_STAGE_CHECKPOINT), "minimal: checkpoint inactive");

    /* AUDIT_ONLY: everything runs; only immune stages enforce. */
    CHECK(gov_stage_active(GOV_MODEL_AUDIT_ONLY, GOV_STAGE_CHECKPOINT), "audit: checkpoint runs");
    CHECK(!gov_stage_enforces(GOV_MODEL_AUDIT_ONLY, GOV_STAGE_CHECKPOINT),
          "audit: checkpoint measures but does NOT block");
    CHECK(gov_stage_enforces(GOV_MODEL_AUDIT_ONLY, GOV_STAGE_KILLSWITCH),
          "audit: killswitch still enforces (safety floor)");

    /* STANDARD: checkpoint runs + enforces. */
    CHECK(gov_stage_enforces(GOV_MODEL_STANDARD, GOV_STAGE_CHECKPOINT), "standard: checkpoint enforces");

    /* PARANOID: shadow applies to all tools. */
    CHECK(gov_shadow_all_tools(GOV_MODEL_PARANOID), "paranoid: shadow-all-tools");
    CHECK(!gov_shadow_all_tools(GOV_MODEL_STANDARD), "standard: shadow high-risk only");

    /* Names round-trip. */
    CHECK(strcmp(gov_model_name(GOV_MODEL_AUDIT_ONLY), "audit_only") == 0, "model name audit_only");
    CHECK(strcmp(gov_stage_name(GOV_STAGE_CHECKPOINT), "checkpoint") == 0, "stage name checkpoint");
}

static void test_counters_and_report(void) {
    gov_experiment_reset();
    gov_stage_record(GOV_STAGE_CHECKPOINT, 0.5, true, true);  /* a real block */
    gov_stage_record(GOV_STAGE_CHECKPOINT, 0.3, false, false);
    gov_stage_record(GOV_STAGE_SHADOW, 1.2, true, false);     /* would-block, audit */
    gov_experiment_note_gate_run();
    gov_experiment_note_gate_run();

    unsigned long calls = 0, byp = 0;
    double ms = 0;
    gov_experiment_totals(&calls, &byp, &ms);
    CHECK(calls == 2, "gate_runs counted");
    CHECK(ms > 1.9 && ms < 2.1, "gate_ms_total aggregates all stages (~2.0)");

    char buf[2048];
    size_t n = gov_experiment_report_json(buf, sizeof(buf));
    CHECK(n > 0 && buf[0] == '{', "report is JSON");
    CHECK(strstr(buf, "\"stage\":\"checkpoint\"") != NULL, "report has checkpoint stage");
    CHECK(strstr(buf, "\"denials\":2") == NULL, "checkpoint denials not overcounted");
    CHECK(strstr(buf, "\"blocks\":1") != NULL, "one real block recorded");

    gov_experiment_reset();
    gov_experiment_totals(&calls, &byp, &ms);
    CHECK(calls == 0 && ms == 0.0, "reset clears counters");
}

static void test_end_to_end_bypass(void) {
    /* model=none must skip the gate entirely and still execute. */
    setenv("DSCO_GOV_MODEL", "none", 1); /* resolved+cached on first model() call */
    CHECK(gov_experiment_bypass_all(), "model=none → bypass_all");

    gov_experiment_reset();
    char result[4096];
    bool ok = tools_execute_for_tier("bash",
                                     "{\"command\":\"echo governance_bypass_probe\"}",
                                     "trusted", result, sizeof(result));
    unsigned long calls = 0, byp = 0;
    double ms = 0;
    gov_experiment_totals(&calls, &byp, &ms);

    CHECK(ok, "bash executed under bypass");
    CHECK(strstr(result, "governance_bypass_probe") != NULL, "bash output present");
    CHECK(byp >= 1, "bypass counter incremented");
    CHECK(calls == 0, "gate-run counter stayed flat (gate skipped)");
    CHECK(ms == 0.0, "no gate latency accrued in bypass arm");
}

int main(void) {
    /* The active model is resolved+cached on first gov_experiment_model() call.
       In production --systems-agent sets this before any tool runs; here we set
       it up front so the cached model matches the end-to-end bypass test. The
       policy-matrix tests use the pure helpers and are model-arg explicit. */
    setenv("DSCO_GOV_MODEL", "none", 1);

    test_policy_matrix();
    test_counters_and_report();
    test_end_to_end_bypass();

    if (fails == 0)
        printf("\nALL GOVERNANCE-EXPERIMENT TESTS PASSED\n");
    else
        printf("\n%d TEST(S) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
