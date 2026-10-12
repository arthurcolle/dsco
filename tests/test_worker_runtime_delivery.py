#!/usr/bin/env python3
"""Real SQLite broadcast fanout, worker boundary ingestion and wait telemetry."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "ipc.h"
#include "event_loop.h"
#include "worker_runtime.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int applied;
void subscription_gate_set_wait_hook(void (*h)(const char *)){(void)h;}
void provider_transport_set_wait_hook(void (*h)(const char *)){(void)h;}
void conv_add_user_text(conversation_t*c,const char*s){(void)c;assert(strstr(s,"correction") || strstr(s,"Task 42 generation 1 completed"));applied++;}
int ev_timer_repeat(ev_loop_t*l,int i,ev_timer_cb c,void*x){(void)l;(void)i;(void)c;(void)x;return -1;}
void ev_timer_cancel(ev_loop_t*l,int i){(void)l;(void)i;}
int main(int argc,char**argv){
 unsetenv("DSCO_MAX_AGENT_TURNS");assert(worker_runtime_turn_ceiling(100)==100);
 assert(worker_runtime_turn_ceiling(0)==0);
 setenv("DSCO_MAX_AGENT_TURNS","2",1);assert(worker_runtime_turn_ceiling(100)==2);assert(worker_runtime_turn_ceiling(1)==1);
 assert(worker_runtime_turn_ceiling(0)==2);
 assert(argc==2);assert(ipc_init(argv[1],"parent"));
 assert(ipc_agent_define("child-A","parent",1,"worker","fixture","*"));
 assert(ipc_agent_define("child-B","parent",1,"worker","fixture","*"));
 assert(ipc_send(NULL,"scope","correction"));assert(ipc_unread_count()==0);
 ipc_message_t m[4];assert(ipc_list_inbox("child-A",true,true,m,4)==1);free(m[0].body);
 assert(ipc_list_inbox("child-A",true,false,m,4)==0);
 assert(ipc_list_inbox("child-B",true,false,m,4)==1);free(m[0].body);
 ipc_shutdown();assert(ipc_init(argv[1],"child-B"));
 conversation_t conv={0};worker_runtime_messages(&conv);assert(applied==1);
 worker_runtime_messages(&conv);assert(applied==1&&ipc_unread_count()==0);
 assert(ipc_send("child-B","scope","correction addressed"));
 worker_runtime_messages(&conv);assert(applied==2);
 /* Parent consumes the child outcome at a request boundary, then decides to follow up. */
 assert(ipc_send("parent","task.outcome","Task 42 generation 1 completed. Result: verified artifact. Review and act."));
 ipc_shutdown();assert(ipc_init(argv[1],"parent"));
 worker_runtime_messages(&conv);assert(applied==3);
 assert(ipc_send("child-B","task.followup","correction: verify artifact"));
 ipc_shutdown();assert(ipc_init(argv[1],"child-B"));
 worker_runtime_messages(&conv);assert(applied==4);
 setenv("DSCO_SUBAGENT","1",1);errno=EAGAIN;worker_runtime_wait("subscription_queue");assert(errno==EAGAIN);
 ipc_agent_info_t info;assert(ipc_get_agent("child-B",&info));
 assert(!strcmp(info.current_task,"subscription_queue")&&ipc_agent_alive("child-B"));
 worker_runtime_wait("provider_network");assert(ipc_get_agent("child-B",&info));
 assert(!strcmp(info.current_task,"provider_network"));ipc_shutdown();
 puts("PASS broadcast, parent outcome consumption and followup, boundary exactly once, wait heartbeat");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p / 'test.c').write_text(SOURCE)
    subprocess.run(['cc', '-std=c11', '-D_DARWIN_C_SOURCE', '-D_POSIX_C_SOURCE=200809L',
                    '-I', str(ROOT / 'include'), str(p / 'test.c'),
                    *[str(ROOT / 'src' / f) for f in ['ipc.c', 'json_util.c', 'env_config.c', 'worker_runtime.c']],
                    '-lsqlite3', '-lm', '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test'), str(p / 'ipc.sqlite')], check=True)
