#!/usr/bin/env python3
"""Exercise actual plan engine scheduling without provider or tool calls."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include "plan.h"
bool tools_execute(const char *name, const char *input, char *out, size_t len) {
    (void)name; (void)input; (void)out; (void)len;
    assert(!"fixture must never execute external tools"); return false;
}
static int new_plan(void) { plan_engine_init(); return plan_create("test", "test", PLAN_MODE_HYBRID); }
int main(void) {
    int p = new_plan(), s = plan_add_step(p, 0, "long leaf", STEP_ATOMIC);
    for (int i=0;i<33;i++) assert(step_add_atom(s,"noop",ATOM_NOOP)>0);
    assert(plan_run_all(p,0)==33 && plan_get(p)->status==PLAN_DONE);
    p = new_plan(); s = plan_add_step(p,0,"wired leaf",STEP_ATOMIC);
    int a=step_add_atom(s,"first",ATOM_NOOP), b=step_add_atom(s,"second",ATOM_NOOP);
    assert(atom_wire(a,b,NULL));
    assert(plan_run_all(p,1)==1 && atom_get(b)->status==PLAN_PENDING);
    assert(plan_run_all(p,0)==1 && plan_get(p)->status==PLAN_DONE);
    for (int state=0; state<2; state++) {
        p=new_plan(); int parent=plan_add_step(p,0,"gate",STEP_GATE);
        s=plan_add_step(p,parent,"child",STEP_ATOMIC); a=step_add_atom(s,"work",ATOM_NOOP);
        assert(step_set_status(parent,state ? PLAN_BLOCKED : PLAN_CANCELLED));
        assert(!step_can_run(s) && plan_run_all(p,0)==0);
        assert(atom_get(a)->status==PLAN_PENDING);
        assert(plan_run_all(p,0)==0); /* rollup must not erase explicit gate */
        if (state) {
            assert(step_set_status(parent,PLAN_PENDING)); plan_rollup_status(p);
            assert(plan_run_all(p,0)==1);
        }
    }
    p=new_plan(); int dependency=plan_add_step(p,0,"dependency",STEP_ATOMIC);
    int dep_atom=step_add_atom(dependency,"dependency work",ATOM_NOOP);
    int parent=plan_add_step(p,0,"gated parent",STEP_COMPOSITE);
    assert(step_add_dep(parent,dependency));
    s=plan_add_step(p,parent,"child",STEP_ATOMIC); a=step_add_atom(s,"work",ATOM_NOOP);
    assert(step_set_priority(s,100)); int next;
    assert(plan_ready_atoms(p,&next,1)==1 && next==dep_atom);
    assert(plan_run_all(p,0)==2 && atom_get(a)->status==PLAN_DONE);
    p=new_plan(); int low=plan_add_step(p,0,"low",STEP_ATOMIC);
    int high=plan_add_step(p,0,"high",STEP_ATOMIC);
    a=step_add_atom(low,"low",ATOM_NOOP); b=step_add_atom(high,"high",ATOM_NOOP);
    assert(step_set_priority(high,10));
    assert(plan_ready_atoms(p,&next,1)==1 && next==b);
    assert(step_set_priority(low,20));
    assert(plan_ready_atoms(p,&next,1)==1 && next==a);
    assert(plan_run_all(p,1)==1 && atom_get(a)->status==PLAN_DONE && atom_get(b)->status==PLAN_PENDING);
    assert(plan_run_all(p,0)==1);
    puts("PASS: large leaves, wired progression, ancestor status/dependency gates, priority changes");
    plan_engine_init();
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-plan-frontier-') as directory:
    path=Path(directory); (path/'frontier.c').write_text(unit)
    subprocess.run(shlex.split(os.environ.get('CC','cc')) + [
        '-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L',
        '-I',str(ROOT/'include'),str(path/'frontier.c'),str(ROOT/'src/plan.c'),
        str(ROOT/'src/json_util.c'),'-lm','-o',str(path/'frontier')],check=True)
    subprocess.run([str(path/'frontier')],check=True)
