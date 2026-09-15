#!/usr/bin/env python3
"""Offline regression: real prompt getter and native builder retain compact guidance."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'^(?:static )?[^\n;]*\b' + name + r'\([^;{]*\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}\n', match.start()) + 3]


HEAD = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "provider.h"
#include "json_util.h"
#include "tool_grounding.h"
int g_cheap_mode;
static const char *workspace;
static unsigned workspace_calls;
const char *dsco_workspace_prompt(void){workspace_calls++;return workspace;}
static const char *chatgpt_model_id(session_state_t*s){return s->model;}
static void provider_append_goal_prompt(jbuf_t*b,session_state_t*s){(void)b;(void)s;}
static void provider_append_structured_output_prompt(jbuf_t*b,session_state_t*s){(void)b;(void)s;}
const char *provider_claude_code_session_id(void){return "fixture-session";}
static bool chatgpt_append_tools(jbuf_t*b,conversation_t*c,session_state_t*s){(void)b;(void)c;(void)s;return false;}
/* This fixture intentionally has no advertised tools. Never hide a manifest
 * accidentally introduced by the production builder behind a no-op helper. */
void tool_grounding_append(jbuf_t*b,const char*tools){(void)b;assert(!tools||!*tools);}
static bool openai_parallel_tool_calls_enabled_for_provider(const char*p){(void)p;return true;}
const char *codex_cache_default_effort(const char*m){(void)m;return "high";}
const char *codex_cache_default_verbosity(const char*m){(void)m;return NULL;}
const char *dcr_reasoning_effort_normalize(const char*p,const char*m,const char*e,char*b,size_t n){(void)p;(void)m;(void)b;(void)n;return e;}
static _Thread_local char s_chatgpt_prompt_cache_key[128];
static void chatgpt_append_tool_items(jbuf_t*b,const message_t*m,bool*f,const char*model){(void)b;(void)m;(void)f;(void)model;}
static void chatgpt_append_message_item(jbuf_t*b,const char*r,const message_t*m,bool*f){(void)b;(void)r;(void)m;(void)f;}
'''
MAIN = r'''
int main(void){
 const char *guidance="[Project AGENTS.md]\nparent: preserve public ABI\nchild: run targeted tests\n"
                      "[User]\nPreserve unrelated edits.\n[Skill Policy]\nFollow explicitly named skills.";
 session_state_t session={0};snprintf(session.model,sizeof(session.model),"fixture-model");
 snprintf(session.effort,sizeof(session.effort),"high");
 unsigned cases=0;
 for(int cheap=0;cheap<2;cheap++)for(int have_workspace=1;have_workspace>=0;have_workspace--)
 for(int override=0;override<3;override++){
  g_cheap_mode=cheap;workspace=have_workspace?guidance:NULL;workspace_calls=0;
  if(override==0)unsetenv("DSCO_SYSTEM_PROMPT");
  else setenv("DSCO_SYSTEM_PROMPT",override==1?"":"explicit worker guidance",1);
  const char *expected_custom=override==2?"explicit worker guidance":workspace;
  char *request=chatgpt_native_build_request(NULL,NULL,&session,100,NULL);
  char *instructions=json_get_str(request,"instructions");
  jbuf_t expected;jbuf_init(&expected,1024);
  if(expected_custom){jbuf_append(&expected,expected_custom);jbuf_append(&expected,"\n\n");}
  jbuf_append(&expected,cheap?SYSTEM_PROMPT_CHEAP:SYSTEM_PROMPT);
  assert(instructions && !strcmp(instructions,expected.data));
  assert(workspace_calls==(override==2?0:1));
  assert(strstr(request,"\"reasoning\":{\"effort\":\"high\"}"));
  assert(strstr(instructions,"Completion means verified acceptance criteria"));
  assert(strstr(instructions,"Autonomy never expands authority"));
  free(instructions);free(request);jbuf_free(&expected);cases++;
 }
 printf("PASS: %u full/compact native request cases preserve workspace guidance, explicit override precedence, empty workspace, model effort and shared contracts\n",cases);
}
'''


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--llm-source',type=Path,default=ROOT/'src/llm.c')
    args=parser.parse_args()
    llm=args.llm_source.read_text();provider=(ROOT/'src/provider.c').read_text()
    code='\n'.join([HEAD,function(llm,'llm_get_custom_system_prompt'),
                    function(provider,'chatgpt_native_build_request'),MAIN])
    with tempfile.TemporaryDirectory(prefix='dsco-compact-workspace-') as directory:
        path=Path(directory);(path/'test.c').write_text(code)
        subprocess.run(['cc','-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L',
                        '-fsanitize=address,undefined','-g','-I',str(ROOT/'include'),
                        str(path/'test.c'),str(ROOT/'src/json_util.c'),'-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True)


if __name__=='__main__':
    main()
