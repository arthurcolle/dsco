#!/usr/bin/env python3
"""Compile actual fabric admission/instance snippets with deterministic stubs.
No credentials, model calls, windows, or provider-account changes.
"""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/tools.c').read_text()
def function(name):
 start=s.index('static ',s.rfind('\n',0,s.index(name+'(')))
 end=s.index('\n}',start)+2
 return s[start:end]
f=s[s.index('static bool tool_provider_fabric('):]
parse=f[f.index('    char *fabric_budget_raw'):f.index('    int depth =')]
coverage=f[f.index('    if (coverage) {'):f.index('    bool diverse_hedge')]
unit=r'''
#include "json_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
typedef struct { int unused; } provider_fabric_lane_t;
static int added,subscriptions,metered_count,local_count,observed_turns,observed_tokens;
static double observed_budget,observed_temp;
static char observed_effort[32],observed_tool[32],observed_system[128];
static bool provider_is_local_endpoint(const char *p){return !strcmp(p,"local");}
static const char *provider_resolve_request_api_key(const char *p,const char *k){(void)p;(void)k;return "dummy";}
static bool provider_usage_is_included(const char *p,const char *k){(void)k;return !strcmp(p,"subscription");}
static bool fabric_add_lane(provider_fabric_lane_t *l,int *n,int max,const char *p,const char *m,int r,bool sub,bool met,const char *key){
 (void)l;(void)max;(void)m;(void)r;(void)key;(*n)++;added++;subscriptions+=sub;metered_count+=met;local_count+=provider_is_local_endpoint(p);return true;
}
static void swarm_set_next_instance(const char *eff,double t,double p,int k,int b,const char *tc,const char *sp,int turns){
 (void)p;(void)k;(void)b;observed_temp=t;observed_turns=turns;
 snprintf(observed_effort,sizeof(observed_effort),"%s",eff?eff:"");
 snprintf(observed_tool,sizeof(observed_tool),"%s",tc?tc:"");
 snprintf(observed_system,sizeof(observed_system),"%s",sp?sp:"");
}
static void swarm_set_next_budget_usd(double b){observed_budget=b;}
static void swarm_set_next_max_tokens(int n){observed_tokens=n;}
static int fabric_reorder_for_coverage(provider_fabric_lane_t *l,int n){(void)l;return n;}
'''+function('fabric_add_env_sublanes')+'\n'+function('fabric_apply_worker_instance')+r'''
static bool parse_budget(const char *input,double *out){char result[256];size_t rlen=sizeof(result);
'''+parse+r'''
*out=fabric_budget;return true;}
static int coverage_cap(int cap){bool coverage=true;provider_fabric_lane_t lanes[10];int lane_count=10;int max_agents=cap;
'''+coverage+r'''
return max_agents;}
int main(void){
 provider_fabric_lane_t lanes[8];int n=0;
 setenv("DSCO_FABRIC_SUBLANES","subscription:a@auto,api:b,local:c",1);
 fabric_add_env_sublanes(lanes,&n,8,1,false,NULL);
 assert(added==2&&subscriptions==1&&metered_count==0&&local_count==1);
 added=subscriptions=metered_count=local_count=0;n=0;
 fabric_add_env_sublanes(lanes,&n,8,1,true,NULL);
 assert(added==3&&subscriptions==1&&metered_count==1&&local_count==1);
 assert(coverage_cap(3)==3);assert(coverage_cap(1)==1);
 double b=0;assert(parse_budget("{\"budget\":1.5}",&b)&&b==1.5);
 assert(parse_budget("{\"budget_usd\":0.9}",&b)&&b==.9);
 unsetenv("DSCO_FABRIC_BUDGET_USD");assert(parse_budget("{}",&b)&&b==0);
 setenv("DSCO_FABRIC_BUDGET_USD","2",1);assert(parse_budget("{}",&b)&&b==2);
 assert(parse_budget("{\"budget\":0.3}",&b)&&b==.3);
 unsetenv("DSCO_FABRIC_BUDGET_USD");
 const char *bad[]={"{\"budget\":-1}","{\"budget\":\"2\"}","{\"budget\":null}","{\"budget\":true}","{\"budget\":1e999}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(!parse_budget(bad[i],&b));
 double cap=fabric_apply_worker_instance("{\"effort\":\"high\",\"temperature\":0.2,\"max_worker_turns\":3,\"max_tokens\":128,\"tool_choice\":\"none\",\"system_prompt\":\"probe\"}","auto",1.5,3);
 assert(fabs(cap-.5)<1e-12&&observed_budget==cap&&observed_turns==3&&observed_tokens==128);
 assert(!strcmp(observed_effort,"auto")&&!strcmp(observed_tool,"none")&&!strcmp(observed_system,"probe"));
 assert(fabs(observed_temp-.2)<1e-12);
 fabric_apply_worker_instance("{\"effort\":\"low\"}","",0,1);
 assert(!strcmp(observed_effort,"low")&&observed_turns==12&&observed_budget==0&&observed_tokens==0);
 puts("PASS: API/subscription/local classification; explicit metered opt-out; coverage cap; strict budget validation; pre-fork child budget/turn/token/options forwarding");
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-fabric-limits-') as d:
 p=Path(d);(p/'test.c').write_text(unit)
 subprocess.run(['cc','-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-Iinclude',str(p/'test.c'),'src/json_util.c','-lm','-o',str(p/'test')],cwd=ROOT,check=True)
 subprocess.run([str(p/'test')],check=True)
