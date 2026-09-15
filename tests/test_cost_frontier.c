#include "cost_frontier.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "json_util.h"
static char *read_fixture(const char *path) {
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char *p=malloc(n+1);assert(p);assert(fread(p,1,n,f)==(size_t)n);p[n]=0;fclose(f);return p;
}
static void rejected_variant(const char *fixture,const char *old,const char *replacement){
 const char *at=strstr(fixture,old);assert(at);char *buf=malloc(strlen(fixture)+strlen(replacement)+1);assert(buf);
 size_t before=(size_t)(at-fixture);memcpy(buf,fixture,before);strcpy(buf+before,replacement);strcpy(buf+before+strlen(replacement),at+strlen(old));
 cost_frontier_selection_t out;const char *error;assert(!cost_frontier_select_json(buf,2000,&out,&error));assert(error);free(buf);
}
int main(int argc,char **argv){
 if(argc>1){char *input=read_fixture(argv[1]);cost_frontier_selection_t selected;const char *error;bool ok=cost_frontier_select_json(input,time(NULL),&selected,&error);printf("selected=%s/%s effort=%s cost=%.12f error=%s audit=%s\n",ok?selected.provider:"",ok?selected.model:"",ok?selected.effort:"",ok?selected.result.cost_per_call:0,error?error:"none",selected.audit_json);free(input);return ok?0:1;}
 char *fixture=read_fixture("tests/fixtures/cost_frontier_policy.json");cost_frontier_selection_t chosen;const char *error;
 assert(cost_frontier_select_json(fixture,2000,&chosen,&error));assert(!strcmp(chosen.provider,"fixture-provider")&&!strcmp(chosen.effort,"none"));assert(strstr(chosen.audit_json,"eligible"));
 rejected_variant(fixture,"\"effort\":\"none\"","\"effort\":\"typo\"");
 rejected_variant(fixture,"\"source\":","\"auth_class\":\"typo\",\"source\":");
 rejected_variant(fixture,"\"selection\":\"cost_frontier\"","\"selection\":\"cost_frontier\",\"upstream\":\"different\"");
 rejected_variant(fixture,"\"expires_at\":2100","\"expires_at\":2000");
 rejected_variant(fixture,"\"evidence_observed_at\":1990","\"evidence_observed_at\":1800");
 rejected_variant(fixture,"\"strict_validation\":true","\"strict_validation\":false");
 rejected_variant(fixture,"\"unpriced_attempts\":0","\"unpriced_attempts\":1");
 rejected_variant(fixture,"\"mean_latency_seconds\":1","\"mean_latency_seconds\":11");
 rejected_variant(fixture,"\"verified\":3","\"verified\":2");
 rejected_variant(fixture,"\"attempts\":3","\"attempts\":3.5");
 rejected_variant(fixture,"\"workload\":\"fixture\"","\"workload\":\"different\"");
 rejected_variant(fixture,"\"selection\":\"cost_frontier\"","\"selection\":\"cost_frontier\",\"provider\":\"other\"");
 rejected_variant(fixture,"\"request_fee\":0","\"request_fee\":null");
 char *policy=json_get_raw(fixture,"frontier_policy");char *array=json_get_raw(policy,"lanes");
 char *lane=malloc(strlen(array));assert(lane);strcpy(lane,array+1);lane[strlen(lane)-1]=0;
 char *two=malloc(strlen(fixture)+strlen(lane)*2+200);assert(two);
 char *start=strstr(fixture,"\"lanes\":[");assert(start);size_t prefix=(size_t)(start-fixture);
 memcpy(two,fixture,prefix);sprintf(two+prefix,"\"lanes\":[%s,%s]}}",lane,lane);
 assert(!cost_frontier_select_json(two,2000,&chosen,&error)); /* identical full identity rejected */
 char *second=malloc(strlen(lane)+64);assert(second);sprintf(second,"{\"upstream\":\"host-b\",%s",lane+1);
 memcpy(two,fixture,prefix);sprintf(two+prefix,"\"lanes\":[%s,%s]}}",second,lane);
 assert(cost_frontier_select_json(two,2000,&chosen,&error));assert(!strcmp(chosen.upstream,"host-b"));assert(strstr(chosen.audit_json,"host-b"));
 free(second);free(two);free(lane);free(array);free(policy);
 free(fixture);

 assert(!cost_frontier_auth_compatible("anthropic","api",false,true,false));
 assert(!cost_frontier_auth_compatible("anthropic","subscription",false,true,true));
 assert(cost_frontier_auth_compatible("openai","api",false,true,false));
 assert(!cost_frontier_auth_compatible("openai","subscription",false,true,false));
 assert(cost_frontier_auth_compatible("openai-codex","subscription",false,false,true));
 assert(!cost_frontier_auth_compatible("openai-codex","api",false,false,true));
 assert(cost_frontier_auth_compatible("sakana","payg",false,true,true));
 assert(!cost_frontier_auth_compatible("sakana","api",false,true,false));
 assert(cost_frontier_auth_compatible("stepfun","subscription",false,true,true));
 assert(!cost_frontier_auth_compatible("stepfun","api",false,true,true));
 assert(cost_frontier_auth_compatible("local","local",true,false,false));
 cost_frontier_scenario_t s={.enabled=true,.prompt_tokens=1000,.output_tokens=100,.require_tools=true,.at=2000,.max_quote_age_seconds=100,.min_attempts=4,.minimum_success_lower_bound=.5,.max_latency_seconds=10};
 cost_frontier_candidate_t c[3];
 c[0]=(cost_frontier_candidate_t){.provider="route-a",.model="model-a",.source="fixture",.observed_at=1950,.route_specific=true,.tools=true,.context_tokens=2000,.max_output_tokens=200,.input_per_million=1,.cached_per_million=.1,.output_per_million=2,.request_fee=.001,.cached_tokens=500,.attempts=10,.verified=10,.mean_latency_seconds=2};
 c[1]=c[0];c[1].provider="route-b";c[1].input_per_million=2;c[1].mean_latency_seconds=3;
 c[2]=c[0];c[2].provider="route-c";c[2].input_per_million=3;c[2].mean_latency_seconds=1;
 cost_frontier_result_t r[3];size_t order[3]={99,99,99};
 assert(cost_frontier_rank(c,3,&s,r,order)==3);assert(fabs(r[0].cost_per_call-.00175)<1e-12);assert(r[0].pareto&&!r[1].pareto&&r[2].pareto);assert(order[0]==0&&order[1]==2&&order[2]==1);
 assert(r[0].success_lower_bound>.7&&r[0].success_lower_bound<.8);
 c[1].mean_latency_seconds=11;assert(cost_frontier_rank(c,3,&s,r,order)==2);assert(!strcmp(r[1].reason,"latency_ceiling_exceeded"));c[1].mean_latency_seconds=3;
 c[1].cached_per_million=-1;assert(cost_frontier_rank(c,3,&s,r,order)==2);assert(!strcmp(r[1].reason,"price_unavailable"));
 c[1].cached_tokens=0;assert(cost_frontier_rank(c,3,&s,r,order)==3);
 c[1].route_specific=false;assert(cost_frontier_rank(c,3,&s,r,order)==2);
 c[1]=c[0];c[1].request_fee=NAN;assert(cost_frontier_rank(c,3,&s,r,order)==2);
 c[1]=c[0];c[1].observed_at=1800;assert(cost_frontier_rank(c,3,&s,r,order)==2);
 c[1]=c[0];c[1].verified=0;assert(cost_frontier_rank(c,3,&s,r,order)==2);
 c[1]=c[0];c[1].input_per_million=c[1].cached_per_million=c[1].output_per_million=c[1].request_fee=0;
 assert(cost_frontier_rank(c,3,&s,r,order)==3);assert(r[1].billed_free&&r[1].cost_per_verified==0&&isfinite(r[1].seconds_per_verified));
 c[1].context_tokens=1099;assert(cost_frontier_rank(c,3,&s,r,order)==2);
 c[1]=c[0];c[1].cached_tokens=1001;assert(cost_frontier_rank(c,3,&s,r,order)==2);
 c[1]=c[0];c[1].tools=false;assert(cost_frontier_rank(c,3,&s,r,order)==2);
 s.enabled=false;order[0]=99;assert(cost_frontier_rank(c,3,&s,r,order)==0&&order[0]==99&&!strcmp(r[0].reason,"disabled"));
 puts("PASS: opt-in route quotes, cache scope, evidence, Pareto, free billing, unknown prices, capabilities");
}
