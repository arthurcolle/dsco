#include "overmind.h"

#include <stdio.h>
#include <string.h>

void overmind_task_init(overmind_task_t *task) {
    if (!task) return;
    memset(task, 0, sizeof(*task));
    task->evidence_available = true;
    task->candidate_count = 1;
    task->max_reasoning_passes = 3;
}

bool overmind_plan(const overmind_task_t *task, overmind_plan_t *plan) {
    if (!task || !plan) return false;
    memset(plan, 0, sizeof(*plan));

    plan->reasoning = OVERMIND_REASON_DEDUCTIVE;
    plan->checks = OVERMIND_CHECK_ASSUMPTIONS;
    plan->disposition = OVERMIND_DISPOSITION_PROCEED;
    plan->reasoning_passes = 1;
    plan->independent_views = 1;
    plan->may_expand_authority = false;

    if (task->uncertain) {
        plan->reasoning |= OVERMIND_REASON_ABDUCTIVE | OVERMIND_REASON_PROBABILISTIC;
        plan->checks |= OVERMIND_CHECK_CONSIDER_OPPOSITE |
                        OVERMIND_CHECK_COUNTERARGUMENT |
                        OVERMIND_CHECK_CALIBRATION;
        plan->reasoning_passes = 2;
        plan->independent_views = 2;
    }
    if (task->predictive) {
        plan->reasoning |= OVERMIND_REASON_INDUCTIVE | OVERMIND_REASON_PROBABILISTIC;
        plan->checks |= OVERMIND_CHECK_REFERENCE_CLASS | OVERMIND_CHECK_CALIBRATION;
    }
    if (task->causal_claim) {
        plan->reasoning |= OVERMIND_REASON_CAUSAL | OVERMIND_REASON_COUNTERFACTUAL;
        plan->checks |= OVERMIND_CHECK_CAUSALITY | OVERMIND_CHECK_CONSIDER_OPPOSITE;
    }
    if (task->planning) {
        plan->reasoning |= OVERMIND_REASON_COUNTERFACTUAL;
        plan->checks |= OVERMIND_CHECK_PREMORTEM | OVERMIND_CHECK_REFERENCE_CLASS;
    }
    if (task->comparative || task->candidate_count > 1) {
        plan->reasoning |= OVERMIND_REASON_ANALOGICAL;
        plan->checks |= OVERMIND_CHECK_COUNTERARGUMENT;
        if (plan->independent_views < 2) plan->independent_views = 2;
    }
    if (task->adversarial) {
        plan->checks |= OVERMIND_CHECK_CONSIDER_OPPOSITE |
                        OVERMIND_CHECK_COUNTERARGUMENT |
                        OVERMIND_CHECK_INDEPENDENT_VERIFY;
        plan->verifier_must_be_independent = true;
    }
    if (task->consequential || task->irreversible || task->self_modification) {
        plan->checks |= OVERMIND_CHECK_PREMORTEM |
                        OVERMIND_CHECK_CALIBRATION |
                        OVERMIND_CHECK_INDEPENDENT_VERIFY;
        plan->verifier_must_be_independent = true;
        plan->disposition = OVERMIND_DISPOSITION_REQUIRE_VERIFIER;
        if (plan->reasoning_passes < 2) plan->reasoning_passes = 2;
    }
    if (!task->evidence_available) {
        plan->disposition = (task->consequential || task->irreversible || task->self_modification)
                                ? OVERMIND_DISPOSITION_ABSTAIN
                                : OVERMIND_DISPOSITION_GATHER_EVIDENCE;
    }

    unsigned cap = task->max_reasoning_passes ? task->max_reasoning_passes : 1;
    if (plan->reasoning_passes > cap) plan->reasoning_passes = cap;
    if (plan->reasoning_passes == 0) plan->reasoning_passes = 1;
    return overmind_plan_valid(plan);
}

bool overmind_plan_valid(const overmind_plan_t *plan) {
    if (!plan || plan->may_expand_authority || plan->reasoning == 0 ||
        plan->reasoning_passes == 0 || plan->independent_views == 0)
        return false;
    if (plan->verifier_must_be_independent &&
        !(plan->checks & OVERMIND_CHECK_INDEPENDENT_VERIFY))
        return false;
    if (plan->disposition < OVERMIND_DISPOSITION_PROCEED ||
        plan->disposition > OVERMIND_DISPOSITION_ABSTAIN)
        return false;
    return true;
}

const char *overmind_disposition_name(overmind_disposition_t disposition) {
    switch (disposition) {
        case OVERMIND_DISPOSITION_PROCEED: return "proceed";
        case OVERMIND_DISPOSITION_GATHER_EVIDENCE: return "gather_evidence";
        case OVERMIND_DISPOSITION_REQUIRE_VERIFIER: return "require_verifier";
        case OVERMIND_DISPOSITION_ABSTAIN: return "abstain";
    }
    return "unknown";
}

int overmind_plan_summary(const overmind_plan_t *plan, char *buf, size_t len) {
    if (!overmind_plan_valid(plan) || !buf || len == 0) return -1;
    int n = snprintf(buf, len,
                     "disposition=%s reasoning=0x%02x checks=0x%02x passes=%u views=%u independent_verifier=%s authority_expansion=false",
                     overmind_disposition_name(plan->disposition), plan->reasoning,
                     plan->checks, plan->reasoning_passes, plan->independent_views,
                     plan->verifier_must_be_independent ? "true" : "false");
    if (n < 0 || (size_t)n >= len) return -1;
    return n;
}
