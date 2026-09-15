#include "cost_frontier.h"
#include <math.h>
#include <string.h>
bool cost_frontier_auth_compatible(const char *provider,const char *auth,bool local,bool api_key_transport,bool subscription_endpoint){
    if(!provider||!auth)return false;
    if(!strcmp(provider,"anthropic"))return false;
    if(!strcmp(provider,"sakana"))return !strcmp(auth,"payg")||!strcmp(auth,"subscription");
    if(local)return !strcmp(auth,"local");
    if(subscription_endpoint)return !strcmp(auth,"subscription");
    return api_key_transport&&!strcmp(auth,"api");
}
static bool price(double p){return isfinite(p)&&p>=0;}
static void wilson(unsigned yes,unsigned n,double *lo,double *hi){
    double z=1.959963984540054,p=(double)yes/n,d=1+z*z/n;
    double center=(p+z*z/(2*n))/d,half=z*sqrt(p*(1-p)/n+z*z/(4.0*n*n))/d;
    *lo=fmax(0,center-half);*hi=fmin(1,center+half);
}
static const char *evaluate(const cost_frontier_candidate_t *c,const cost_frontier_scenario_t *s,cost_frontier_result_t *r){
    if(!c->provider||!*c->provider||!c->model||!*c->model)return "missing_lane_identity";
    if(!c->route_specific||!c->source||!*c->source)return "route_quote_unavailable";
    if(c->observed_at<=0||c->observed_at>s->at||difftime(s->at,c->observed_at)>s->max_quote_age_seconds)return "quote_time_unusable";
    if(c->cached_tokens<0||c->cached_tokens>s->prompt_tokens)return "invalid_cache_scope";
    if(c->context_tokens<=0||s->prompt_tokens>c->context_tokens||s->output_tokens>c->context_tokens-s->prompt_tokens)return "context_unavailable_or_exceeded";
    if(c->max_output_tokens<=0||s->output_tokens>c->max_output_tokens)return "output_limit_unavailable_or_exceeded";
    if(s->require_tools&&!c->tools)return "tools_unavailable";
    int64_t input=s->prompt_tokens-c->cached_tokens;
    if(!price(c->request_fee)||(input&&!price(c->input_per_million))||(c->cached_tokens&&!price(c->cached_per_million))||(s->output_tokens&&!price(c->output_per_million)))return "price_unavailable";
    if(c->attempts<s->min_attempts||!c->attempts||!c->verified||c->verified>c->attempts||!isfinite(c->mean_latency_seconds)||c->mean_latency_seconds<=0)return "verification_evidence_unavailable";
    if(c->mean_latency_seconds>s->max_latency_seconds)return "latency_ceiling_exceeded";
    wilson(c->verified,c->attempts,&r->success_lower_bound,&r->success_upper_bound);
    if(r->success_lower_bound<s->minimum_success_lower_bound)return "success_evidence_below_threshold";
    r->cost_per_call=c->request_fee+(input?input*c->input_per_million/1e6:0)+(c->cached_tokens?c->cached_tokens*c->cached_per_million/1e6:0)+(s->output_tokens?s->output_tokens*c->output_per_million/1e6:0);
    double success=(double)c->verified/c->attempts;
    r->cost_per_verified=r->cost_per_call/success;
    r->seconds_per_verified=c->mean_latency_seconds/success;
    if(!price(r->cost_per_verified)||!price(r->seconds_per_verified))return "projection_overflow";
    r->billed_free=r->cost_per_call==0;
    return NULL;
}
static bool better(const cost_frontier_result_t *a,const cost_frontier_result_t *b){
    if(a->pareto!=b->pareto)return a->pareto;
    if(a->cost_per_verified!=b->cost_per_verified)return a->cost_per_verified<b->cost_per_verified;
    return a->seconds_per_verified<b->seconds_per_verified;
}
size_t cost_frontier_rank(const cost_frontier_candidate_t *c,size_t n,const cost_frontier_scenario_t *s,cost_frontier_result_t *r,size_t *order){
    if(!r||!order||!s||(!c&&n))return 0;
    bool valid=s->prompt_tokens>=0&&s->output_tokens>=0&&s->at>0&&s->max_quote_age_seconds>0&&s->min_attempts>0&&isfinite(s->minimum_success_lower_bound)&&s->minimum_success_lower_bound>=0&&s->minimum_success_lower_bound<=1&&isfinite(s->max_latency_seconds)&&s->max_latency_seconds>0;
    size_t count=0;
    for(size_t i=0;i<n;i++){
        memset(&r[i],0,sizeof(r[i]));
        r[i].reason=!s->enabled?"disabled":!valid?"invalid_scenario":evaluate(&c[i],s,&r[i]);
        r[i].eligible=r[i].reason==NULL;
        if(r[i].eligible){r[i].reason="eligible";r[i].pareto=true;order[count++]=i;}
    }
    for(size_t i=0;i<count;i++)for(size_t j=0;j<count;j++){
        cost_frontier_result_t *a=&r[order[i]],*b=&r[order[j]];
        if(b->cost_per_verified<=a->cost_per_verified&&b->seconds_per_verified<=a->seconds_per_verified&&b->success_lower_bound>=a->success_lower_bound&&(b->cost_per_verified<a->cost_per_verified||b->seconds_per_verified<a->seconds_per_verified||b->success_lower_bound>a->success_lower_bound))a->pareto=false;
    }
    for(size_t i=1;i<count;i++){size_t value=order[i],j=i;while(j&&better(&r[value],&r[order[j-1]])){order[j]=order[j-1];j--;}order[j]=value;}
    return count;
}

#include "json_util.h"
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
/* Strict primitive readers reject null, booleans, numeric strings, fractions
 * for integer contracts, missing fields and nonfinite values. */
static double number(const char *json,const char *key){
    char *raw=json_get_raw(json,key);if(!raw)return NAN;
    char *end;errno=0;double v=strtod(raw,&end);
    bool ok=end!=raw&&!*end&&!errno&&isfinite(v)&&(*raw=='-'||(*raw>='0'&&*raw<='9'));
    free(raw);return ok?v:NAN;
}
static long long integer(const char *json,const char *key){
    char *raw=json_get_raw(json,key);if(!raw)return -1;
    char *end;errno=0;long long n=strtoll(raw,&end,10);
    bool ok=end!=raw&&!*end&&!errno&&n>=0&&*raw>='0'&&*raw<='9';free(raw);return ok?n:-1;
}
static bool exact_bool(const char *json,const char *key,bool expected){
    char *raw=json_get_raw(json,key);bool ok=raw&&!strcmp(raw,expected?"true":"false");free(raw);return ok;
}
static bool string_field(const char *json,const char *key,char *out,size_t cap){
    char *s=json_get_str(json,key);bool ok=s&&*s&&strlen(s)<cap;
    if(ok)memcpy(out,s,strlen(s)+1);free(s);return ok;
}
typedef struct {
    cost_frontier_selection_t lanes[COST_FRONTIER_MAX_LANES];
    size_t count;bool bad;
    time_t at;unsigned evidence_age;double max_latency;
} parse_t;
static void parse_lane(const char *json,void *ctx){
    parse_t *p=ctx;if(p->bad)return;
    if(*json!='{'||p->count==COST_FRONTIER_MAX_LANES){p->bad=true;return;}
    cost_frontier_selection_t *l=&p->lanes[p->count];
    if(!string_field(json,"provider",l->provider,sizeof(l->provider))||!string_field(json,"model",l->model,sizeof(l->model))||!string_field(json,"effort",l->effort,sizeof(l->effort))||!string_field(json,"source",l->source,sizeof(l->source))){p->bad=true;return;}
    const char *optional_keys[]={"upstream","quantization","auth_class"};
    char *optional_dest[]={l->upstream,l->quantization,l->auth_class};
    size_t optional_caps[]={sizeof(l->upstream),sizeof(l->quantization),sizeof(l->auth_class)};
    for(size_t i=0;i<3;i++){
        char *raw=json_get_raw(json,optional_keys[i]);char *value=json_get_str(json,optional_keys[i]);
        if(raw&&(!value||strlen(value)>=optional_caps[i]))p->bad=true;
        if(value&&strlen(value)<optional_caps[i])strcpy(optional_dest[i],value);
        free(raw);free(value);
    }
    if(!l->auth_class[0])strcpy(l->auth_class,"api");
    const char *efforts[]={"none","minimal","low","medium","high","xhigh","max"};
    bool valid_effort=false;for(size_t i=0;i<sizeof(efforts)/sizeof(*efforts);i++)if(!strcmp(l->effort,efforts[i]))valid_effort=true;
    const char *auths[]={"api","payg","subscription","oauth","local"};
    bool valid_auth=false;for(size_t i=0;i<sizeof(auths)/sizeof(*auths);i++)if(!strcmp(l->auth_class,auths[i]))valid_auth=true;
    if(!valid_effort||!valid_auth||strlen(l->provider)>=32)p->bad=true;
    if(p->bad)return;
    for(size_t i=0;i<p->count;i++)if(!strcmp(l->provider,p->lanes[i].provider)&&!strcmp(l->model,p->lanes[i].model)&&!strcmp(l->effort,p->lanes[i].effort)&&!strcmp(l->upstream,p->lanes[i].upstream)&&!strcmp(l->quantization,p->lanes[i].quantization)&&!strcmp(l->auth_class,p->lanes[i].auth_class)){p->bad=true;return;}
    cost_frontier_candidate_t *c=&l->quote;c->provider=l->provider;c->model=l->model;c->source=l->source;
    c->observed_at=(time_t)integer(json,"quote_observed_at");c->route_specific=exact_bool(json,"route_specific",true);c->tools=exact_bool(json,"tools",true);
    c->context_tokens=integer(json,"context_tokens");c->max_output_tokens=integer(json,"max_output_tokens");c->cached_tokens=integer(json,"cached_tokens");
    c->input_per_million=number(json,"input_per_million");c->cached_per_million=number(json,"cached_per_million");c->output_per_million=number(json,"output_per_million");c->request_fee=number(json,"request_fee");
    long long attempts=integer(json,"attempts"),verified=integer(json,"verified"),evidence=integer(json,"evidence_observed_at");
    c->mean_latency_seconds=number(json,"mean_latency_seconds");
    bool qualified=exact_bool(json,"strict_validation",true)&&integer(json,"unpriced_attempts")==0&&attempts>0&&attempts<=UINT_MAX&&verified>=0&&verified<=attempts&&evidence>0&&evidence<=p->at&&difftime(p->at,(time_t)evidence)<=p->evidence_age&&c->mean_latency_seconds<=p->max_latency;
    c->attempts=qualified?(unsigned)attempts:0;c->verified=qualified?(unsigned)verified:0;
    p->count++;
}
bool cost_frontier_select_json(const char *input,time_t at,cost_frontier_selection_t *selected,const char **error){
    const char *failure="invalid_frontier_policy";bool ok=false;char *policy=NULL;
    if(selected)memset(selected,0,sizeof(*selected));
    parse_t *parsed=NULL;
    if(!selected||!input||strlen(input)>256*1024||!json_is_valid_container(input))goto done;
    policy=json_get_raw(input,"frontier_policy");if(!policy||*policy!='{')goto done;
    char schema[64],workload[128],validator[64],top_workload[128],top_validator[64];
    if(!string_field(policy,"schema",schema,sizeof(schema))||strcmp(schema,"dsco.cost_frontier_policy.v1")||!string_field(policy,"workload",workload,sizeof(workload))||!string_field(policy,"validator",validator,sizeof(validator))||!string_field(input,"workload",top_workload,sizeof(top_workload))||!string_field(input,"validator",top_validator,sizeof(top_validator))||strcmp(workload,top_workload)||strcmp(validator,top_validator)||(strcmp(validator,"integer-json-v1")&&strcmp(validator,"standalone-json-v1")))goto done;
    long long expires=integer(policy,"expires_at"),quote_age=integer(policy,"max_quote_age_seconds"),evidence_age=integer(policy,"max_evidence_age_seconds"),min_attempts=integer(policy,"min_attempts");
    if(expires<=at){failure="expired_frontier_policy";goto done;}
    if(quote_age<=0||quote_age>86400||evidence_age<=0||evidence_age>86400||min_attempts<=0||min_attempts>UINT_MAX)goto done;
    cost_frontier_scenario_t scenario={.enabled=true,.at=at,.prompt_tokens=integer(policy,"prompt_tokens"),.output_tokens=integer(policy,"output_tokens"),.require_tools=exact_bool(policy,"require_tools",true),.max_quote_age_seconds=(unsigned)quote_age,.min_attempts=(unsigned)min_attempts,.minimum_success_lower_bound=number(policy,"minimum_success_lower_bound"),.max_latency_seconds=number(policy,"max_latency_seconds")};
    if(!exact_bool(policy,"require_tools",true)&&!exact_bool(policy,"require_tools",false))goto done;
    parsed=calloc(1,sizeof(*parsed));if(!parsed)goto done;parsed->at=at;parsed->evidence_age=(unsigned)evidence_age;parsed->max_latency=number(policy,"max_latency_seconds");
    if(!isfinite(parsed->max_latency)||parsed->max_latency<=0)goto done;
    json_array_foreach(policy,"lanes",parse_lane,parsed);if(parsed->bad||!parsed->count)goto done;
    cost_frontier_candidate_t candidates[COST_FRONTIER_MAX_LANES];cost_frontier_result_t results[COST_FRONTIER_MAX_LANES];size_t order[COST_FRONTIER_MAX_LANES];
    char *explicit_provider=json_get_str(input,"provider"),*explicit_model=json_get_str(input,"model"),*explicit_effort=json_get_str(input,"effort");
    char *explicit_upstream=json_get_str(input,"upstream"),*explicit_quantization=json_get_str(input,"quantization"),*explicit_auth=json_get_str(input,"auth_class");
    for(size_t i=0;i<parsed->count;i++){
        candidates[i]=parsed->lanes[i].quote;
        if((explicit_provider&&strcmp(explicit_provider,candidates[i].provider))||(explicit_model&&strcmp(explicit_model,candidates[i].model))||(explicit_effort&&strcmp(explicit_effort,parsed->lanes[i].effort))||(explicit_upstream&&strcmp(explicit_upstream,parsed->lanes[i].upstream))||(explicit_quantization&&strcmp(explicit_quantization,parsed->lanes[i].quantization))||(explicit_auth&&strcmp(explicit_auth,parsed->lanes[i].auth_class)))candidates[i].attempts=0;
    }
    free(explicit_provider);free(explicit_model);free(explicit_effort);free(explicit_upstream);free(explicit_quantization);free(explicit_auth);
    size_t eligible=cost_frontier_rank(candidates,parsed->count,&scenario,results,order);
    jbuf_t audit;jbuf_init(&audit,4096);jbuf_append(&audit,"[");
    for(size_t i=0;i<parsed->count;i++) {
        if(i)jbuf_append(&audit,",");
        jbuf_append(&audit,"{\"provider\":");jbuf_append_json_str(&audit,candidates[i].provider);
        jbuf_append(&audit,",\"model\":");jbuf_append_json_str(&audit,candidates[i].model);
        jbuf_append(&audit,",\"effort\":");jbuf_append_json_str(&audit,parsed->lanes[i].effort);
        jbuf_append(&audit,",\"upstream\":");jbuf_append_json_str(&audit,parsed->lanes[i].upstream);
        jbuf_append(&audit,",\"quantization\":");jbuf_append_json_str(&audit,parsed->lanes[i].quantization);
        jbuf_append(&audit,",\"auth_class\":");jbuf_append_json_str(&audit,parsed->lanes[i].auth_class);
        jbuf_append(&audit,",\"reason\":");jbuf_append_json_str(&audit,results[i].reason);
        jbuf_appendf(&audit,",\"eligible\":%s,\"pareto\":%s}",results[i].eligible?"true":"false",results[i].pareto?"true":"false");
    }
    jbuf_append(&audit,"]");
    if(audit.len>=sizeof(selected->audit_json)){jbuf_free(&audit);goto done;}
    memcpy(selected->audit_json,audit.data,audit.len+1);jbuf_free(&audit);
    if(!eligible){failure="no_eligible_cost_frontier_lane";goto done;}
    memcpy(parsed->lanes[order[0]].audit_json,selected->audit_json,strlen(selected->audit_json)+1);
    *selected=parsed->lanes[order[0]];selected->quote.provider=selected->provider;selected->quote.model=selected->model;selected->quote.source=selected->source;selected->result=results[order[0]];selected->expires_at=(time_t)expires;ok=true;
done:
    if(error)*error=ok?NULL:failure;free(policy);free(parsed);return ok;
}
