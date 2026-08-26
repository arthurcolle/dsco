#ifndef DSCO_OVERMIND_H
#define DSCO_OVERMIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Overmind is a cognitive-orchestration planner. It neither grants authority
 * nor executes tools; capability governance remains the execution boundary. */

typedef enum {
    OVERMIND_REASON_DEDUCTIVE       = 1u << 0,
    OVERMIND_REASON_INDUCTIVE       = 1u << 1,
    OVERMIND_REASON_ABDUCTIVE       = 1u << 2,
    OVERMIND_REASON_ANALOGICAL      = 1u << 3,
    OVERMIND_REASON_CAUSAL          = 1u << 4,
    OVERMIND_REASON_PROBABILISTIC   = 1u << 5,
    OVERMIND_REASON_COUNTERFACTUAL  = 1u << 6
} overmind_reasoning_t;

typedef enum {
    OVERMIND_CHECK_CONSIDER_OPPOSITE = 1u << 0,
    OVERMIND_CHECK_COUNTERARGUMENT    = 1u << 1,
    OVERMIND_CHECK_REFERENCE_CLASS    = 1u << 2,
    OVERMIND_CHECK_PREMORTEM          = 1u << 3,
    OVERMIND_CHECK_ASSUMPTIONS        = 1u << 4,
    OVERMIND_CHECK_CAUSALITY          = 1u << 5,
    OVERMIND_CHECK_CALIBRATION        = 1u << 6,
    OVERMIND_CHECK_INDEPENDENT_VERIFY = 1u << 7
} overmind_check_t;

typedef enum {
    OVERMIND_DISPOSITION_PROCEED,
    OVERMIND_DISPOSITION_GATHER_EVIDENCE,
    OVERMIND_DISPOSITION_REQUIRE_VERIFIER,
    OVERMIND_DISPOSITION_ABSTAIN
} overmind_disposition_t;

typedef struct {
    bool consequential;
    bool irreversible;
    bool uncertain;
    bool predictive;
    bool causal_claim;
    bool planning;
    bool comparative;
    bool adversarial;
    bool self_modification;
    bool evidence_available;
    unsigned candidate_count;
    unsigned max_reasoning_passes;
} overmind_task_t;

typedef struct {
    uint32_t reasoning;
    uint32_t checks;
    overmind_disposition_t disposition;
    unsigned reasoning_passes;
    unsigned independent_views;
    bool verifier_must_be_independent;
    bool may_expand_authority; /* invariant: always false */
} overmind_plan_t;

void overmind_task_init(overmind_task_t *task);
bool overmind_plan(const overmind_task_t *task, overmind_plan_t *plan);
bool overmind_plan_valid(const overmind_plan_t *plan);
const char *overmind_disposition_name(overmind_disposition_t disposition);
int overmind_plan_summary(const overmind_plan_t *plan, char *buf, size_t len);

#endif
