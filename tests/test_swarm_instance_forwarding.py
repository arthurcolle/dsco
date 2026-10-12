#!/usr/bin/env python3
"""Compile actual instance setup/export: prompt, cwd, limits, reset and fail-closed cwd."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
s = (ROOT / 'src/swarm.c').read_text()
a = s.index('static struct {', s.index('/* Optional per-spawn model-instance spec.'))
b = s.index('\nint swarm_spawn_in_group', a)
instance = s[a:b]
s = (ROOT / 'src/tools.c').read_text()
a = s.index('static void swarm_apply_task_instance(')
b = s.index('\nstatic const char *topology_strategy_label', a)
forward = s[a:b]
unit = r'''
#include "json_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
static void swarm_child_budget_export(double x){(void)x;}
typedef struct {
 char *task, *model, *provider, *executor, *effort;
 char *system_prompt, *tool_choice;
 double temperature;
} swarm_task_spec_t;
''' + instance + forward + r'''
int main(int argc,char**argv){
 assert(argc==2);swarm_task_spec_t spec={.effort="low"};
 char input[4096];snprintf(input,sizeof(input),"{\"system_prompt\":\"scope-marker\",\"cwd\":\"%s\",\"tool_choice\":\"none\",\"temperature\":0.25,\"max_tokens\":64}",argv[1]);
 swarm_apply_task_instance(&spec,input,3);
 assert(!strcmp(s_next_instance.system_prompt,"scope-marker"));
 assert(!strcmp(s_next_instance.cwd,argv[1]));
 pid_t pid=fork();assert(pid>=0);
 if(!pid){swarm_apply_instance_env();char cwd[4096];assert(getcwd(cwd,sizeof(cwd)));assert(!strcmp(cwd,argv[1]));
 assert(!strcmp(getenv("DSCO_SYSTEM_PROMPT"),"scope-marker"));
 assert(!strcmp(getenv("DSCO_EFFORT"),"low"));assert(!strcmp(getenv("DSCO_MAX_AGENT_TURNS"),"3"));
 assert(!strcmp(getenv("DSCO_MAX_TOKENS"),"64"));assert(!strcmp(getenv("DSCO_TOOL_CHOICE"),"none"));_exit(0);}
 int status;assert(waitpid(pid,&status,0)==pid&&WEXITSTATUS(status)==0);
 swarm_clear_next_instance();assert(!s_next_instance.set&&!s_next_instance.cwd&&!s_next_instance.system_prompt);
 swarm_apply_task_instance(&spec,"{\"cwd\":\"/nonexistent-dsco-fixture-cwd\"}",1);
 pid=fork();assert(pid>=0);if(!pid){swarm_apply_instance_env();_exit(99);}
 assert(waitpid(pid,&status,0)==pid&&WEXITSTATUS(status)==126);swarm_clear_next_instance();
 swarm_apply_task_instance(&spec,"{}",1);assert(!s_next_instance.cwd&&!s_next_instance.system_prompt);swarm_clear_next_instance();
 puts("PASS prompt, cwd, effort, tool policy, output/turn limits, fork environment, cleanup, invalid cwd");
}
'''
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory).resolve()
    (p/'test.c').write_text(unit)
    subprocess.run(['cc','-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L',
                    '-I',str(ROOT/'include'),str(p/'test.c'),str(ROOT/'src/json_util.c'),
                    '-lm','-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test'),str(p)],check=True)
