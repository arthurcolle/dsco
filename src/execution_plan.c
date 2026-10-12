#include "execution_plan.h"
#include "json_util.h"
#include "work_dispatch.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool text_ok(const char *s, size_t cap, bool required) {
    return memchr(s, 0, cap) && (!required || *s);
}
#define TEXT(x, required) text_ok((x), sizeof(x), (required))
static bool resource_ok(ep_resource_t r) {
    return r.known && r.cash_microusd >= 0 && r.quota_units >= 0;
}
static bool add(ep_resource_t *a, ep_resource_t b) {
    if (!resource_ok(*a) || !resource_ok(b) ||
        b.cash_microusd > INT64_MAX - a->cash_microusd ||
        b.quota_units > INT64_MAX - a->quota_units) return false;
    a->cash_microusd += b.cash_microusd;
    a->quota_units += b.quota_units;
    return true;
}
static bool add_seconds(uint64_t *a, uint64_t b) {
    if (b > UINT64_MAX - *a) return false;
    *a += b; return true;
}
static bool within(ep_resource_t a, ep_resource_t budget) {
    return resource_ok(a) && resource_ok(budget) &&
           a.cash_microusd <= budget.cash_microusd && a.quota_units <= budget.quota_units;
}
static bool binding_ok(const ep_binding_t *b) {
    return TEXT(b->provider,true) && TEXT(b->model,true) && TEXT(b->endpoint,true) &&
           TEXT(b->account,true) && TEXT(b->auth_product,true) && TEXT(b->effort,true) &&
           TEXT(b->upstream,false) && TEXT(b->quantization,false);
}
static bool binding_equal(const ep_binding_t *a, const ep_binding_t *b) {
    return !strcmp(a->provider,b->provider) && !strcmp(a->model,b->model) &&
           !strcmp(a->endpoint,b->endpoint) && !strcmp(a->account,b->account) &&
           !strcmp(a->auth_product,b->auth_product) && !strcmp(a->effort,b->effort) &&
           !strcmp(a->upstream,b->upstream) && !strcmp(a->quantization,b->quantization);
}
static bool copy_text(char *out, size_t cap, const char *in) {
    if (!in) { *out=0; return true; }
    size_t n=strlen(in); if (n>=cap) return false;
    memcpy(out,in,n+1); return true;
}
bool execution_plan_contract_from_work_unit(const work_unit_t *w, ep_contract_t *c) {
    if (!w || !c || (w->executor && *w->executor) || !TEXT(w->id,true) || w->depends_count>EP_MAX_JOBS ||
        (w->depends_count && !w->depends_on)) return false;
    memset(c,0,sizeof(*c)); c->kind=w->kind;
    memcpy(c->id,w->id,sizeof(c->id));
    if (!copy_text(c->inputs,sizeof(c->inputs),w->payload) ||
        !copy_text(c->tool,sizeof(c->tool),w->tool) ||
        !copy_text(c->acceptance,sizeof(c->acceptance),w->acceptance_cmd)) return false;
    if (!copy_text(c->required_lane.provider,sizeof(c->required_lane.provider),w->provider) ||
        !copy_text(c->required_lane.model,sizeof(c->required_lane.model),w->model) ||
        !copy_text(c->required_lane.effort,sizeof(c->required_lane.effort),w->effort)) return false;
    c->dependency_count=w->depends_count;
    for (size_t i=0;i<w->depends_count;i++) {
        if (!text_ok(w->depends_on[i],WORK_ID_LEN,true)) return false;
        memcpy(c->dependencies[i],w->depends_on[i],WORK_ID_LEN);
    }
    /* No conversion from max_cost_usd double, or inferred operator authority. */
    return true;
}
static int dependency(const execution_plan_t *p, const char *id) {
    for (size_t i=0;i<p->job_count;i++) if (!strcmp(p->jobs[i].contract.id,id)) return (int)i;
    return -1;
}
static bool contract_ok(const ep_contract_t *c, const ep_options_t *o, const char **reason) {
    *reason="invalid_contract";
    if (!TEXT(c->id,true) || !TEXT(c->inputs,true) || !TEXT(c->tool,false) ||
        !TEXT(c->acceptance,true) || !TEXT(c->authority,true) || !TEXT(c->workload,true) ||
        !TEXT(c->validator,true) || !TEXT(c->auth_product,false) ||
        !TEXT(c->exact_endpoint,false) || !TEXT(c->required_lane.provider,false) ||
        !TEXT(c->required_lane.model,false) || !TEXT(c->required_lane.endpoint,false) ||
        !TEXT(c->required_lane.account,false) || !TEXT(c->required_lane.auth_product,false) ||
        !TEXT(c->required_lane.effort,false) || !TEXT(c->required_lane.upstream,false) ||
        !TEXT(c->required_lane.quantization,false) || c->dependency_count>EP_MAX_JOBS ||
        c->kind<WORK_UNIT_PROMPT || c->kind>WORK_UNIT_TOOL ||
        c->policy<EP_THROUGHPUT || c->policy>EP_SYNTHESIS ||
        c->effect<EP_READ_ONLY || c->effect>EP_EXTERNAL_EFFECT ||
        c->prompt_tokens<0 || c->output_tokens<0 || !c->attempt_limit ||
        c->attempt_limit>EP_MAX_ATTEMPTS) return false;
    for (size_t d=0;d<c->dependency_count;d++) {
        if (!TEXT(c->dependencies[d],true)) return false;
        for (size_t k=0;k<d;k++) if (!strcmp(c->dependencies[d],c->dependencies[k])) return false;
    }
    if (!c->operator_authorized) { *reason="operator_authorization_missing"; return false; }
    if (c->deadline<=o->at) { *reason="deadline_expired"; return false; }
    if (!c->component_durations_known) { *reason="unknown_component_duration"; return false; }
    if (!resource_ok(c->verification) || !resource_ok(c->synthesis) ||
        !resource_ok(c->coordination) || !resource_ok(c->recovery)) {
        *reason="unknown_component_cost"; return false;
    }
    if ((c->policy==EP_THROUGHPUT || c->policy==EP_SYNTHESIS) && c->attempt_limit!=1) return false;
    if (c->policy==EP_SYNTHESIS && !c->dependency_count) return false;
    if (c->policy==EP_DELAYED_HEDGE && (c->attempt_limit<2 || !c->hedge_delay_seconds)) return false;
    if (c->effect!=EP_READ_ONLY && c->attempt_limit>1 && !c->isolated_proposals) {
        *reason="effectful_speculation_requires_isolation"; return false;
    }
    return true;
}
static bool components(const ep_contract_t *c, ep_resource_t attempt,
                       uint64_t duration, ep_resource_t *total, uint64_t *seconds) {
    *total=attempt; *seconds=duration;
    return add(total,c->verification) && add(total,c->synthesis) &&
           add(total,c->coordination) && add(total,c->recovery) &&
           add_seconds(seconds,c->verification_seconds) &&
           add_seconds(seconds,c->synthesis_seconds) &&
           add_seconds(seconds,c->coordination_seconds) &&
           add_seconds(seconds,c->recovery_seconds);
}
/* Integer ceiling of amount * samples / successes, without float money. */
static bool acceptance_cost(int64_t amount, unsigned n, unsigned yes, int64_t *out) {
    if (!yes || !n || yes>n) return false;
    uint64_t a=(uint64_t)amount, whole=a/yes, rem=a%yes;
    if (whole>(uint64_t)INT64_MAX/n) return false;
    uint64_t extra=(rem*n+yes-1)/yes, base=whole*n;
    if (extra>(uint64_t)INT64_MAX-base) return false;
    *out=(int64_t)(base+extra); return true;
}
static void evaluate(const ep_contract_t *c, const ep_lane_t *l, const ep_options_t *o,
                     ep_candidate_t *r) {
    memset(r,0,sizeof(*r)); r->binding=l->binding;
    if (!binding_ok(&l->binding)) { r->rejections=EP_REJECT_IDENTITY; return; }
    if (!TEXT(l->workload,false) || !TEXT(l->validator,false) ||
        !TEXT(l->correlation_group,false)) { r->rejections=EP_REJECT_SCOPE; return; }
    strcpy(r->correlation_group,l->correlation_group);
    strcpy(r->workload,l->workload); strcpy(r->validator,l->validator);
    r->quote_observed_at=l->quote.observed_at; r->evidence_observed_at=l->evidence_observed_at;
    r->samples=l->quote.attempts; r->accepted_samples=l->quote.verified; r->uncertainty=l->uncertainty;
    if (!l->available) r->rejections|=EP_REJECT_UNAVAILABLE;
    /* Unresolved never equals an exact endpoint, even if its label matches. */
    if (!l->endpoint_resolved || (c->exact_endpoint[0] && strcmp(c->exact_endpoint,l->binding.endpoint))) r->rejections|=EP_REJECT_ENDPOINT;
    const ep_binding_t *pin=&c->required_lane, *b=&l->binding;
#define PIN(field) (pin->field[0] && strcmp(pin->field,b->field))
    if (PIN(provider) || PIN(model) || PIN(account) || PIN(effort) || PIN(upstream) || PIN(quantization)) r->rejections|=EP_REJECT_IDENTITY;
    if (PIN(endpoint)) r->rejections|=EP_REJECT_ENDPOINT;
    if (PIN(auth_product)) r->rejections|=EP_REJECT_AUTH;
#undef PIN
    if (c->auth_product[0] && strcmp(c->auth_product,l->binding.auth_product)) r->rejections|=EP_REJECT_AUTH;
    if (!l->operator_authorized) r->rejections|=EP_REJECT_AUTHORITY;
    if ((l->capabilities & c->capabilities)!=c->capabilities) r->rejections|=EP_REJECT_CAPABILITIES;
    if ((l->modalities & c->modalities)!=c->modalities) r->rejections|=EP_REJECT_MODALITY;
    if ((l->privacy_domains & c->privacy_domains)!=c->privacy_domains) r->rejections|=EP_REJECT_PRIVACY;
    if (c->capabilities && !l->tools) r->rejections|=EP_REJECT_TOOLS;
    if (l->quote.context_tokens<=0 || c->prompt_tokens>l->quote.context_tokens ||
        c->output_tokens>l->quote.context_tokens-c->prompt_tokens ||
        l->quote.max_output_tokens<c->output_tokens) r->rejections|=EP_REJECT_CONTEXT;
    if (strcmp(c->workload,l->workload) || strcmp(c->validator,l->validator)) r->rejections|=EP_REJECT_SCOPE;
    if (l->evidence_observed_at<=0 || l->evidence_observed_at>o->at ||
        difftime(o->at,l->evidence_observed_at)>o->max_evidence_age_seconds) r->rejections|=EP_REJECT_STALE;
    if (!isfinite(l->uncertainty) || l->uncertainty<0 || l->uncertainty>o->max_uncertainty) r->rejections|=EP_REJECT_UNCERTAINTY;
    if (!l->duration_upper_seconds || !components(c,l->attempt,l->duration_upper_seconds,&r->total,&r->duration_seconds)) r->rejections|=EP_REJECT_UNKNOWN;
    if (r->duration_seconds>(uint64_t)(c->deadline-o->at)) r->rejections|=EP_REJECT_DEADLINE;
    if (!within(r->total,o->budget)) r->rejections|=EP_REJECT_BUDGET;
    if (r->rejections) return; /* eligibility before statistical ranking */
    cost_frontier_candidate_t quote=l->quote;
    /* Pricing evidence is caller supplied for THIS bound identity. */
    if (!quote.provider || !quote.model || strcmp(quote.provider,l->binding.provider) || strcmp(quote.model,l->binding.model)) {
        r->rejections|=EP_REJECT_IDENTITY; return;
    }
    cost_frontier_scenario_t scenario={.enabled=true,.at=o->at,
        .prompt_tokens=c->prompt_tokens,.output_tokens=c->output_tokens,
        .require_tools=c->capabilities!=0,.max_quote_age_seconds=o->max_quote_age_seconds,
        .min_attempts=o->min_samples,.minimum_success_lower_bound=o->minimum_success_lower_bound,
        .max_latency_seconds=(double)(c->deadline-o->at)};
    size_t order;
    if (!cost_frontier_rank(&quote,1,&scenario,&r->evidence,&order)) {
        r->rejections|=EP_REJECT_FRONTIER;
        copy_text(r->frontier_reason,sizeof(r->frontier_reason),r->evidence.reason);
        return;
    }
    strcpy(r->frontier_reason,"eligible");
    if (!acceptance_cost(r->total.cash_microusd,quote.attempts,quote.verified,&r->cost_to_acceptance_microusd) ||
        !acceptance_cost(r->total.quota_units,quote.attempts,quote.verified,&r->quota_to_acceptance_units)) r->rejections|=EP_REJECT_UNKNOWN;
}
static bool better(const ep_candidate_t *a, const ep_candidate_t *b) {
    if (a->cost_to_acceptance_microusd!=b->cost_to_acceptance_microusd)
        return a->cost_to_acceptance_microusd<b->cost_to_acceptance_microusd;
    if (a->quota_to_acceptance_units!=b->quota_to_acceptance_units)
        return a->quota_to_acceptance_units<b->quota_to_acceptance_units;
    return a->duration_seconds<b->duration_seconds;
}
static void set_attempt(ep_attempt_t *a, const ep_contract_t *c, int index,
                        const ep_lane_t *l, const char *reason, bool first) {
    memset(a,0,sizeof(*a)); a->candidate=index;
    if (l) { a->binding=l->binding; a->account_slots=1; a->lane_slots=1; }
    a->attempt=l?l->attempt:c->mechanical;
    a->verification=c->verification; a->synthesis=c->synthesis;
    a->coordination=c->coordination; a->recovery=c->recovery;
    components(c,a->attempt,l?l->duration_upper_seconds:c->mechanical_duration_seconds,&a->total,&a->duration_seconds);
    a->proposal_only=c->effect!=EP_READ_ONLY && c->isolated_proposals;
    a->designated_committer=c->effect!=EP_READ_ONLY && first;
    strcpy(a->reason,reason);
}
static bool block(execution_plan_t *p, const char *reason) {
    p->ready=false; strcpy(p->blocked_reason,reason); return false;
}
bool execution_plan_build(const ep_contract_t *contracts, size_t n,
                          const ep_lane_t *lanes, size_t count,
                          const ep_options_t *o, execution_plan_t *p) {
    if (!p) return false;
    memset(p,0,sizeof(*p)); p->reserved.known=true;
    if (!o || !contracts || !n || n>EP_MAX_JOBS || count>EP_MAX_LANES || (count && !lanes)) return block(p,"invalid_input");
    p->options=*o; p->options.profile=NULL;
    if (o->at<=0 || !o->max_evidence_age_seconds || !o->max_quote_age_seconds ||
        !o->min_samples || !isfinite(o->minimum_success_lower_bound) ||
        o->minimum_success_lower_bound<0 || o->minimum_success_lower_bound>1 ||
        !isfinite(o->max_uncertainty) || o->max_uncertainty<0 || o->max_uncertainty>1 ||
        !resource_ok(o->budget)) return block(p,"invalid_options");
    if (o->profile) {
        if (!isfinite(o->profile->parallelism_score) || !isfinite(o->profile->convergence_score)) return block(p,"invalid_profile");
        p->parallelism_hint=o->profile->parallelism_score;
        p->convergence_hint=o->profile->convergence_score;
    }
    p->job_count=n;
    for (size_t i=0;i<n;i++) {
        p->jobs[i].contract=contracts[i]; const char *reason;
        if (!contract_ok(&contracts[i],o,&reason)) {
            strcpy(p->jobs[i].blocked_reason,reason); return block(p,reason);
        }
        for (size_t j=0;j<i;j++) if (!strcmp(contracts[i].id,contracts[j].id)) return block(p,"duplicate_job_id");
    }
    /* Deterministic Kahn order, preserving original order for independent jobs. */
    bool sorted[EP_MAX_JOBS]={false}; size_t done=0;
    while (done<n) {
        bool progress=false;
        for (size_t i=0;i<n;i++) if (!sorted[i]) {
            bool clear=true;
            for (size_t d=0;d<contracts[i].dependency_count;d++) {
                int dep=dependency(p,contracts[i].dependencies[d]);
                if (dep<0) return block(p,"missing_dependency");
                if (!sorted[dep]) clear=false;
            }
            if (clear) { sorted[i]=true; p->topological_order[done++]=i; progress=true; }
        }
        if (!progress) return block(p,"dependency_cycle");
    }
    bool feasible=true;
    for (size_t i=0;i<n;i++) {
        ep_job_t *j=&p->jobs[i]; const ep_contract_t *c=&j->contract;
        work_unit_t unit={.kind=c->kind,.tool=(char *)c->tool};
        j->mechanical=work_dispatch_unit_is_mechanical(&unit);
        j->allowed_transitions=EP_ALLOW_REPLAN|EP_ALLOW_RECONCILE|EP_ALLOW_FINISH;
        if (c->effect!=EP_READ_ONLY) j->allowed_transitions|=EP_ALLOW_COMMIT;
        if (c->policy==EP_CASCADE && c->attempt_limit>1) j->allowed_transitions|=EP_ALLOW_RETRY;
        if (c->policy==EP_DELAYED_HEDGE) j->allowed_transitions|=EP_ALLOW_HEDGE;
        if (j->mechanical) {
            if (c->attempt_limit!=1 || c->policy==EP_DELAYED_HEDGE || c->exact_endpoint[0] ||
                c->required_lane.provider[0] || c->required_lane.model[0] || c->required_lane.endpoint[0] ||
                c->required_lane.account[0] || c->required_lane.auth_product[0] || c->required_lane.effort[0] ||
                c->required_lane.upstream[0] || c->required_lane.quantization[0] ||
                !c->mechanical_duration_seconds || !resource_ok(c->mechanical) ||
                !components(c,c->mechanical,c->mechanical_duration_seconds,&j->attempts[0].total,&j->attempts[0].duration_seconds)) {
                strcpy(j->blocked_reason,"invalid_mechanical_contract"); feasible=false; continue;
            }
            set_attempt(&j->attempts[0],c,-1,NULL,"specified_tool_via_capability_gate",true);
            j->attempt_count=1;
        } else {
            if (c->kind==WORK_UNIT_TOOL) { strcpy(j->blocked_reason,"specified_tool_missing"); feasible=false; continue; }
            j->candidate_count=count;
            size_t order[EP_MAX_LANES], eligible=0;
            for (size_t k=0;k<count;k++) {
                evaluate(c,&lanes[k],o,&j->candidates[k]);
                for (size_t prev=0;prev<k;prev++) if (binding_ok(&lanes[k].binding) && binding_ok(&lanes[prev].binding) && binding_equal(&lanes[k].binding,&lanes[prev].binding)) j->candidates[k].rejections|=EP_REJECT_IDENTITY;
                if (j->candidates[k].rejections) continue;
                size_t pos=eligible;
                while (pos && better(&j->candidates[k],&j->candidates[order[pos-1]])) { order[pos]=order[pos-1]; pos--; }
                order[pos]=k; eligible++;
            }
            if (eligible<c->attempt_limit) { strcpy(j->blocked_reason,"insufficient_eligible_lanes"); feasible=false; continue; }
            for (size_t k=0;k<c->attempt_limit;k++) {
                const char *reason=k==0?"lowest_estimated_cost_to_acceptance":c->policy==EP_CASCADE?"observed_rejection_or_transport_failure":"deadline_hedge_after_delay";
                set_attempt(&j->attempts[k],c,(int)order[k],&lanes[order[k]],reason,k==0);
            }
            j->attempt_count=c->attempt_limit;
        }
        for (size_t k=0;k<j->attempt_count;k++) {
            if (!add(&p->reserved,j->attempts[k].total)) { strcpy(j->blocked_reason,"reservation_overflow"); feasible=false; break; }
            if (c->policy==EP_CASCADE) {
                if (!add_seconds(&j->duration_upper_seconds,j->attempts[k].duration_seconds)) { strcpy(j->blocked_reason,"duration_overflow"); feasible=false; }
            } else {
                uint64_t end=j->attempts[k].duration_seconds;
                if (k && !add_seconds(&end,(uint64_t)c->hedge_delay_seconds*k)) { strcpy(j->blocked_reason,"duration_overflow"); feasible=false; }
                if (end>j->duration_upper_seconds) j->duration_upper_seconds=end;
            }
        }
        j->ready=!j->blocked_reason[0];
    }
    /* Deadline feasibility includes dependency completion and all authorized
     * cascade attempts; throughput jobs may overlap, never synthesized by max_agents. */
    uint64_t finish[EP_MAX_JOBS]={0};
    for (size_t oidx=0;oidx<n;oidx++) {
        size_t i=p->topological_order[oidx]; ep_job_t *j=&p->jobs[i]; uint64_t start=0;
        for (size_t d=0;d<j->contract.dependency_count;d++) {
            int dep=dependency(p,j->contract.dependencies[d]);
            if (finish[dep]>start) start=finish[dep];
        }
        finish[i]=start;
        if (!add_seconds(&finish[i],j->duration_upper_seconds) || finish[i]>(uint64_t)(j->contract.deadline-o->at)) {
            j->ready=false; strcpy(j->blocked_reason,"graph_deadline_exceeded"); feasible=false;
        }
    }
    if (!feasible) return block(p,"blocked_jobs");
    if (!within(p->reserved,o->budget)) return block(p,"plan_budget_exceeded");
    p->ready=true; return true;
}

bool execution_plan_admission_request(const execution_plan_t *p,size_t job,size_t attempt,
                                      ep_admission_request_t *out) {
    if (!out) return false;
    memset(out,0,sizeof(*out));
    if (!p || !p->ready || job>=p->job_count || !p->jobs[job].ready || attempt>=p->jobs[job].attempt_count) return false;
    const ep_job_t *j=&p->jobs[job]; const ep_attempt_t *a=&j->attempts[attempt];
    out->contract=j->contract; out->placement=*a; out->job_index=job; out->attempt_index=attempt;
    out->binding=a->binding; out->resources=a->total;
    out->account_slots=a->account_slots; out->lane_slots=a->lane_slots;
    out->deadline=j->contract.deadline; out->proposal_only=a->proposal_only;
    out->designated_committer=a->designated_committer;
    strcpy(out->authority,j->contract.authority); strcpy(out->acceptance,j->contract.acceptance);
    return true;
}
static bool accepted_attempt(const ep_job_t *j,size_t k,const ep_outcome_t *e) {
    if (!e->started || !e->transport_complete || !e->route_verified || !e->semantic_accepted || e->cancelled || !e->billing_known) return false;
    if (j->contract.effect==EP_READ_ONLY) return e->effects==EP_EFFECT_NONE;
    return j->attempts[k].designated_committer && e->effects==EP_EFFECT_COMMITTED;
}
bool execution_plan_job_accepted(const execution_plan_t *p,size_t job,const ep_job_evidence_t *evidence) {
    if (!p || !p->ready || !evidence || job>=p->job_count) return false;
    const ep_job_t *j=&p->jobs[job];
    for (size_t d=0;d<j->contract.dependency_count;d++) {
        int index=dependency(p,j->contract.dependencies[d]);
        if (index<0 || !execution_plan_job_accepted(p,(size_t)index,evidence)) return false;
    }
    for (size_t k=0;k<j->attempt_count;k++) {
        const ep_outcome_t *e=&evidence[job].attempts[k];
        /* Unknown effects in ANY launched attempt fence graph advancement. */
        if (e->started && (e->effects==EP_EFFECT_UNRESOLVED ||
            (j->contract.effect==EP_READ_ONLY && e->effects!=EP_EFFECT_NONE) ||
            (!j->attempts[k].designated_committer && e->effects==EP_EFFECT_COMMITTED))) return false;
    }
    for (size_t k=0;k<j->attempt_count;k++) if (accepted_attempt(j,k,&evidence[job].attempts[k])) return true;
    return false;
}
ep_decision_t execution_plan_decide(const execution_plan_t *p,size_t job,
                                    const ep_job_evidence_t *all,time_t at) {
    ep_decision_t d={.transition=EP_BLOCK,.attempt=-1,.charge_hold={.known=true},.reason="invalid_plan_or_evidence"};
    if (!p || !p->ready || !all || job>=p->job_count) return d;
    const ep_job_t *j=&p->jobs[job]; bool reconcile=false;
    for (size_t k=0;k<j->attempt_count;k++) {
        const ep_outcome_t *e=&all[job].attempts[k];
        if (!e->started) continue;
        if (e->effects==EP_EFFECT_UNRESOLVED ||
            (j->contract.effect==EP_READ_ONLY && e->effects!=EP_EFFECT_NONE) ||
            (!j->attempts[k].designated_committer && e->effects==EP_EFFECT_COMMITTED)) reconcile=true;
        if (!e->billing_known) { add(&d.charge_hold,j->attempts[k].attempt); reconcile=true; }
        /* Even a semantically rejected committed mutation cannot be repeated. */
        if (e->effects==EP_EFFECT_COMMITTED && !accepted_attempt(j,k,e)) reconcile=true;
    }
    if (reconcile) { d.transition=EP_RECONCILE; d.reason="unresolved_charge_or_external_effect"; return d; }
    if (execution_plan_job_accepted(p,job,all)) { d.transition=EP_FINISH; d.reason="verified_route_and_semantic_acceptance"; return d; }
    if (at<p->options.at || at>=j->contract.deadline) { d.transition=EP_REPLAN; d.reason="deadline_requires_new_plan"; return d; }
    for (size_t dep=0;dep<j->contract.dependency_count;dep++) {
        int index=dependency(p,j->contract.dependencies[dep]);
        if (index<0 || !execution_plan_job_accepted(p,(size_t)index,all)) {
            d.transition=EP_WAIT; d.reason="dependencies_require_accepted_inputs"; return d;
        }
    }
    /* An isolated winner is a proposal, never permission for a second commit.
     * Transfer to designated committer is an explicit future consumer step. */
    for (size_t k=0;k<j->attempt_count;k++) {
        const ep_outcome_t *e=&all[job].attempts[k];
        if (e->started && e->transport_complete && e->route_verified && e->semantic_accepted &&
            !e->cancelled && e->effects==EP_EFFECT_ISOLATED && j->contract.effect!=EP_READ_ONLY) {
            d.transition=EP_COMMIT; d.attempt=0; d.reason="designated_committer_requires_authorized_proposal"; return d;
        }
    }
    for (size_t k=0;k<j->attempt_count;k++) if (!all[job].attempts[k].started) {
        if (j->attempts[k].duration_seconds>(uint64_t)(j->contract.deadline-at)) {
            d.transition=EP_REPLAN; d.reason="remaining_deadline_insufficient"; return d;
        }
        if (k==0) { d.transition=EP_DISPATCH; d.attempt=0; d.reason="dependencies_accepted_live_lease_required"; return d; }
        const ep_outcome_t *prev=&all[job].attempts[k-1];
        if (j->contract.policy==EP_CASCADE && (prev->transport_complete || prev->cancelled)) {
            d.transition=EP_RETRY; d.attempt=(int)k; d.reason="observed_rejection_with_preserved_authority"; return d;
        }
        if (j->contract.policy==EP_DELAYED_HEDGE && prev->started_at>0 && prev->started_at<=at &&
            difftime(at,prev->started_at)>=j->contract.hedge_delay_seconds) {
            d.transition=EP_HEDGE; d.attempt=(int)k; d.reason="delay_elapsed_joint_reliability_unknown"; return d;
        }
        d.transition=EP_WAIT; d.reason="fallback_trigger_not_observed"; return d;
    }
    for (size_t k=0;k<j->attempt_count;k++) if (!all[job].attempts[k].transport_complete && !all[job].attempts[k].cancelled) {
        d.transition=EP_WAIT; d.reason="attempt_in_flight"; return d;
    }
    d.transition=EP_REPLAN; d.reason="authorized_attempts_exhausted"; return d;
}

bool execution_plan_accounting(const execution_plan_t *p,
    const ep_settlement_t settled[EP_MAX_JOBS][EP_MAX_ATTEMPTS], ep_accounting_t *out) {
    if (!out) return false;
    memset(out,0,sizeof(*out));
    out->spent.known=out->held.known=out->available.known=true;
    if (!p || !p->ready || !settled) return false;
    out->held=p->reserved;
    for (size_t i=0;i<p->job_count;i++) for (size_t k=0;k<p->jobs[i].attempt_count;k++) {
        if (!settled[i][k].reconciled) continue;
        ep_resource_t hold=p->jobs[i].attempts[k].attempt, charge=settled[i][k].charged;
        /* Charges over the authorized bound require a new accounting review. */
        if (!within(charge,hold) || !add(&out->spent,charge)) return false;
        out->held.cash_microusd-=hold.cash_microusd;
        out->held.quota_units-=hold.quota_units;
    }
    ep_resource_t used=out->spent;
    if (!add(&used,out->held) || !within(used,p->options.budget)) return false;
    out->available=p->options.budget;
    out->available.cash_microusd-=used.cash_microusd;
    out->available.quota_units-=used.quota_units;
    return true;
}

static void js_text(jbuf_t *b,const char *key,const char *value,size_t cap) {
    /* Even malformed rejected input can be rendered safely for inspection. */
    char copy[EP_TEXT+1]; size_t n=0;
    while (n<cap && value[n]) n++;
    if (n>EP_TEXT) n=EP_TEXT;
    memcpy(copy,value,n); copy[n]=0;
    jbuf_append_json_str(b,key); jbuf_append(b,":"); jbuf_append_json_str(b,copy);
}
#define JS(b,key,value) js_text(b,key,value,sizeof(value))
static void js_resource(jbuf_t *b,ep_resource_t r) {
    if (!resource_ok(r)) { jbuf_append(b,"{\"known\":false,\"cash_microusd\":null,\"quota_units\":null}"); return; }
    jbuf_appendf(b,"{\"known\":true,\"cash_microusd\":%lld,\"quota_units\":%lld}",(long long)r.cash_microusd,(long long)r.quota_units);
}
static void js_binding(jbuf_t *b,const ep_binding_t *v) {
    jbuf_append(b,"{"); JS(b,"provider",v->provider); jbuf_append(b,",");
    JS(b,"model",v->model); jbuf_append(b,","); JS(b,"endpoint",v->endpoint); jbuf_append(b,",");
    JS(b,"account",v->account); jbuf_append(b,","); JS(b,"auth_product",v->auth_product); jbuf_append(b,",");
    JS(b,"effort",v->effort); jbuf_append(b,","); JS(b,"upstream",v->upstream); jbuf_append(b,",");
    JS(b,"quantization",v->quantization); jbuf_append(b,"}");
}
static void js_rejections(jbuf_t *b,uint64_t reasons) {
    const char *names[]={"unavailable_catalog_lane","endpoint_unresolved_or_mismatch","identity_missing_or_duplicate",
        "authentication_product_mismatch","operator_authorization_missing","capabilities_missing","modality_missing",
        "privacy_domain_mismatch","context_unavailable_or_exceeded","tools_missing","workload_or_validator_scope_mismatch",
        "evidence_stale_or_unknown","cost_or_duration_unknown_or_overflow","uncertainty_unusable","frontier_evidence_unqualified",
        "deadline_exceeded","budget_exceeded"};
    jbuf_append(b,"["); bool comma=false;
    for (size_t i=0;i<sizeof(names)/sizeof(*names);i++) if (reasons & (1ULL<<i)) {
        if (comma) jbuf_append(b,","); jbuf_append_json_str(b,names[i]); comma=true;
    }
    jbuf_append(b,"]");
}
static const char *policy_name(ep_policy_t p) {
    switch (p) { case EP_THROUGHPUT:return "throughput"; case EP_CASCADE:return "sequential_cascade";
        case EP_DELAYED_HEDGE:return "delayed_hedge"; case EP_SYNTHESIS:return "synthesis_accepted_inputs"; }
    return "invalid";
}
static void js_contract(jbuf_t *b,const ep_contract_t *c) {
    jbuf_append(b,"{"); JS(b,"id",c->id); jbuf_append(b,","); JS(b,"inputs",c->inputs); jbuf_append(b,",");
    JS(b,"tool",c->tool); jbuf_append(b,","); JS(b,"acceptance",c->acceptance); jbuf_append(b,",");
    JS(b,"authority",c->authority); jbuf_append(b,","); JS(b,"workload",c->workload); jbuf_append(b,",");
    JS(b,"validator",c->validator); jbuf_append(b,","); JS(b,"auth_product",c->auth_product); jbuf_append(b,",");
    JS(b,"exact_endpoint",c->exact_endpoint); jbuf_append(b,",\"required_lane\":"); js_binding(b,&c->required_lane);
    jbuf_appendf(b,",\"kind\":%d,\"effect\":%d,\"capabilities\":%llu,\"modalities\":%llu,\"privacy_domains\":%llu,\"deadline\":%lld,\"prompt_tokens\":%lld,\"output_tokens\":%lld,\"attempt_limit\":%u,\"hedge_delay_seconds\":%u,\"operator_authorized\":%s,\"isolated_proposals\":%s,\"component_durations_known\":%s,\"policy\":",
        c->kind,c->effect,(unsigned long long)c->capabilities,(unsigned long long)c->modalities,
        (unsigned long long)c->privacy_domains,(long long)c->deadline,(long long)c->prompt_tokens,
        (long long)c->output_tokens,c->attempt_limit,c->hedge_delay_seconds,c->operator_authorized?"true":"false",
        c->isolated_proposals?"true":"false",c->component_durations_known?"true":"false");
    jbuf_append_json_str(b,policy_name(c->policy));
    jbuf_append(b,",\"dependencies\":[");
    for (size_t i=0;i<c->dependency_count && i<EP_MAX_JOBS;i++) {
        if (i) jbuf_append(b,","); char copy[WORK_ID_LEN+1]; memcpy(copy,c->dependencies[i],WORK_ID_LEN); copy[WORK_ID_LEN]=0; jbuf_append_json_str(b,copy);
    }
    jbuf_append(b,"],\"mechanical\":"); js_resource(b,c->mechanical);
    jbuf_append(b,",\"verification\":"); js_resource(b,c->verification);
    jbuf_append(b,",\"possible_synthesis\":"); js_resource(b,c->synthesis);
    jbuf_append(b,",\"coordination\":"); js_resource(b,c->coordination);
    jbuf_append(b,",\"authorized_recovery\":"); js_resource(b,c->recovery);
    jbuf_appendf(b,",\"duration_bounds_seconds\":{\"mechanical\":%llu,\"verification\":%llu,\"possible_synthesis\":%llu,\"coordination\":%llu,\"authorized_recovery\":%llu}}",
        (unsigned long long)c->mechanical_duration_seconds,(unsigned long long)c->verification_seconds,
        (unsigned long long)c->synthesis_seconds,(unsigned long long)c->coordination_seconds,(unsigned long long)c->recovery_seconds);
}
char *execution_plan_json(const execution_plan_t *p) {
    if (!p) return NULL;
    jbuf_t b; jbuf_init(&b,8192);
    jbuf_appendf(&b,"{\"schema\":\"dsco.execution_plan.v1\",\"pure_intent\":true,\"live_admission_supported\":false,\"ready\":%s,\"at\":%lld,\"blocked_reason\":",p->ready?"true":"false",(long long)p->options.at);
    char blocked[97]; memcpy(blocked,p->blocked_reason,96); blocked[96]=0; jbuf_append_json_str(&b,blocked);
    jbuf_append(&b,",\"budget\":"); js_resource(&b,p->options.budget);
    jbuf_append(&b,",\"planned_reservations\":"); js_resource(&b,p->reserved);
    jbuf_appendf(&b,",\"evidence_requirements\":{\"max_age_seconds\":%u,\"max_quote_age_seconds\":%u,\"min_samples\":%u,\"minimum_success_lower_bound\":%.17g,\"max_uncertainty\":%.17g},\"profile_advice\":{\"parallelism\":%.17g,\"convergence\":%.17g},\"topological_order\":[",
        p->options.max_evidence_age_seconds,p->options.max_quote_age_seconds,p->options.min_samples,
        isfinite(p->options.minimum_success_lower_bound)?p->options.minimum_success_lower_bound:0,
        isfinite(p->options.max_uncertainty)?p->options.max_uncertainty:0,p->parallelism_hint,p->convergence_hint);
    for (size_t i=0;i<p->job_count;i++) { if (i) jbuf_append(&b,","); jbuf_appendf(&b,"%zu",p->topological_order[i]); }
    jbuf_append(&b,"],\"jobs\":[");
    for (size_t i=0;i<p->job_count;i++) {
        const ep_job_t *j=&p->jobs[i]; if (i) jbuf_append(&b,",");
        jbuf_append(&b,"{\"contract\":"); js_contract(&b,&j->contract);
        jbuf_appendf(&b,",\"ready\":%s,\"mechanical\":%s,\"blocked_reason\":",j->ready?"true":"false",j->mechanical?"true":"false");
        jbuf_append_json_str(&b,j->blocked_reason);
        jbuf_appendf(&b,",\"allowed_transitions\":%u,\"joint_hedge_reliability\":null,\"duration_upper_seconds\":%llu,\"acceptance_gates\":[\"transport_completion\",\"verified_exact_action_or_route\",\"semantic_validator\",\"effects_reconciled\",\"designated_authorized_commit_if_effectful\"],\"candidates\":[",j->allowed_transitions,(unsigned long long)j->duration_upper_seconds);
        for (size_t k=0;k<j->candidate_count;k++) {
            const ep_candidate_t *r=&j->candidates[k]; if (k) jbuf_append(&b,",");
            jbuf_append(&b,"{\"binding\":"); js_binding(&b,&r->binding);
            jbuf_append(&b,",\"rejections\":"); js_rejections(&b,r->rejections);
            jbuf_append(&b,","); JS(&b,"frontier_reason",r->frontier_reason); jbuf_append(&b,",");
            JS(&b,"correlation_group",r->correlation_group); jbuf_append(&b,",");
            JS(&b,"workload",r->workload); jbuf_append(&b,","); JS(&b,"validator",r->validator);
            jbuf_appendf(&b,",\"samples\":%u,\"accepted_samples\":%u,\"evidence_observed_at\":%lld,\"quote_observed_at\":%lld,\"uncertainty\":",r->samples,r->accepted_samples,(long long)r->evidence_observed_at,(long long)r->quote_observed_at);
            if (isfinite(r->uncertainty)) jbuf_appendf(&b,"%.17g",r->uncertainty); else jbuf_append(&b,"null");
            jbuf_append(&b,",\"success_lower_bound\":");
            if (!r->rejections) jbuf_appendf(&b,"%.17g",r->evidence.success_lower_bound); else jbuf_append(&b,"null");
            jbuf_append(&b,",\"estimated_cost_to_acceptance\":");
            if (!r->rejections) js_resource(&b,(ep_resource_t){r->cost_to_acceptance_microusd,r->quota_to_acceptance_units,true}); else js_resource(&b,(ep_resource_t){0});
            jbuf_append(&b,",\"reservation_bound\":"); js_resource(&b,r->total); jbuf_append(&b,"}");
        }
        jbuf_append(&b,"],\"attempts\":[");
        for (size_t k=0;k<j->attempt_count;k++) {
            const ep_attempt_t *a=&j->attempts[k]; if (k) jbuf_append(&b,",");
            jbuf_appendf(&b,"{\"candidate\":%d,\"reason\":",a->candidate); jbuf_append_json_str(&b,a->reason);
            jbuf_append(&b,",\"binding\":"); js_binding(&b,&a->binding);
            jbuf_appendf(&b,",\"account_slots\":%u,\"lane_slots\":%u,\"proposal_only\":%s,\"designated_committer\":%s,\"duration_seconds\":%llu,\"attempt\":",a->account_slots,a->lane_slots,a->proposal_only?"true":"false",a->designated_committer?"true":"false",(unsigned long long)a->duration_seconds);
            js_resource(&b,a->attempt); jbuf_append(&b,",\"verification\":"); js_resource(&b,a->verification);
            jbuf_append(&b,",\"possible_synthesis\":"); js_resource(&b,a->synthesis);
            jbuf_append(&b,",\"coordination\":"); js_resource(&b,a->coordination);
            jbuf_append(&b,",\"authorized_recovery\":"); js_resource(&b,a->recovery);
            jbuf_append(&b,",\"total\":"); js_resource(&b,a->total); jbuf_append(&b,"}");
        }
        jbuf_append(&b,"]}");
    }
    jbuf_append(&b,"]}"); return b.data;
}
