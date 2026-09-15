#!/usr/bin/env python3
"""Execute create's actual budget parser/spawn loop with deterministic stubs.
No provider, credentials, network, or model calls. Also checks child cap export.
"""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
s = (ROOT / 'src/tools.c').read_text()
f = s[s.index('static bool tool_create_swarm('):s.index('static bool tool_swarm_status(')]
parser = f[f.index('    char *budget_raw'):f.index('    /* Check hierarchical depth limit */')]
loop = f[f.index('    int spawned = 0;'):f.index('    char swarm_detail')]
unit = r'''
#include "swarm.h"
#include "json_util.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static swarm_t g_swarm;
static double next_cap, observed[8];
static int calls, fail_at;
static void swarm_apply_task_instance(void *spec,const char *input,int turns) {
 (void)spec;(void)input;(void)turns; next_cap=-1;
}
void swarm_set_next_budget_usd(double cap) { next_cap=cap; }
static int spawn(void) { int i=calls++; assert(next_cap>=0); observed[i]=next_cap; return i==fail_at ? -1:i; }
int swarm_spawn_provider(swarm_t *s,int gid,const char *task,const char *model,const char *provider) {
 (void)s;(void)gid;(void)task;(void)model;(void)provider; return spawn();
}
int swarm_spawn_in_group(swarm_t *s,int gid,const char *task,const char *model) {
 (void)s;(void)gid;(void)task;(void)model; return spawn();
}
swarm_child_t *swarm_get(swarm_t *s,int id) { return &s->children[id]; }
static void swarm_emit_child_event(const char *a,swarm_child_t *c,const char *b,void *d) {
 (void)a;(void)c;(void)b;(void)d;
}
static bool exercise(const char *input, int n) {
 char result[256]; size_t rlen=sizeof(result);
''' + parser + r'''
 struct {int count; struct {char *task,*model,*provider;} specs[8];} parse_ctx={0};
 parse_ctx.count=n;
 for(int i=0;i<n;i++) {parse_ctx.specs[i].task="task"; parse_ctx.specs[i].provider=i%2 ? NULL:"test";}
 char *model="test"; int gid=0;
''' + loop + r'''
 return spawned==n-(fail_at>=0 && fail_at<n);
}
int main(void) {
 memset(&g_swarm,0,sizeof(g_swarm));
 const char *invalid[]={"{\"budget\":-1}","{\"budget\":\"2\"}","{\"budget\":null}","{\"budget\":true}","{\"budget\":1e999}"};
 for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {calls=0; assert(!exercise(invalid[i],3)); assert(calls==0);}
 fail_at=-1;calls=0;assert(exercise("{\"budget\":1.8}",3));assert(calls==3);
 for(int i=0;i<3;i++) {assert(fabs(observed[i]-.6)<1e-12);assert(fabs(g_swarm.children[i].budget_usd-.6)<1e-12);}
 fail_at=1;calls=0;assert(exercise("{\"budget\":1.8}",3));assert(fabs(observed[2]-.6)<1e-12);
 fail_at=-1;calls=0;assert(exercise("{}",2));assert(observed[0]==0&&observed[1]==0);
 calls=0;assert(exercise("{\"budget\":0}",2));assert(observed[0]==0);
 unsetenv("DSCO_BUDGET");unsetenv("DSCO_CHILD_BUDGET");swarm_child_budget_export(.6);
 assert(fabs(strtod(getenv("DSCO_BUDGET"),NULL)-.6)<1e-12);
 setenv("DSCO_CHILD_BUDGET","0.2",1);swarm_child_budget_export(.6);
 assert(fabs(strtod(getenv("DSCO_BUDGET"),NULL)-.2)<1e-12);
 assert(fabs(strtod(getenv("DSCO_CHILD_BUDGET"),NULL)-.2)<1e-12);
 puts("PASS: create budget validation, pre-spawn partition, failed-task isolation, metadata, inherited ceiling");
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-create-budget-') as d:
    p=Path(d); (p/'probe.c').write_text(unit)
    subprocess.run(['cc','-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L','-Iinclude',str(p/'probe.c'),'src/swarm_accounting.c','src/json_util.c','-lm','-o',str(p/'probe')],cwd=ROOT,check=True)
    subprocess.run([str(p/'probe')],check=True)
