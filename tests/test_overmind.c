#include "overmind.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_routine_baseline(void) {
    overmind_task_t task;
    overmind_plan_t plan;
    overmind_task_init(&task);
    assert(overmind_plan(&task, &plan));
    assert(plan.disposition == OVERMIND_DISPOSITION_PROCEED);
    assert(plan.reasoning == OVERMIND_REASON_DEDUCTIVE);
    assert(plan.checks & OVERMIND_CHECK_ASSUMPTIONS);
    assert(!plan.may_expand_authority);
}

static void test_uncertain_prediction(void) {
    overmind_task_t task;
    overmind_plan_t plan;
    overmind_task_init(&task);
    task.uncertain = true;
    task.predictive = true;
    assert(overmind_plan(&task, &plan));
    assert(plan.reasoning & OVERMIND_REASON_ABDUCTIVE);
    assert(plan.reasoning & OVERMIND_REASON_INDUCTIVE);
    assert(plan.reasoning & OVERMIND_REASON_PROBABILISTIC);
    assert(plan.checks & OVERMIND_CHECK_CONSIDER_OPPOSITE);
    assert(plan.checks & OVERMIND_CHECK_REFERENCE_CLASS);
    assert(plan.checks & OVERMIND_CHECK_CALIBRATION);
    assert(plan.independent_views == 2);
}

static void test_planning_and_causality(void) {
    overmind_task_t task;
    overmind_plan_t plan;
    overmind_task_init(&task);
    task.planning = true;
    task.causal_claim = true;
    assert(overmind_plan(&task, &plan));
    assert(plan.reasoning & OVERMIND_REASON_CAUSAL);
    assert(plan.reasoning & OVERMIND_REASON_COUNTERFACTUAL);
    assert(plan.checks & OVERMIND_CHECK_PREMORTEM);
    assert(plan.checks & OVERMIND_CHECK_CAUSALITY);
}

static void test_self_change_without_evidence_abstains(void) {
    overmind_task_t task;
    overmind_plan_t plan;
    overmind_task_init(&task);
    task.self_modification = true;
    task.evidence_available = false;
    assert(overmind_plan(&task, &plan));
    assert(plan.disposition == OVERMIND_DISPOSITION_ABSTAIN);
    assert(plan.verifier_must_be_independent);
    assert(plan.checks & OVERMIND_CHECK_INDEPENDENT_VERIFY);
    assert(!plan.may_expand_authority);
}

static void test_bounded_compute(void) {
    overmind_task_t task;
    overmind_plan_t plan;
    overmind_task_init(&task);
    task.uncertain = true;
    task.consequential = true;
    task.max_reasoning_passes = 1;
    assert(overmind_plan(&task, &plan));
    assert(plan.reasoning_passes == 1);
}

static void test_invalid_authority_expansion(void) {
    overmind_plan_t plan = {
        .reasoning = OVERMIND_REASON_DEDUCTIVE,
        .checks = OVERMIND_CHECK_ASSUMPTIONS,
        .disposition = OVERMIND_DISPOSITION_PROCEED,
        .reasoning_passes = 1,
        .independent_views = 1,
        .may_expand_authority = true,
    };
    assert(!overmind_plan_valid(&plan));
}

static void test_structured_summary(void) {
    overmind_task_t task;
    overmind_plan_t plan;
    char buf[512];
    overmind_task_init(&task);
    task.adversarial = true;
    assert(overmind_plan(&task, &plan));
    assert(overmind_plan_summary(&plan, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "authority_expansion=false"));
    assert(strstr(buf, "independent_verifier=true"));
}

int main(void) {
    test_routine_baseline();
    test_uncertain_prediction();
    test_planning_and_causality();
    test_self_change_without_evidence_abstains();
    test_bounded_compute();
    test_invalid_authority_expansion();
    test_structured_summary();
    puts("overmind tests: ok");
    return 0;
}
