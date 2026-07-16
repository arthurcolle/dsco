/* strategy.c — strategic objective, option, bet, and pivot assessment. */
#include "strategy.h"
#include "json_util.h"
#include "tools.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static strategy_hooks_t g_hooks;
static strat_objective_t g_obj[STRAT_MAX_OBJECTIVES];
static int g_count;

void strategy_set_hooks(const strategy_hooks_t *h) { if (h) g_hooks=*h; else memset(&g_hooks,0,sizeof(g_hooks)); }
void strategy_reset(void) { memset(g_obj,0,sizeof(g_obj)); g_count=0; }
int strategy_objective_count(void) { return g_count; }
const strat_objective_t *strategy_objective_at(int i) { return i>=0&&i<g_count ? &g_obj[i] : NULL; }

double strategy_kelly_fraction(double p, double b) {
    if (p<=0 || p>=1 || b<=0) return 0;
    double f=(b*p-(1-p))/b/2.0;
    if (f<0) f=0; if (f>STRAT_BET_FRACTION_MAX) f=STRAT_BET_FRACTION_MAX;
    return f;
}
static void audit(const char *t,const char *d) { if(g_hooks.audit) g_hooks.audit("strategy",t,d); }
static int find_obj(const char *n) { for(int i=0;i<g_count;i++) if(!strcmp(g_obj[i].name,n)) return i; return -1; }
static bool reason_ok(const char *s) { return s && strlen(s)>=12; }

bool strategy_assess(const char *in,char *out,size_t n) {
    if(!in||!out||!n) return false;
    char *a=json_get_str(in,"action");
    if(!a){ snprintf(out,n,"{\"status\":\"rejected\",\"error\":\"action required\"}"); return false; }
    bool ok=false;
    if(!strcmp(a,"set_objective")) {
        char *name=json_get_str(in,"name"), *acc=json_get_str(in,"acceptance");
        int pri=json_get_int(in,"priority",3); double bud=json_get_double(in,"budget_usd",0);
        if(!name||!*name||!reason_ok(acc)||pri<1||pri>5||bud<0) snprintf(out,n,"{\"status\":\"rejected\",\"error\":\"name, substantive acceptance criteria, priority 1..5, and nonnegative budget required\"}");
        else { int i=find_obj(name); if(i<0&&g_count<STRAT_MAX_OBJECTIVES)i=g_count++; if(i<0) snprintf(out,n,"{\"status\":\"rejected\",\"error\":\"objective capacity reached\"}"); else { snprintf(g_obj[i].name,sizeof(g_obj[i].name),"%s",name); snprintf(g_obj[i].acceptance,sizeof(g_obj[i].acceptance),"%s",acc); g_obj[i].priority=pri; g_obj[i].budget_usd=bud; g_obj[i].status=STRAT_OBJ_ACTIVE; snprintf(out,n,"{\"status\":\"accepted\",\"objective\":\"%s\",\"priority\":%d}",g_obj[i].name,pri); audit("objective_set",g_obj[i].name); ok=true; } }
        free(name); free(acc);
    } else if(!strcmp(a,"list_objectives")) {
        size_t u=snprintf(out,n,"{\"status\":\"accepted\",\"objectives\":[");
        for(int i=0;i<g_count&&u<n;i++) u+=snprintf(out+u,n-u,"%s{\"name\":\"%s\",\"priority\":%d,\"status\":%d,\"progress\":%.3f,\"budget_usd\":%.2f,\"spent_usd\":%.2f}",i?",":"",g_obj[i].name,g_obj[i].priority,g_obj[i].status,g_obj[i].progress,g_obj[i].budget_usd,g_obj[i].spent_usd);
        if(u<n) snprintf(out+u,n-u,"]}"); ok=true;
    } else if(!strcmp(a,"complete_objective")||!strcmp(a,"abandon_objective")) {
        char *name=json_get_str(in,"name"),*why=json_get_str(in,"reason"); int i=name?find_obj(name):-1;
        bool abandon=!strcmp(a,"abandon_objective");
        if(i<0|| (abandon&&!reason_ok(why))) snprintf(out,n,"{\"status\":\"rejected\",\"error\":\"known objective and substantive abandonment reason required\"}");
        else { g_obj[i].status=abandon?STRAT_OBJ_ABANDONED:STRAT_OBJ_COMPLETE; if(!abandon)g_obj[i].progress=1; snprintf(g_obj[i].status_reason,sizeof(g_obj[i].status_reason),"%s",why?why:""); snprintf(out,n,"{\"status\":\"accepted\",\"objective\":\"%s\",\"terminal\":\"%s\"}",g_obj[i].name,abandon?"abandoned":"complete"); audit(a,g_obj[i].name); ok=true; } free(name);free(why);
    } else if(!strcmp(a,"size_bet")) {
        double p=json_get_double(in,"probability",-1), b=json_get_double(in,"payoff_ratio",-1), bank=json_get_double(in,"bankroll",-1); double f=strategy_kelly_fraction(p,b);
        if(p<=0||p>=1||b<=0||bank<0) snprintf(out,n,"{\"status\":\"rejected\",\"error\":\"probability (0,1), positive payoff_ratio and bankroll required\"}");
        else { snprintf(out,n,"{\"status\":\"accepted\",\"edge\":%.6f,\"half_kelly_fraction\":%.6f,\"allocation\":%.2f,\"cap\":%.2f}",p-(1.0/(b+1)),f,bank*f,STRAT_BET_FRACTION_MAX); ok=true; }
    } else if(!strcmp(a,"pivot_check")) {
        char *name=json_get_str(in,"name"); int i=name?find_obj(name):-1; double spent=json_get_double(in,"spent_usd",-1), progress=json_get_double(in,"progress",-1), fs=1; bool on=true; char sum[128]="not observed";
        if(g_hooks.frontier_snapshot) g_hooks.frontier_snapshot(&fs,&on,sum,sizeof(sum));
        if(i<0||spent<0||progress<0||progress>1) snprintf(out,n,"{\"status\":\"rejected\",\"error\":\"known objective, spent_usd >= 0, progress 0..1 required\"}");
        else { g_obj[i].spent_usd=spent;g_obj[i].progress=progress; double sf=g_obj[i].budget_usd>0?spent/g_obj[i].budget_usd:0; const char *v=(sf>=.75&&progress<.35)||(!on&&sf>=.5)?"pivot":(sf>=.5&&progress<.5)||fs<.7?"warn":"persevere"; snprintf(out,n,"{\"status\":\"accepted\",\"verdict\":\"%s\",\"spend_fraction\":%.4f,\"progress_fraction\":%.4f,\"frontier_score\":%.4f,\"on_frontier\":%s}",v,sf,progress,fs,on?"true":"false"); audit("pivot_check",v);ok=true;} free(name);
    } else snprintf(out,n,"{\"status\":\"rejected\",\"error\":\"unknown action\"}");
    free(a); return ok;
}
static char *adapter(const char *name,const char *in,void *ctx){(void)name;(void)ctx;char *r=calloc(1,8192);strategy_assess(in,r,8192);return r;}
void strategy_register_tool(void) {
 tools_register_external("strategic_assess","Strategic control: register durable objectives, evaluate progress and pivot timing, or size a bounded reversible bet. Actions: set_objective, list_objectives, complete_objective, abandon_objective, size_bet, pivot_check.",
 "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"set_objective\",\"list_objectives\",\"complete_objective\",\"abandon_objective\",\"size_bet\",\"pivot_check\"]},\"name\":{\"type\":\"string\"},\"acceptance\":{\"type\":\"string\"},\"priority\":{\"type\":\"integer\"},\"budget_usd\":{\"type\":\"number\"},\"spent_usd\":{\"type\":\"number\"},\"progress\":{\"type\":\"number\"},\"probability\":{\"type\":\"number\"},\"payoff_ratio\":{\"type\":\"number\"},\"bankroll\":{\"type\":\"number\"},\"reason\":{\"type\":\"string\"}},\"required\":[\"action\"]}",adapter,NULL);
}
