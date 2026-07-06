/* Governance-model A/B bypass test.
   Verifies that DSCO_GOV_MODEL=none routes tool execution around the entire
   governance gate (the ungoverned control arm) and that the experiment
   counters record the bypass. This is the measurement backbone for comparing
   governance overhead across models. */
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

int main(void) {
    /* Enable the ungoverned control arm before any tool call so the cached
       decision inside tools_execute_for_tier picks it up. */
    setenv("DSCO_GOV_MODEL", "none", 1);

    unsigned long calls0 = 0, bypassed0 = 0;
    double ms0 = 0;
    tools_governance_experiment_stats(&calls0, &bypassed0, &ms0);

    /* A tool that would normally traverse the gate. echo/date via bash is
       governance-relevant (shell group); under bypass it must skip the gate. */
    char result[4096];
    bool ok = tools_execute_for_tier("bash",
                                     "{\"command\":\"echo governance_bypass_probe\"}",
                                     "trusted", result, sizeof(result));

    unsigned long calls1 = 0, bypassed1 = 0;
    double ms1 = 0;
    tools_governance_experiment_stats(&calls1, &bypassed1, &ms1);

    CHECK(ok, "bash executed under bypass");
    CHECK(strstr(result, "governance_bypass_probe") != NULL, "bash output present");
    CHECK(bypassed1 > bypassed0, "bypass counter incremented");
    CHECK(calls1 == calls0, "gate-run counter did NOT increment (gate skipped)");
    CHECK(ms1 == ms0, "no gate latency accrued in bypass arm");

    if (fails == 0)
        printf("\nALL GOVERNANCE-BYPASS TESTS PASSED\n");
    else
        printf("\n%d TEST(S) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
