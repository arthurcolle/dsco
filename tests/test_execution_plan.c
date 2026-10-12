/* Offline vertical slice: planner + actual frontier + dispatcher predicate.
 * Tripwires make execution/admission accidental calls fail immediately. */
#include "execution_plan.h"
#include "work_dispatch.h"
#include "provider_admission.h"
#include "subscription_gate.h"
#include "swarm.h"
#include "tools.h"
#include "json_util.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned launches, calls, admissions;
int swarm_spawn_provider(swarm_t *s,int group,const char *task,const char *model,const char *provider) {
    (void)s;(void)group;(void)task;(void)model;(void)provider; launches++; abort();
}
bool tools_execute_for_tier(const char *tool,const char *args,const char *tier,char *out,size_t cap) {
    (void)tool;(void)args;(void)tier;(void)out;(void)cap; calls++; abort();
}
bool provider_admission_join(provider_admission_t *q,const char *base) {
    (void)q;(void)base; admissions++; abort();
}
bool subscription_gate_acquire(subscription_gate_t *q,const char *scope,const volatile int *interrupt,long *waited) {
    (void)q;(void)scope;(void)interrupt;(void)waited; admissions++; abort();
}
/* Real keyword profiler against a tiny offline topology registry. */
static topology_t profile_topology;
const topology_t *topology_registry(int *count) { *count=1; return &profile_topology; }
bool topology_is_runnable(const topology_t *t) { return t!=NULL; }
const topology_t *topology_find(const char *name) { (void)name; return &profile_topology; }
static execution_plan_t *plan;
static ep_resource_t money(int64_t cash,int64_t quota) { return (ep_resource_t){cash,quota,true}; }
static ep_options_t options(void) {
    return (ep_options_t){.at=1000,.max_evidence_age_seconds=100,.max_quote_age_seconds=100,
        .min_samples=20,.minimum_success_lower_bound=.7,.max_uncertainty=.2,.budget={10000,100,true}};
}
static ep_contract_t contract(const char *id) {
    ep_contract_t c={.kind=WORK_UNIT_PROMPT,.policy=EP_THROUGHPUT,.effect=EP_READ_ONLY,
        .capabilities=1,.modalities=1,.privacy_domains=1,.prompt_tokens=100,.output_tokens=10,
        .deadline=1100,.attempt_limit=1,.operator_authorized=true,.component_durations_known=true,
        .mechanical={0,0,true},.verification={10,1,true},.synthesis={20,1,true},
        .coordination={30,1,true},.recovery={40,1,true},.mechanical_duration_seconds=4,
        .verification_seconds=1};
    strcpy(c.id,id); strcpy(c.inputs,"inspect explicit inputs"); strcpy(c.acceptance,"validator-v1");
    strcpy(c.authority,"operator:read-workspace-only"); strcpy(c.workload,"file-inspection");
    strcpy(c.validator,"validator-v1"); strcpy(c.auth_product,"subscription"); return c;
}
static ep_lane_t lane(const char *endpoint,int64_t cash,int64_t quota) {
    ep_lane_t l={.available=true,.endpoint_resolved=true,.operator_authorized=true,.tools=true,
        .capabilities=1,.modalities=1,.privacy_domains=1,.evidence_observed_at=990,
        .duration_upper_seconds=4,.uncertainty=.1,.attempt={cash,quota,true},
        .quote={.provider="fixture-provider",.model="fixture-model",.source="fixture-route-quote",
            .observed_at=990,.route_specific=true,.tools=true,.context_tokens=1000,
            .max_output_tokens=100,.attempts=100,.verified=90,.mean_latency_seconds=2,
            .request_fee=0,.input_per_million=0,.output_per_million=0,.cached_per_million=0}};
    strcpy(l.binding.provider,"fixture-provider"); strcpy(l.binding.model,"fixture-model");
    strcpy(l.binding.endpoint,endpoint); strcpy(l.binding.account,"authorized-account");
    strcpy(l.binding.auth_product,"subscription"); strcpy(l.binding.effort,"none");
    strcpy(l.workload,"file-inspection"); strcpy(l.validator,"validator-v1");
    strcpy(l.correlation_group,"shared-backend"); return l;
}
static ep_outcome_t accepted(void) {
    return (ep_outcome_t){.started=true,.transport_complete=true,.route_verified=true,
        .semantic_accepted=true,.billing_known=true,.process_exit_zero=true,.started_at=1000,.effects=EP_EFFECT_NONE};
}
static void zero_components(ep_contract_t *c) {
    c->verification=c->synthesis=c->coordination=c->recovery=money(0,0);
}
static void exact_and_eligibility(void) {
    ep_options_t o=options(); ep_contract_t c=contract("one"); ep_lane_t l[2]={lane("bound",100,2),lane("bound",50,2)};
    strcpy(l[0].binding.upstream,"unresolved-host");
    l[0].endpoint_resolved=false; strcpy(c.exact_endpoint,"bound");
    assert(execution_plan_build(&c,1,l,2,&o,plan));
    assert(plan->jobs[0].candidates[0].rejections & EP_REJECT_ENDPOINT);
    assert(plan->jobs[0].attempts[0].candidate==1);
    char *json=execution_plan_json(plan); assert(strstr(json,"endpoint_unresolved_or_mismatch")); free(json);
    l[1].endpoint_resolved=false; assert(!execution_plan_build(&c,1,l,2,&o,plan));
    l[0]=lane("other",100,2); assert(!execution_plan_build(&c,1,l,1,&o,plan));
    c.exact_endpoint[0]=0;
    l[0]=lane("bound",100,2); l[0].capabilities=0; l[0].modalities=0; l[0].privacy_domains=0;
    l[0].tools=false; l[0].available=false; l[0].operator_authorized=false; strcpy(l[0].binding.auth_product,"api");
    assert(!execution_plan_build(&c,1,l,1,&o,plan));
    uint64_t r=plan->jobs[0].candidates[0].rejections;
    assert((r & (EP_REJECT_CAPABILITIES|EP_REJECT_MODALITY|EP_REJECT_PRIVACY|EP_REJECT_AUTH|EP_REJECT_TOOLS|EP_REJECT_UNAVAILABLE|EP_REJECT_AUTHORITY)) ==
        (EP_REJECT_CAPABILITIES|EP_REJECT_MODALITY|EP_REJECT_PRIVACY|EP_REJECT_AUTH|EP_REJECT_TOOLS|EP_REJECT_UNAVAILABLE|EP_REJECT_AUTHORITY));
    l[0]=lane("bound",100,2); strcpy(c.required_lane.account,"only-this-account");
    assert(!execution_plan_build(&c,1,l,1,&o,plan)); assert(plan->jobs[0].candidates[0].rejections & EP_REJECT_IDENTITY);
    c.required_lane.account[0]=0; l[0].quote.context_tokens=100;
    assert(!execution_plan_build(&c,1,l,1,&o,plan)); assert(plan->jobs[0].candidates[0].rejections & EP_REJECT_CONTEXT);
    puts("PASS exact unresolved endpoint, catalog visibility, capabilities/modality/privacy/auth/authority/context");
}
static void estimates(void) {
    ep_options_t o=options(); ep_contract_t c=contract("one"); ep_lane_t l=lane("bound",100,2);
    l.evidence_observed_at=899; assert(!execution_plan_build(&c,1,&l,1,&o,plan));
    assert(plan->jobs[0].candidates[0].rejections & EP_REJECT_STALE);
    char *json=execution_plan_json(plan); assert(strstr(json,"\"success_lower_bound\":null")); free(json);
    l=lane("bound",100,2); strcpy(l.workload,"unrelated");
    assert(!execution_plan_build(&c,1,&l,1,&o,plan)); assert(plan->jobs[0].candidates[0].rejections & EP_REJECT_SCOPE);
    l=lane("bound",100,2); l.quote.attempts=2; l.quote.verified=2;
    assert(!execution_plan_build(&c,1,&l,1,&o,plan));
    assert(!strcmp(plan->jobs[0].candidates[0].frontier_reason,"verification_evidence_unavailable"));
    l=lane("bound",100,2); l.quote.observed_at=899;
    assert(!execution_plan_build(&c,1,&l,1,&o,plan)); assert(!strcmp(plan->jobs[0].candidates[0].frontier_reason,"quote_time_unusable"));
    l=lane("bound",100,2); l.uncertainty=NAN;
    assert(!execution_plan_build(&c,1,&l,1,&o,plan)); assert(plan->jobs[0].candidates[0].rejections & EP_REJECT_UNCERTAINTY);
    l=lane("bound",100,2); l.attempt.known=false; o.budget=money(200,20);
    assert(!execution_plan_build(&c,1,&l,1,&o,plan)); assert(plan->jobs[0].candidates[0].rejections & EP_REJECT_UNKNOWN);
    l=lane("bound",100,2); l.duration_upper_seconds=0;
    assert(!execution_plan_build(&c,1,&l,1,&o,plan));
    l=lane("bound",100,2); c.component_durations_known=false;
    assert(!execution_plan_build(&c,1,&l,1,&o,plan)); assert(!strcmp(plan->blocked_reason,"unknown_component_duration"));
    puts("PASS stale, unrelated, sparse, unknown costs/duration and uncertainty remain unqualified");
}
static void mechanical_and_adapter(void) {
    ep_contract_t c=contract("one"); ep_options_t o=options();
    work_unit_t unit={.kind=WORK_UNIT_TOOL,.payload="{\"expression\":\"1+1\"}",.tool="calculate",.acceptance_cmd="validate-integer"};
    strcpy(unit.id,"typed-one");
    ep_contract_t adapted; assert(execution_plan_contract_from_work_unit(&unit,&adapted));
    assert(adapted.kind==WORK_UNIT_TOOL && !strcmp(adapted.tool,"calculate"));
    assert(!strcmp(adapted.inputs,unit.payload) && !adapted.operator_authorized);
    unit.provider="pinned-provider"; unit.model="pinned-model"; unit.effort="high";
    assert(execution_plan_contract_from_work_unit(&unit,&adapted)); assert(!strcmp(adapted.required_lane.model,"pinned-model"));
    unit.executor="unsupported-cli"; assert(!execution_plan_contract_from_work_unit(&unit,&adapted));
    c.kind=WORK_UNIT_TOOL; strcpy(c.tool,"calculate"); strcpy(c.inputs,"{\"expression\":\"1+1\"}");
    assert(execution_plan_build(&c,1,NULL,0,&o,plan));
    assert(plan->jobs[0].mechanical && plan->jobs[0].attempts[0].candidate==-1 && plan->jobs[0].candidate_count==0);
    assert(plan->jobs[0].attempts[0].account_slots==0 && plan->jobs[0].attempts[0].lane_slots==0);
    c.tool[0]=0; assert(!execution_plan_build(&c,1,NULL,0,&o,plan));
    c.kind=WORK_UNIT_EDIT; assert(!execution_plan_build(&c,1,NULL,0,&o,plan));
    puts("PASS typed specified arithmetic/tool work uses mechanical placement; adapter preserves pins and rejects unsupported executor");
}
static void economics_and_policies(void) {
    ep_options_t o=options(); ep_contract_t c=contract("one"); ep_lane_t l[2]={lane("a",600,2),lane("b",400,2)};
    c.policy=EP_DELAYED_HEDGE; c.attempt_limit=2; c.hedge_delay_seconds=2; zero_components(&c);
    o.budget=money(1000,4); assert(execution_plan_build(&c,1,l,2,&o,plan));
    assert(plan->reserved.cash_microusd==1000 && plan->reserved.quota_units==4);
    assert(!plan->jobs[0].joint_hedge_reliability_known);
    char *json=execution_plan_json(plan); assert(strstr(json,"\"joint_hedge_reliability\":null")); assert(strstr(json,"shared-backend")); free(json);
    ep_job_evidence_t all[EP_MAX_JOBS]={0}; all[0].attempts[0]=accepted();
    assert(execution_plan_decide(plan,0,all,1003).transition==EP_FINISH);
    assert(plan->reserved.cash_microusd==1000); /* Acceptance does not refund other attempts. */
    l[1].attempt.cash_microusd=401; assert(!execution_plan_build(&c,1,l,2,&o,plan)); assert(!strcmp(plan->blocked_reason,"plan_budget_exceeded"));
    l[0]=lane("a",300,2); l[1]=lane("b",800,2); c.policy=EP_CASCADE;
    assert(!execution_plan_build(&c,1,l,2,&o,plan)); o.budget=money(1100,4);
    assert(execution_plan_build(&c,1,l,2,&o,plan)); assert(plan->reserved.cash_microusd==1100);
    o.budget.quota_units=3; assert(!execution_plan_build(&c,1,l,2,&o,plan));
    c=contract("one"); o=options(); l[0]=lane("a",100,2);
    assert(execution_plan_build(&c,1,l,1,&o,plan));
    ep_attempt_t *a=&plan->jobs[0].attempts[0]; assert(a->total.cash_microusd==200 && a->total.quota_units==6);
    assert(a->verification.cash_microusd==10 && a->synthesis.cash_microusd==20 && a->coordination.cash_microusd==30 && a->recovery.cash_microusd==40);
    assert(plan->jobs[0].candidates[0].cost_to_acceptance_microusd==223);
    c.recovery.known=false; assert(!execution_plan_build(&c,1,l,1,&o,plan));
    c=contract("one"); c.verification.cash_microusd=INT64_MAX;
    assert(!execution_plan_build(&c,1,l,1,&o,plan));
    puts("PASS full cash/quota components, hedge/cascade reservations, integer overflow and unknown bounds");
}
static void holds_and_reconciliation(void) {
    ep_options_t o=options(); ep_contract_t c=contract("one"); ep_lane_t l[2]={lane("a",400,2),lane("b",600,2)};
    c.policy=EP_DELAYED_HEDGE; c.attempt_limit=2; c.hedge_delay_seconds=2; zero_components(&c); o.budget=money(1000,4);
    assert(execution_plan_build(&c,1,l,2,&o,plan));
    ep_job_evidence_t all[EP_MAX_JOBS]={0}; all[0].attempts[0]=accepted(); all[0].attempts[1]=accepted();
    all[0].attempts[1].cancelled=true; all[0].attempts[1].billing_known=false;
    ep_decision_t d=execution_plan_decide(plan,0,all,1003);
    assert(d.transition==EP_RECONCILE && d.charge_hold.cash_microusd==600 && d.charge_hold.quota_units==2);
    ep_settlement_t settled[EP_MAX_JOBS][EP_MAX_ATTEMPTS]={0}; ep_accounting_t account;
    assert(execution_plan_accounting(plan,settled,&account)); assert(account.available.cash_microusd==0 && account.available.quota_units==0);
    settled[0][1]=(ep_settlement_t){true,{200,2,true}};
    assert(execution_plan_accounting(plan,settled,&account));
    assert(account.spent.cash_microusd==200 && account.held.cash_microusd==400 && account.available.cash_microusd==400);
    assert(account.available.quota_units==0); /* Slots released != quota restored. */
    all[0].attempts[1].billing_known=true; assert(execution_plan_decide(plan,0,all,1003).transition==EP_FINISH);
    settled[0][1].charged.cash_microusd=601; assert(!execution_plan_accounting(plan,settled,&account));
    puts("PASS cancelled unknown-billing holds, explicit settlement replaces hold, no automatic quota refunds");
}
static void acceptance_dependencies_and_deadlines(void) {
    ep_options_t o=options(); ep_contract_t c[2]={contract("one"),contract("two")}; ep_lane_t l=lane("bound",100,2);
    c[1].policy=EP_SYNTHESIS; c[1].dependency_count=1; strcpy(c[1].dependencies[0],"one");
    c[1].deadline=1009; assert(!execution_plan_build(c,2,&l,1,&o,plan));
    assert(!strcmp(plan->jobs[1].blocked_reason,"graph_deadline_exceeded"));
    c[1].deadline=1010; assert(execution_plan_build(c,2,&l,1,&o,plan)); /* 4+1 then 4+1 = accepted at 10 */
    ep_job_evidence_t all[EP_MAX_JOBS]={0}; all[0].attempts[0].started=true; all[0].attempts[0].transport_complete=true;
    all[0].attempts[0].billing_known=true; all[0].attempts[0].process_exit_zero=true;
    assert(!execution_plan_job_accepted(plan,0,all)); assert(execution_plan_decide(plan,1,all,1001).transition==EP_WAIT);
    all[0].attempts[0].route_verified=true; assert(execution_plan_decide(plan,1,all,1001).transition==EP_WAIT);
    all[0].attempts[0].semantic_accepted=true; assert(execution_plan_decide(plan,1,all,1005).transition==EP_DISPATCH);
    assert(execution_plan_decide(plan,1,all,1006).transition==EP_REPLAN);
    all[1].attempts[0]=accepted(); assert(execution_plan_job_accepted(plan,1,all));
    all[0].attempts[0].semantic_accepted=false; assert(!execution_plan_job_accepted(plan,1,all));
    c[1].deadline=1100; strcpy(c[1].dependencies[0],"missing"); assert(!execution_plan_build(c,2,&l,1,&o,plan)); assert(!strcmp(plan->blocked_reason,"missing_dependency"));
    strcpy(c[1].dependencies[0],"one"); c[0].dependency_count=1; strcpy(c[0].dependencies[0],"two");
    assert(!execution_plan_build(c,2,&l,1,&o,plan)); assert(!strcmp(plan->blocked_reason,"dependency_cycle"));
    puts("PASS exit/route/semantic acceptance gates, synthesis dependencies, graph verification deadline, missing edges/cycles");
}
static void fallback_and_effects(void) {
    ep_options_t o=options(); ep_contract_t c=contract("one"); ep_lane_t l[2]={lane("a",100,2),lane("b",200,2)};
    c.policy=EP_CASCADE; c.attempt_limit=2;
    assert(execution_plan_build(&c,1,l,2,&o,plan));
    ep_job_evidence_t all[EP_MAX_JOBS]={0}; assert(execution_plan_decide(plan,0,all,1001).transition==EP_DISPATCH);
    all[0].attempts[0]=(ep_outcome_t){.started=true,.billing_known=true,.started_at=1000};
    assert(execution_plan_decide(plan,0,all,1002).transition==EP_WAIT);
    all[0].attempts[0].transport_complete=true; assert(execution_plan_decide(plan,0,all,1002).transition==EP_RETRY);
    all[0].attempts[1]=accepted(); assert(execution_plan_decide(plan,0,all,1003).transition==EP_FINISH);
    c.policy=EP_DELAYED_HEDGE; c.hedge_delay_seconds=3; c.effect=EP_EXTERNAL_EFFECT;
    assert(!execution_plan_build(&c,1,l,2,&o,plan)); assert(!strcmp(plan->blocked_reason,"effectful_speculation_requires_isolation"));
    c.isolated_proposals=true; assert(execution_plan_build(&c,1,l,2,&o,plan));
    assert(plan->jobs[0].attempts[0].proposal_only && plan->jobs[0].attempts[1].proposal_only);
    assert(plan->jobs[0].attempts[0].designated_committer && !plan->jobs[0].attempts[1].designated_committer);
    memset(all,0,sizeof(all)); all[0].attempts[0]=(ep_outcome_t){.started=true,.billing_known=true,.started_at=1000};
    assert(execution_plan_decide(plan,0,all,1002).transition==EP_WAIT);
    assert(execution_plan_decide(plan,0,all,1003).transition==EP_HEDGE);
    all[0].attempts[0].effects=EP_EFFECT_UNRESOLVED;
    assert(execution_plan_decide(plan,0,all,1003).transition==EP_RECONCILE);
    all[0].attempts[0].effects=EP_EFFECT_NONE; assert(execution_plan_decide(plan,0,all,1003).transition==EP_HEDGE);
    all[0].attempts[1]=accepted(); all[0].attempts[1].effects=EP_EFFECT_ISOLATED;
    assert(execution_plan_decide(plan,0,all,1004).transition==EP_COMMIT);
    assert(!execution_plan_job_accepted(plan,0,all));
    all[0].attempts[0]=accepted(); all[0].attempts[0].effects=EP_EFFECT_COMMITTED;
    assert(execution_plan_decide(plan,0,all,1005).transition==EP_FINISH);
    all[0].attempts[1].effects=EP_EFFECT_COMMITTED; assert(execution_plan_decide(plan,0,all,1005).transition==EP_RECONCILE);
    c=contract("one"); assert(execution_plan_build(&c,1,l,1,&o,plan));
    memset(all,0,sizeof(all)); all[0].attempts[0]=accepted(); all[0].attempts[0].effects=EP_EFFECT_COMMITTED;
    assert(!execution_plan_job_accepted(plan,0,all)); assert(execution_plan_decide(plan,0,all,1003).transition==EP_RECONCILE);
    puts("PASS cascade trigger, delayed hedge, isolated proposals, designated commit, effect reconciliation and preserved authority");
}
static void deterministic_and_admission(void) {
    ep_options_t o=options(); ep_contract_t c[2]={contract("one"),contract("two")}; ep_lane_t l[2]={lane("a",100,2),lane("b",100,2)};
    strcpy(profile_topology.name,"triage"); profile_topology.total_agents=1;
    task_profile_t *profile=task_profile("parallel inspect and combine accepted results",NULL);
    assert(profile); o.profile=profile;
    assert(execution_plan_build(c,2,l,2,&o,plan)); char *first=execution_plan_json(plan);
    assert(json_is_valid_container(first)); assert(plan->jobs[0].attempts[0].candidate==0);
    assert(plan->topological_order[0]==0 && plan->topological_order[1]==1 && !plan->options.profile);
    ep_admission_request_t request; assert(execution_plan_admission_request(plan,1,0,&request));
    assert(request.account_slots==1 && request.lane_slots==1);
    assert(request.resources.cash_microusd==200 && !strcmp(request.binding.endpoint,"a"));
    assert(!strcmp(request.contract.inputs,c[1].inputs) && !strcmp(request.authority,c[1].authority));
    assert(!strcmp(request.acceptance,c[1].acceptance) && request.job_index==1);
    assert(!execution_plan_admission_request(plan,2,0,&request));
    assert(execution_plan_build(c,2,l,2,&o,plan)); char *second=execution_plan_json(plan);
    assert(!strcmp(first,second)); assert(strstr(first,"\"live_admission_supported\":false"));
    assert(strstr(first,"verified_exact_action_or_route")); free(first); free(second);
    task_profile_free(profile); o.profile=NULL;
    c[0].operator_authorized=false; assert(!execution_plan_build(c,2,l,2,&o,plan));
    assert(!execution_plan_admission_request(plan,0,0,&request));
    first=execution_plan_json(plan); assert(json_is_valid_container(first)); free(first);
    c[0]=contract("one"); memset(c[0].inputs,'x',sizeof(c[0].inputs));
    assert(!execution_plan_build(c,2,l,2,&o,plan)); first=execution_plan_json(plan); assert(json_is_valid_container(first)); free(first);
    assert(launches==0 && calls==0 && admissions==0);
    puts("PASS deterministic JSON and profile advice; same fabric/topology admission intent; zero worker/tool/admission calls");
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--dry-run")) {
        plan=calloc(1,sizeof(*plan)); assert(plan); ep_options_t o=options();
        ep_contract_t c=contract("inspect"); c.policy=EP_DELAYED_HEDGE; c.attempt_limit=2; c.hedge_delay_seconds=3;
        ep_lane_t l[3]={lane("bound-a",100,2),lane("bound-b",200,2),lane("unresolved",50,2)};
        l[2].endpoint_resolved=false; assert(execution_plan_build(&c,1,l,3,&o,plan));
        char *json=execution_plan_json(plan); puts(json); free(json); free(plan);
        assert(launches==0 && calls==0 && admissions==0); return 0;
    }
    plan=calloc(1,sizeof(*plan)); assert(plan);
    exact_and_eligibility(); estimates(); mechanical_and_adapter(); economics_and_policies();
    holds_and_reconciliation(); acceptance_dependencies_and_deadlines(); fallback_and_effects(); deterministic_and_admission();
    free(plan); puts("PASS execution planner offline integration"); return 0;
}
