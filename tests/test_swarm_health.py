#!/usr/bin/env python3
"""Compile real health renderer; validate bounded snapshots without inference."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
unit=r'''
#include <assert.h>
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "swarm_telemetry.h"
bool swarm_active_test(const swarm_t *s,int id) {return (s->active.words[id/64]>>(id%64))&1;}
int main(void) {
 swarm_t *s=calloc(1,sizeof(*s)); assert(s); s->child_count=128;
 struct timeval tv;gettimeofday(&tv,NULL);double now=tv.tv_sec+tv.tv_usec/1e6;
 for(int i=0;i<128;i++) {
  swarm_child_t *c=&s->children[i];c->id=i;c->start_time=now-1000;
  c->status=i<16?SWARM_RUNNING:SWARM_DONE;
  if(i<16)s->active.words[0]|=1ULL<<i;
  snprintf(c->provider,sizeof(c->provider),"provider%d",i);
  snprintf(c->model,sizeof(c->model),"model\\\"%d",i);
 }
 s->children[0].ui_last_emit_time=now-500;
 s->children[0].reported_cost_known=true;s->children[0].reported_cost_usd=.25;
 s->children[0].budget_cost_known=true;s->children[0].budget_accounted_usd=.5;
 s->children[0].subsidized=true;
 s->children[1].estimated_cost_known=true;s->children[1].est_cost_usd=.1;
 s->children[2].reported_cost_known=true;s->children[2].reported_cost_usd=NAN;
 char out[131072];assert(swarm_health_json(s,"{}",out,sizeof(out)));puts(out);
 assert(swarm_health_json(s,"{\"worker_offset\":16,\"worker_limit\":2}",out,sizeof(out)));puts(out);
 assert(swarm_health_json(s,"{}",out,2400));puts(out);
 assert(!swarm_health_json(s,"{}",out,100));puts(out);
 assert(!swarm_health_json(s,"{\"stall_threshold_seconds\":-1}",out,sizeof(out)));puts(out);
 s->children[3].reported_cost_known=true;s->children[3].reported_cost_usd=DBL_MAX;
 s->children[4].reported_cost_known=true;s->children[4].reported_cost_usd=DBL_MAX;
 s->retired_spent_usd=INFINITY;
 assert(swarm_health_json(s,"{}",out,sizeof(out)));puts(out);
 free(s);
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-health-test-') as directory:
 p=Path(directory);(p/'test.c').write_text(unit)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L',
 '-I',str(ROOT/'include'),str(p/'test.c'),str(ROOT/'src/swarm_telemetry.c'),str(ROOT/'src/json_util.c'),
 str(ROOT/'src/env_config.c'),'-lm','-o',str(p/'test')],check=True)
 result=subprocess.run([str(p/'test')],capture_output=True,text=True,check=True)
 full,page,bounded,tiny,invalid,overflow=[json.loads(line) for line in result.stdout.splitlines()]
 assert full['counts']['active']==16 and full['counts']['done']==112
 assert len(full['workers'])==128 and full['workers_remaining']==0
 assert full['workers'][0]['stall_candidate'] is True
 assert full['workers'][1]['stall_candidate'] is None
 assert full['output_silence_candidates']==1 and full['progress_unavailable_workers']==15
 assert full['costs']['accounted_known_subtotal_usd']==.5
 assert full['costs']['reported_known_subtotal_usd']==.25
 assert full['costs']['estimated_known_subtotal_usd']==.1
 assert full['costs']['reported_unknown_workers']==127
 assert full['costs']['includes_subscriptions'] is True
 assert [w['id'] for w in page['workers']]==[16,17]
 assert page['next_worker_offset']==18
 assert bounded['workers_remaining']>0 and len(json.dumps(bounded,separators=(',',':')))<2400
 assert 'error' in tiny and 'error' in invalid
 assert overflow['costs']['reported_known_subtotal_usd'] is None
 assert overflow['costs']['retired_unclassified_cost_usd'] is None
 assert overflow['costs']['reported_unknown_workers']>0
 assert len(json.dumps(full))>25000
 print('PASS: health costs include subscriptions, unknowns retained, age is not stall, 128 workers, nonfinite costs and bounded JSON pagination')
