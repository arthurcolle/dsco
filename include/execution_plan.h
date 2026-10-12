#ifndef DSCO_EXECUTION_PLAN_H
#define DSCO_EXECUTION_PLAN_H
#include "cost_frontier.h"
#include "task_profile.h"
#include "work_unit.h"

#define EP_MAX_JOBS 16
#define EP_MAX_LANES 32
#define EP_MAX_ATTEMPTS 3
#define EP_TEXT 4096
/* Cash is micro-USD; subscription quota is caller-defined integer units.
 * Unknown is never zero. All resource components must be explicitly known. */
typedef struct { int64_t cash_microusd, quota_units; bool known; } ep_resource_t;
typedef enum { EP_THROUGHPUT, EP_CASCADE, EP_DELAYED_HEDGE, EP_SYNTHESIS } ep_policy_t;
typedef enum { EP_READ_ONLY, EP_MUTATION, EP_EXTERNAL_EFFECT } ep_effect_t;
typedef struct {
    char provider[64], model[128], endpoint[256], account[128];
    char auth_product[32], effort[16], upstream[64], quantization[32];
} ep_binding_t;
typedef struct {
    char id[WORK_ID_LEN], inputs[EP_TEXT], tool[128], acceptance[1024];
    char authority[256], workload[128], validator[128], auth_product[32];
    char exact_endpoint[256];
    ep_binding_t required_lane; /* every nonempty field is an exact constraint */
    char dependencies[EP_MAX_JOBS][WORK_ID_LEN];
    size_t dependency_count;
    work_unit_kind_t kind;
    ep_policy_t policy;
    ep_effect_t effect;
    uint64_t capabilities, modalities, privacy_domains;
    int64_t prompt_tokens, output_tokens;
    time_t deadline;
    unsigned attempt_limit, hedge_delay_seconds;
    bool operator_authorized, isolated_proposals;
    /* Bounds for mechanical execution, verification, optional synthesis,
     * coordination, and authorized recovery; no implicit free component. */
    ep_resource_t mechanical, verification, synthesis, coordination, recovery;
    bool component_durations_known; /* explicit hard bounds, never p95 sums */
    uint64_t mechanical_duration_seconds, verification_seconds;
    uint64_t synthesis_seconds, coordination_seconds, recovery_seconds;
} ep_contract_t;
typedef struct {
    ep_binding_t binding;
    bool available, endpoint_resolved, operator_authorized, tools;
    uint64_t capabilities, modalities, privacy_domains;
    cost_frontier_candidate_t quote;
    char workload[128], validator[128], correlation_group[128];
    time_t evidence_observed_at;
    /* Includes queue delay. Unknown duration cannot satisfy a deadline. */
    uint64_t duration_upper_seconds;
    double uncertainty; /* caller's bound, 0..1; not an independence claim */
    ep_resource_t attempt;
} ep_lane_t;
typedef struct {
    time_t at;
    unsigned max_evidence_age_seconds, max_quote_age_seconds, min_samples;
    double minimum_success_lower_bound, max_uncertainty;
    ep_resource_t budget;
    /* Advice only. Keyword profiling cannot authorize work or set policy. */
    const task_profile_t *profile;
} ep_options_t;
/* Every rejection is inspectable, in original catalog order. */
enum {
    EP_REJECT_UNAVAILABLE=1ULL<<0, EP_REJECT_ENDPOINT=1ULL<<1,
    EP_REJECT_IDENTITY=1ULL<<2, EP_REJECT_AUTH=1ULL<<3,
    EP_REJECT_AUTHORITY=1ULL<<4, EP_REJECT_CAPABILITIES=1ULL<<5,
    EP_REJECT_MODALITY=1ULL<<6, EP_REJECT_PRIVACY=1ULL<<7,
    EP_REJECT_CONTEXT=1ULL<<8, EP_REJECT_TOOLS=1ULL<<9,
    EP_REJECT_SCOPE=1ULL<<10, EP_REJECT_STALE=1ULL<<11,
    EP_REJECT_UNKNOWN=1ULL<<12, EP_REJECT_UNCERTAINTY=1ULL<<13,
    EP_REJECT_FRONTIER=1ULL<<14, EP_REJECT_DEADLINE=1ULL<<15,
    EP_REJECT_BUDGET=1ULL<<16
};
typedef struct {
    ep_binding_t binding;
    uint64_t rejections;
    cost_frontier_result_t evidence;
    char frontier_reason[96], correlation_group[128];
    ep_resource_t total;
    int64_t cost_to_acceptance_microusd, quota_to_acceptance_units;
    uint64_t duration_seconds;
    char workload[128], validator[128];
    time_t quote_observed_at, evidence_observed_at;
    unsigned samples, accepted_samples;
    double uncertainty;
} ep_candidate_t;
typedef struct {
    int candidate; /* -1 = mechanical dispatcher; never an inferred route */
    ep_binding_t binding;
    ep_resource_t attempt, verification, synthesis, coordination, recovery, total;
    unsigned account_slots, lane_slots;
    uint64_t duration_seconds;
    bool proposal_only, designated_committer;
    char reason[96];
} ep_attempt_t;
enum {
    EP_ALLOW_RETRY=1<<0, EP_ALLOW_HEDGE=1<<1, EP_ALLOW_REPLAN=1<<2,
    EP_ALLOW_RECONCILE=1<<3, EP_ALLOW_COMMIT=1<<4, EP_ALLOW_FINISH=1<<5
};
typedef struct {
    ep_contract_t contract;
    bool mechanical, ready;
    char blocked_reason[96];
    ep_candidate_t candidates[EP_MAX_LANES];
    size_t candidate_count, attempt_count;
    ep_attempt_t attempts[EP_MAX_ATTEMPTS];
    unsigned allowed_transitions;
    /* Deliberately unknown: even distinct providers can share failures. */
    bool joint_hedge_reliability_known;
    uint64_t duration_upper_seconds;
} ep_job_t;
typedef struct {
    ep_options_t options; /* profile pointer removed in built snapshot */
    double parallelism_hint, convergence_hint;
    bool ready; /* feasible intent only; grants no permission to execute */
    char blocked_reason[96];
    ep_resource_t reserved;
    size_t job_count;
    ep_job_t jobs[EP_MAX_JOBS];
    size_t topological_order[EP_MAX_JOBS];
} execution_plan_t;
/* Bounded adapter copies existing typed inputs/dependencies. The operator must
 * then supply explicit authority, effects, policy, deadline and resources. */
bool execution_plan_contract_from_work_unit(const work_unit_t *, ep_contract_t *);
/* No I/O, credentials, worker launches, gate acquisition or live reservation.
 * False returns an inspectable blocked plan; output must be heap allocated. */
bool execution_plan_build(const ep_contract_t *, size_t, const ep_lane_t *, size_t,
                          const ep_options_t *, execution_plan_t *);
/* Caller frees JSON. Stable field/catalog/job order; no clock or random IDs. */
char *execution_plan_json(const execution_plan_t *);

/* Fabric and topology consumers use this SAME intent contract. The existing
 * provider_admission fairness ticket and subscription_gate lane permits do not
 * atomically reserve account + lane + budget. No live adapter is supplied here.
 * A future coordinator must recheck facts and atomically issue an opaque lease
 * immediately before dispatch. This request itself is never a live lease. */
typedef struct {
    ep_contract_t contract;
    ep_attempt_t placement;
    size_t job_index, attempt_index;
    ep_binding_t binding;
    ep_resource_t resources;
    unsigned account_slots, lane_slots;
    time_t deadline;
    char authority[256], acceptance[1024];
    bool proposal_only, designated_committer;
} ep_admission_request_t;
bool execution_plan_admission_request(const execution_plan_t *, size_t, size_t,
                                      ep_admission_request_t *);

typedef enum { EP_EFFECT_NONE, EP_EFFECT_ISOLATED, EP_EFFECT_COMMITTED,
               EP_EFFECT_UNRESOLVED } ep_observed_effect_t;
typedef struct {
    bool started, transport_complete, route_verified, semantic_accepted;
    bool process_exit_zero, cancelled, billing_known;
    ep_observed_effect_t effects;
    time_t started_at;
} ep_outcome_t;
typedef struct { ep_outcome_t attempts[EP_MAX_ATTEMPTS]; } ep_job_evidence_t;
/* Pure accounting: a settlement REPLACES its attempt hold; other planned
 * components remain reserved. Releasing concurrency never restores quota.
 * No settlement is inferred from cancellation or process completion. */
typedef struct { bool reconciled; ep_resource_t charged; } ep_settlement_t;
typedef struct { ep_resource_t spent, held, available; } ep_accounting_t;
bool execution_plan_accounting(const execution_plan_t *,
    const ep_settlement_t [EP_MAX_JOBS][EP_MAX_ATTEMPTS], ep_accounting_t *);

typedef enum { EP_WAIT, EP_DISPATCH, EP_RETRY, EP_HEDGE, EP_REPLAN,
               EP_RECONCILE, EP_COMMIT, EP_FINISH, EP_BLOCK } ep_transition_t;
typedef struct {
    ep_transition_t transition;
    int attempt;
    ep_resource_t charge_hold;
    const char *reason;
} ep_decision_t;
/* Evidence supplied by trusted validators. Process exit alone never accepts.
 * Reconciliation replaces unresolved evidence with observed facts, not a flag
 * which silently releases a charge or authorizes another mutation. */
bool execution_plan_job_accepted(const execution_plan_t *, size_t,
                                 const ep_job_evidence_t *);
ep_decision_t execution_plan_decide(const execution_plan_t *, size_t,
                                     const ep_job_evidence_t *, time_t);
#endif
