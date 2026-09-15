#!/usr/bin/env python3
"""Regression for stale IPC completion, including same-identity reclaims."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "ipc.h"
#include "event_loop.h"
int ev_timer_repeat(ev_loop_t*l,int i,ev_timer_cb c,void*x){(void)l;(void)i;(void)c;(void)x;return -1;}
void ev_timer_cancel(ev_loop_t*l,int i){(void)l;(void)i;}
int main(int argc,char**argv){
 assert(argc>=2);setenv("DSCO_DURABLE_BOOT_TASK_ID","fixture",1);
 if(argc==3){
  assert(ipc_init(argv[1],"worker-B"));ipc_task_t b;
  assert(ipc_task_claim(&b));assert(ipc_task_start(b.id,b.generation));
  assert(ipc_task_complete(b.id,b.generation,"fresh-result-B"));ipc_shutdown();return 0;
 }
 assert(ipc_init(argv[1],"worker-A"));
 int id=ipc_task_submit("shared",0,0);ipc_task_t a;
 assert(ipc_task_claim(&a)&&a.id==id);assert(a.generation==1);
 assert(ipc_task_start(id,a.generation));assert(ipc_task_requeue_stale(-1)==1);
 pid_t p=fork();assert(p>=0);
 if(p==0){execl(argv[0],argv[0],argv[1],"worker",(char*)0);_exit(99);}
 int status;assert(waitpid(p,&status,0)==p&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 assert(!ipc_task_complete(id,a.generation,"stale-A"));
 assert(!ipc_task_fail(id,a.generation,"stale-failure-A"));
 assert(!ipc_task_start(id,a.generation));
 ipc_task_t listed[4];int count=ipc_task_list(NULL,listed,4);assert(count==1);
 assert(!strcmp(listed[0].assigned_to,"worker-B")&&listed[0].generation==2);
 assert(listed[0].status==IPC_TASK_DONE&&!strcmp(listed[0].result,"fresh-result-B"));free(listed[0].result);
 puts("PASS: stale process cannot overwrite another worker's result");
 id=ipc_task_submit("same identity",0,0);assert(ipc_task_claim(&a)&&a.id==id);
 assert(ipc_task_start(id,a.generation));assert(ipc_task_requeue_stale(-1)==1);
 ipc_task_t newer;assert(ipc_task_claim(&newer)&&newer.id==id&&newer.generation>a.generation);
 assert(!ipc_task_start(id,a.generation));assert(!ipc_task_complete(id,a.generation,"old"));
 assert(!ipc_task_fail(id,a.generation,"old"));assert(ipc_task_start(id,newer.generation));
 assert(ipc_task_complete(id,newer.generation,"new"));
 assert(!ipc_task_complete(id,newer.generation,"duplicate overwrite"));
 assert(!ipc_task_complete(99999,1,"missing"));
 puts("PASS: old generation is fenced even when the owner and PID match; terminal states cannot be overwritten");
 int wrong=ipc_task_submit_to("someone-else","wrong boot",0,0);
 int boot=ipc_task_submit_to("worker-A","exact boot",0,0);
 assert(!ipc_task_claim_id(wrong,&a));assert(ipc_task_claim_id(boot,&a)&&a.id==boot);
 assert(ipc_task_start(a.id,a.generation));assert(ipc_task_complete(a.id,a.generation,"boot done"));
 puts("PASS: durable boot claims only the exact pending task routed to its identity");
 ipc_shutdown();return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-ipc-fencing-') as directory:
 p=Path(directory);(p/'test.c').write_text(SOURCE)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+[
  '-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L','-I',str(ROOT/'include'),
  str(p/'test.c'),str(ROOT/'src/ipc.c'),str(ROOT/'src/json_util.c'),str(ROOT/'src/env_config.c'),
  '-lsqlite3','-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p/'board.sqlite')],check=True)
