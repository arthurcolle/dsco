#!/usr/bin/env python3
"""Sixteen real processes coordinate through production SQLite IPC; no inference."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "ipc.h"
#include "event_loop.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#define WORKERS 16
#define JOBS 128
int ev_timer_repeat(ev_loop_t*l,int i,ev_timer_cb c,void*x){(void)l;(void)i;(void)c;(void)x;return -1;}
void ev_timer_cancel(ev_loop_t*l,int i){(void)l;(void)i;}
int main(int argc,char **argv){
 assert(argc==2 || argc==6);
 if(argc==6){
  assert(ipc_init(argv[1],argv[2])); char ch='r';
  assert(write(atoi(argv[3]),&ch,1)==1);
  assert(read(atoi(argv[4]),&ch,1)==1);
  ipc_message_t m[4];int n=ipc_recv(m,4);assert(n==2);
  int broadcast=0,directed=0;
  for(int j=0;j<n;j++){
   if(!strcmp(m[j].topic,"broadcast")){assert(!strcmp(m[j].body,"shared-contract"));broadcast++;}
   if(!strcmp(m[j].topic,"directed")){assert(!strcmp(m[j].body,argv[2]));directed++;}
   free(m[j].body);
  }
  assert(broadcast==1 && directed==1 && ipc_recv(m,4)==0);
  for(int j=0;j<JOBS/WORKERS;j++){
   ipc_task_t t;assert(ipc_task_claim(&t));assert(ipc_task_start(t.id,t.generation));
   assert(ipc_task_complete(t.id,t.generation,argv[2]));
   assert(!ipc_task_complete(t.id,t.generation,"duplicate"));
  }
  assert(ipc_send("parent","finished",argv[2]));ipc_shutdown();return 0;
 }
 assert(ipc_init(argv[1],"parent"));
 for(int i=0;i<WORKERS;i++){char id[32];snprintf(id,sizeof(id),"worker-%d",i);
  assert(ipc_agent_define(id,"parent",1,"worker","fixture","*"));
  assert(ipc_send(id,"directed",id));}
 assert(ipc_send(NULL,"broadcast","shared-contract"));
 for(int i=0;i<JOBS;i++) assert(ipc_task_submit("bounded task",0,0)>0);
 ipc_shutdown();int ready[2],go[2];assert(pipe(ready)==0 && pipe(go)==0);pid_t kids[WORKERS];
 for(int i=0;i<WORKERS;i++){
  kids[i]=fork();assert(kids[i]>=0);
  if(!kids[i]){char id[32],wr[32],rd[32];snprintf(id,sizeof(id),"worker-%d",i);
   snprintf(wr,sizeof(wr),"%d",ready[1]);snprintf(rd,sizeof(rd),"%d",go[0]);
   close(ready[0]);close(go[1]);execl(argv[0],argv[0],argv[1],id,wr,rd,"child",(char*)0);_exit(99);}
 }
 close(ready[1]);close(go[0]);char ch;
 for(int i=0;i<WORKERS;i++) assert(read(ready[0],&ch,1)==1);
 for(int i=0;i<WORKERS;i++) assert(write(go[1],"g",1)==1);
 close(ready[0]);close(go[1]);
 for(int i=0;i<WORKERS;i++){int status;assert(waitpid(kids[i],&status,0)==kids[i]);assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);}
 assert(ipc_init(argv[1],"parent"));ipc_task_t tasks[JOBS];assert(ipc_task_list(NULL,tasks,JOBS)==JOBS);
 int counts[WORKERS]={0};
 for(int i=0;i<JOBS;i++){assert(tasks[i].status==IPC_TASK_DONE);assert(tasks[i].generation==1);
  int worker=-1;assert(sscanf(tasks[i].result,"worker-%d",&worker)==1 && worker>=0 && worker<WORKERS);
  assert(!strcmp(tasks[i].result,tasks[i].assigned_to));counts[worker]++;free(tasks[i].result);}
 for(int i=0;i<WORKERS;i++)assert(counts[i]==JOBS/WORKERS);
 ipc_message_t receipts[WORKERS];assert(ipc_recv(receipts,WORKERS)==WORKERS);
 for(int i=0;i<WORKERS;i++){assert(!strcmp(receipts[i].topic,"finished"));free(receipts[i].body);}
 assert(ipc_recv(receipts,WORKERS)==0);ipc_shutdown();
 puts("PASS: 16 simultaneous processes, 128 uniquely completed tasks, independent broadcast/directed delivery, 16 collected receipts, no duplicate completion");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-parallel-coordination-') as directory:
    p=Path(directory)
    (p/'test.c').write_text(SOURCE)
    subprocess.run(['cc','-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L',
        '-I',str(ROOT/'include'),str(p/'test.c'),
        *[str(ROOT/'src'/f) for f in ('ipc.c','json_util.c','env_config.c')],
        '-lsqlite3','-lm','-o',str(p/'test')],check=True)
    proc=subprocess.Popen([str(p/'test'),str(p/'board.sqlite')],start_new_session=True)
    try:
        assert proc.wait(timeout=45)==0
    finally:
        if proc.poll() is None:
            import signal
            os.killpg(proc.pid,signal.SIGKILL)
            proc.wait()
