#!/usr/bin/env python3
"""Exercise real SQLite task migration/recovery without inference."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
unit=r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>
#include "ipc.h"
#include "event_loop.h"
int ev_timer_repeat(ev_loop_t *l,int i,ev_timer_cb c,void *x) {(void)l;(void)i;(void)c;(void)x;return -1;}
void ev_timer_cancel(ev_loop_t *l,int i) {(void)l;(void)i;}
int main(int argc,char **argv) {
 assert(argc==2); const char *db=argv[1];
 /* Keep assignments across closing a fixture connection, like a crashed process. */
 setenv("DSCO_DURABLE_BOOT_TASK_ID","fixture",1);
 assert(ipc_init(db,"sender"));
 int target=ipc_task_submit_to("recipient","targeted",10,0);
 int shared=ipc_task_submit("shared",0,0); assert(target>0 && shared>0); ipc_shutdown();
 assert(ipc_init(db,"recipient")); ipc_task_t task;
 assert(ipc_task_claim_targeted(&task) && task.id==target);
 assert(!strcmp(task.target_agent_id,"recipient"));
 assert(ipc_task_start(target,task.generation)); ipc_shutdown();
 /* Opening after owner loss triggers automatic orphan recovery. */
 assert(ipc_init(db,"stranger"));
 assert(!ipc_task_claim_targeted(&task));
 assert(ipc_task_claim(&task) && task.id==shared);
 assert(!task.target_agent_id[0]);
 assert(!ipc_task_claim(&task)); ipc_shutdown();
 assert(ipc_init(db,"recipient"));
 assert(ipc_task_claim_targeted(&task) && task.id==target);
 assert(ipc_task_requeue_stale(-1)==1); /* explicit stale-lease path retains target */
 ipc_shutdown();
 assert(ipc_init(db,"stranger"));
 assert(ipc_task_claim(&task) && task.id==shared);
 assert(!ipc_task_claim(&task)); ipc_shutdown();
 assert(ipc_init(db,"recipient"));
 assert(ipc_task_claim_targeted(&task) && task.id==target);
 assert(ipc_task_complete(target,task.generation,"done")); ipc_shutdown();
 /* Legacy active assignment lacks provenance: migrate conservatively pinned. */
 sqlite3 *sql; assert(sqlite3_open(db,&sql)==SQLITE_OK);
 assert(sqlite3_exec(sql,"DROP TABLE tasks; CREATE TABLE tasks (id INTEGER PRIMARY KEY AUTOINCREMENT,assigned_to TEXT,created_by TEXT,parent_task_id INTEGER DEFAULT 0,priority INTEGER DEFAULT 0,status TEXT DEFAULT 'pending',description TEXT DEFAULT '',result TEXT,created_at REAL,started_at REAL,completed_at REAL); INSERT INTO tasks(assigned_to,status,description) VALUES('legacy','running','old');",0,0,0)==SQLITE_OK);
 sqlite3_close(sql);
 assert(ipc_init(db,"stranger")); assert(!ipc_task_claim(&task)); ipc_shutdown();
 assert(ipc_init(db,"legacy")); assert(ipc_task_claim_targeted(&task));
 assert(!strcmp(task.target_agent_id,"legacy")); ipc_shutdown();
 puts("PASS: targeted recovery excludes unrelated workers; shared recovery and legacy migration retain semantics");
}
'''
with tempfile.TemporaryDirectory(prefix='dsco-ipc-target-') as directory:
 p=Path(directory); (p/'test.c').write_text(unit)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+[
 '-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L','-I',str(ROOT/'include'),
 str(p/'test.c'),str(ROOT/'src/ipc.c'),str(ROOT/'src/json_util.c'),str(ROOT/'src/env_config.c'),
 '-lsqlite3','-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p/'tasks.sqlite')],check=True)
