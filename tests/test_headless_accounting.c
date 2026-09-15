#include "headless_accounting.h"
#include "inference_cost.h"
#include "deepseek_pricing.h"
#include "config.h"
#include "chronicle.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
static bool direct_available=true;
int deepseek_pricing_lookup(const char *model,time_t at,model_price_t *out,const char **source,time_t *observed){
 (void)at;if(!direct_available||strcmp(model,"deepseek-v4-flash"))return 0;
 *out=(model_price_t){.input=.22,.output=.66,.cached_input=.007,.cache_write=-1};
 *source="deepseek_first_party_off_peak";*observed=123;return 1;
}
static bool journal_ok=true;
static char record[8192];
const char *chronicle_run_id(void){return "test-run";}
bool chronicle_journal_append(const char *t,const char *j,bool d){(void)t;(void)d;snprintf(record,sizeof(record),"%s",j);return journal_ok;}
bool chronicle_event(const char *a,const char *b,const char *c,const char *d,const char *e,const char *f,const char *g,const char *h){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return true;}
const char *provider_auth_mode(const char *p,const char *k){(void)p;(void)k;return "fixture";}
static bool included;static int events;static double last_cost;
static model_info_t priced={.model_id="known",.input_price=1,.output_price=2,.cache_read_price=.1,.cache_write_price=1.25};
const model_info_t *openrouter_cache_lookup(const char *n){return !strcmp(n,"known") ? &priced : NULL;}
const model_info_t *codex_cache_lookup(const char *n){(void)n;return NULL;}
int model_pricing_lookup(const char *provider,const char *model,model_price_t *out){(void)provider;(void)model;(void)out;return false;}
bool provider_usage_is_included(const char *p,const char *k){(void)p;(void)k;return included;}
bool chronicle_llm_response(const char *a,const char *b,const char *c,const char *d,
 const char *e,const char *f,int g,int h,int i,int j,int k,double cost,double m,const char *n,const char *o){
 (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)m;(void)n;(void)o;events++;last_cost=cost;return true;}
void session_state_init(session_state_t *s,const char *m){memset(s,0,sizeof(*s));snprintf(s->model,sizeof(s->model),"%s",m);}
int main(void){
 session_state_t s={0};strcpy(s.model,"known");
 stream_result_t r={.ok=true,.cost_usd=.02};r.usage.input_tokens=1000;r.usage.output_tokens=100;
 assert(headless_account_response(&s,"openrouter","",&r));assert(fabs(s.total_reported_cost_usd-.02)<1e-9);
 r.cost_usd=0;r.usage.cache_read_input_tokens=100;r.usage.cache_creation_input_tokens=10;
 assert(headless_account_response(&s,"openrouter","",&r));assert(fabs(last_cost-.0012225)<1e-9);
 included=true;assert(headless_account_response(&s,"included","",&r));assert(fabs(last_cost-.0012225)<1e-9 && s.turn_count==3);
 assert(strstr(record,"\"subscription_included\":true"));
 assert(strstr(record,"\"token_basis\":\"input_excludes_cache\""));
 r.cost_usd=.03;r.cost_reported=true;assert(headless_account_response(&s,"included","",&r));assert(last_cost==.03);
 assert(strstr(record,"\"provider_reported_usd\":0.030000000000"));
 assert(strstr(record,"\"estimated_inference_usd\":0.001222500000"));
 r.cost_usd=0;assert(headless_account_response(&s,"included","",&r));assert(fabs(last_cost-.0012225)<1e-9);
 assert(strstr(record,"\"provider_reported_usd\":0.000000000000"));
 assert(s.subscription_response_count==3);r.cost_reported=false;
 included=false;r.ok=false;r.cost_usd=.003;assert(headless_account_response(&s,"openrouter","",&r));assert(last_cost==.003);
 r.ok=true;r.cost_usd=0;r.cost_reported=true;assert(headless_account_response(&s,"openrouter","",&r) && last_cost==0);
 int before=events;memset(&r,0,sizeof(r));assert(headless_account_response(&s,"openrouter","",&r));assert(events==before);
 int unpriced_before=s.unpriced_response_count;double known_before=s.total_reported_cost_usd;
 r.ok=true;strcpy(s.model,"unknown");assert(headless_account_response(&s,"openrouter","",&r));assert(events==before);
 assert(r.ok && s.unpriced_response_count==unpriced_before+1 && s.total_reported_cost_usd==known_before);
 assert(strstr(record,"\"success\":true") && strstr(record,"\"budget_accounted_usd\":null"));
 r.cost_usd=NAN;assert(headless_account_response(&s,"openrouter","",&r));
 assert(s.unpriced_response_count==unpriced_before+2 && s.total_reported_cost_usd==known_before);
 assert(strstr(record,"\"estimated_inference_usd\":null"));
 journal_ok=false;char path[]="/tmp/dsco-cost-test-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);setenv("DSCO_COST_LEDGER_PATH",path,1);
 r.ok=false;assert(headless_account_response(&s,"fixture","",&r));FILE *f=fopen(path,"r");assert(f && fgets(record,sizeof(record),f));fclose(f);unlink(path);
 assert(strstr(record,"dsco.inference_cost.v1"));
 setenv("DSCO_COST_LEDGER_PATH","/nonexistent/dsco/cost.jsonl",1);assert(!headless_account_response(&s,"fixture","",&r));
 double saved=s.total_reported_cost_usd;int turns=s.turn_count;
 headless_session_retarget(&s,"known");assert(s.total_reported_cost_usd==saved && s.turn_count==turns && !strcmp(s.model,"known"));
 inference_cost_t measured;
 stream_result_t rates={0};rates.usage.input_tokens=1;rates.usage.output_tokens=100;
 priced.input_price=-1;inference_cost_measure("known",&rates,false,&measured);
 assert(!measured.estimated_known); /* positive output must not offset an unknown input rate */
 rates.usage.input_tokens=0;inference_cost_measure("known",&rates,false,&measured);
 assert(measured.estimated_known && measured.estimated_usd==0.0002);
 priced.input_price=NAN;inference_cost_measure("known",&rates,false,&measured);
 assert(measured.estimated_known); /* an unused unavailable rate is harmless */
 rates.usage.input_tokens=1;inference_cost_measure("known",&rates,false,&measured);
 assert(!measured.estimated_known);
 stream_result_t k3_usage={.ok=true,.actual_model="k3"};
 k3_usage.usage.input_tokens=1000;k3_usage.usage.output_tokens=100;
 inference_cost_measure("kimi-code/k3",&k3_usage,true,&measured);
 assert(!measured.estimated_known && !measured.budget_known);
 k3_usage.cost_reported=true; /* A reported zero bill is retained, not a zero inference value. */
 inference_cost_measure("kimi-code/k3",&k3_usage,true,&measured);
 assert(measured.provider_reported_known && measured.provider_reported_usd==0);
 assert(!measured.estimated_known && !measured.budget_known);
 k3_usage.cost_usd=.01;inference_cost_measure("kimi-code/k3",&k3_usage,true,&measured);
 assert(measured.budget_known && measured.budget_usd==.01);
 stream_result_t ds={.actual_model="deepseek-v4-flash"};ds.usage.input_tokens=1000;ds.usage.output_tokens=100;ds.usage.cache_read_input_tokens=1000;
 inference_cost_measure_for_provider("deepseek","deepseek/deepseek-v4-flash",&ds,false,&measured);
 assert(measured.estimated_known && fabs(measured.estimated_usd-.000293)<1e-12);
 assert(!strcmp(measured.pricing_scope,"route_specific") && measured.pricing_observed_at==123);
 ds.cost_reported=true;ds.cost_usd=.01;
 inference_cost_measure_for_provider("deepseek","deepseek-v4-flash",&ds,false,&measured);
 assert(measured.budget_usd==.01 && measured.estimated_usd<.01);
 inference_cost_measure_for_provider("openrouter","deepseek-v4-flash",&ds,false,&measured);
 assert(strcmp(measured.pricing_scope,"route_specific"));
 direct_available=false;
 inference_cost_measure_for_provider("deepseek","deepseek-v4-flash",&ds,false,&measured);
 assert(!strcmp(measured.pricing_scope,"reference_fallback_direct_quote_unavailable"));
 /* Metadata-only observed Responses receipt, at its recorded rates. Cached
  * input belongs only to the discounted category, not to both categories. */
 priced.input_price=10;priced.output_price=50;priced.cache_read_price=1;
 stream_result_t cached_response={.ok=true};
 cached_response.usage.input_tokens=1514;cached_response.usage.output_tokens=153;
 cached_response.usage.cache_read_input_tokens=17280;
 inference_cost_measure("known",&cached_response,true,&measured);
 assert(measured.estimated_known && fabs(measured.estimated_usd-.040070)<1e-12);
 puts("PASS: reported cost, token/cache fallback, subscriptions, partial failures, missing/nonfinite cost");
}
