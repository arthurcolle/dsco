#!/usr/bin/env python3
"""Compact prompts retain every native core tool; verify schemas and live gates."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'bench'))
from warm_mcp_compare import Host
NAMES=['bash','dsco-python-3x','discover_tools','load_tools','invoke_tool','evict_tools','read_file','write_file','edit_file','list_directory','find_files','grep_files']
HEAD=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "config.h"
#include "provider.h"
#include "tools.h"
#include "json_util.h"
#include "tool_grounding.h"
int g_cheap_mode;
bool goal_is_active(const session_state_t*s){(void)s;return false;}
int tools_external_count(void){return 0;}
bool pixel_tui_session_active(void){return false;}
bool tools_profile_allows_index(int index){int count=0;tools_get_all(&count);return index>=0&&index<count;}
static const char *openai_last_user_context(conversation_t*c){(void)c;return "";}
const tool_def_t **tools_get_filtered(const char*c,int n,int*out){(void)c;(void)n;*out=0;return NULL;}
external_tool_snapshot_t tools_external_snapshot(void){return (external_tool_snapshot_t){0};}
void tools_external_snapshot_free(external_tool_snapshot_t*s){(void)s;}
int tools_rank_external_snapshot(const external_tool_snapshot_t*s,const char*c,int*o,int n){(void)s;(void)c;(void)o;(void)n;return 0;}
static const char *chatgpt_model_id(session_state_t*s){return s->model;}
const char *llm_get_custom_system_prompt(void){return NULL;}
static void provider_append_goal_prompt(jbuf_t*b,session_state_t*s){(void)b;(void)s;}
static void provider_append_structured_output_prompt(jbuf_t*b,session_state_t*s){(void)b;(void)s;}
static void chatgpt_append_tool_items(jbuf_t*b,const message_t*m,bool*f,const char*r){(void)b;(void)m;(void)f;(void)r;assert(0);}
static void chatgpt_append_message_item(jbuf_t*b,const char*r,const message_t*m,bool*f){(void)b;(void)r;(void)m;(void)f;assert(0);}
const char *provider_claude_code_session_id(void){return "schema-test";}
static bool openai_parallel_tool_calls_enabled_for_provider(const char*p){(void)p;return true;}
const char *codex_cache_default_effort(const char*m){(void)m;return "high";}
const char *codex_cache_default_verbosity(const char*m){(void)m;return NULL;}
const char *dcr_reasoning_effort_normalize(const char*p,const char*m,const char*e,char*b,size_t n){(void)p;(void)m;(void)b;(void)n;return e;}
static _Thread_local char s_chatgpt_prompt_cache_key[128];
'''
MAIN=r'''
int main(int argc,char**argv){
 assert(argc==2);g_cheap_mode=atoi(argv[1]);
 unsetenv("DSCO_OR_DISABLE_TOOLS");setenv("DSCO_TOOL_PROXY","1",1);
 session_state_t s={0};snprintf(s.model,sizeof(s.model),"gpt-6-astra");snprintf(s.effort,sizeof(s.effort),"high");
 char *request=chatgpt_native_build_request(NULL,NULL,&s,100,NULL);assert(json_is_valid_container(request));puts(request);free(request);
 s.direct_answer_mode=true;request=chatgpt_native_build_request(NULL,NULL,&s,100,NULL);assert(!strstr(request,"\"tools\":"));assert(!strstr(request,"LIVE TOOLS — CURRENT REQUEST"));free(request);
 s.direct_answer_mode=false;setenv("DSCO_OR_DISABLE_TOOLS","1",1);request=chatgpt_native_build_request(NULL,NULL,&s,100,NULL);assert(!strstr(request,"\"tools\":"));assert(!strstr(request,"LIVE TOOLS — CURRENT REQUEST"));free(request);
}
'''
def function(source,name):
    match=re.search(r'^(?:static )?[^\n;]*\b'+name+r'\([^;{]*\)\s*\{',source,re.M);assert match,name
    return source[match.start():source.index('\n}\n',match.start())+3]

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--before-source',type=Path,required=True);p.add_argument('--binary',type=Path,default=ROOT/'dsco');p.add_argument('--schema-only',action='store_true')
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    catalog=[]; source=(ROOT/'src/tools.c').read_text()
    for name in NAMES:
        match=re.search(r'\{\.name\s*=\s*"'+re.escape(name)+r'"(?P<body>.*?)\.execute\s*=',source,re.S);assert match,name
        row={'name':name}
        for field in ['description','input_schema_json']:
            value=re.search(r'\.'+field+r'\s*=\s*(.*?)(?:,\s*\.[A-Za-z_]+\s*=|$)',match.group('body'),re.S);assert value,(name,field)
            row[field]=''.join(json.loads(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"',value.group(1),re.S))
        json.loads(row['input_schema_json']);catalog.append(row)
    table='static const tool_def_t catalog[]={'+','.join('{'+','.join('.'+key+'='+json.dumps(value) for key,value in row.items())+'}' for row in catalog)+'};\nconst tool_def_t *tools_get_all(int*n){*n=sizeof(catalog)/sizeof(catalog[0]);return catalog;}\n'
    requests={};builds={};bodies={}
    for variant,path in [('before',a.before_source.resolve()),('after',ROOT/'src/provider.c')]:
        source=path.read_text();append=function(source,'chatgpt_append_tools');proxy=''
        if 'openai_proxy_tools(' in append:
            names=re.search(r'static const char \*const openai_proxy_tool_names\[\] = \{.*?\n\};\n'
                            r'enum \{ OPENAI_PROXY_TOOL_LIMIT =.*?\};',source,re.S);assert names
            proxy=names[0]+'\n'+function(source,'openai_proxy_tools')
        unit=HEAD+table+proxy+append+function(source,'chatgpt_native_build_request')+MAIN
        cpath=out/f'{variant}.c';cpath.write_text(unit)
        command=['cc','-O1','-g','-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L','-fsanitize=address,undefined','-I',str(ROOT/'include'),str(cpath),str(ROOT/'src/json_util.c'),str(ROOT/'src/json_fast.c'),str(ROOT/'src/tool_grounding.c'),'-o',str(out/variant)]
        subprocess.run(command,check=True,timeout=30)
        builds[variant]={'provider_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'compiler_argv':command}
        requests[variant]={};bodies[variant]={}
        for cheap in [0,1]:
            run=subprocess.run([str(out/variant),str(cheap)],capture_output=True,text=True,check=True,timeout=10)
            raw=run.stdout.rstrip('\n');body=json.loads(raw);expected=NAMES
            bodies[variant][str(cheap)]=body
            assert [t['name'] for t in body['tools']]==expected
            if variant=='after':
                assert body['instructions'].count('LIVE TOOLS — CURRENT REQUEST')==1
                inventory=re.search(r'Directly advertised tool names \((\d+)\): ([^\n]+)\.\n',body['instructions'])
                assert inventory and int(inventory[1])==len(NAMES)
                assert sorted(inventory[2].split(', '))==sorted(NAMES)
            for tool in body['tools']:
                original=next(t for t in catalog if t['name']==tool['name'])
                assert tool['parameters']==json.loads(original['input_schema_json']) and tool['description']==original['description']
            requests[variant][str(cheap)]={'request_bytes':len(raw.encode()),'tool_count':len(body['tools']),'tool_names':expected,'schema_bytes':len(json.dumps(body['tools'],separators=(',',':')).encode()),'instruction_bytes':len(body['instructions'].encode())}
            (out/f'{variant}-cheap-{cheap}.json').write_text(raw+'\n')
    # Shrinking the tool list saved bytes but added model discovery/load turns.
    # Both profiles retain the exact full direct tools; only instructions shrink.
    assert bodies['before']==bodies['after']
    assert bodies['after']['0']['tools']==bodies['after']['1']['tools']
    assert requests['after']['1']['instruction_bytes']<requests['after']['0']['instruction_bytes']
    assert requests['after']['1']['request_bytes']<requests['after']['0']['request_bytes']
    if a.schema_only:
        result={'passed':True,'schema_only':True,'builds':builds,'requests':requests,'verification':None}
        (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result,indent=2));return
    env=os.environ.copy()
    for key in ['DSCO_GOV_BYPASS','DSCO_ALLOW_READ','DSCO_ALLOW_WRITE','DSCO_ALLOW_RUN','DSCO_ALLOW_NET','DSCO_ALLOW_CONTROL','DSCO_ALLOW_SECRETS','DSCO_ALLOW_EXFIL','DSCO_MCP_SERVER','DSCO_MCP_SERVERS']:
        env.pop(key,None)
    env.update(DSCO_GOV_MODEL='standard',DSCO_TOOLMGMT='0',DSCO_MCP_HEADLESS='0',DSCO_NO_AUTO_INTERACTIVE='1')
    verification={}
    with tempfile.TemporaryDirectory(prefix='dsco-compact-gate-') as temp:
        work=Path(temp); read=work/'read.txt';read.write_text('progressive-read-proof')
        argv=[str(a.binary.resolve()),'--cheap','mcp','serve','--toolsets','all','--tier','trusted']
        for locked in [False,True]:
            child_env=dict(env)
            if locked:child_env['DSCO_ALLOW_WRITE']='0'
            host=Host('dsco',argv,work,child_env,out,'write-denied' if locked else 'trusted')
            try:
                def call(name,args):return host.call('tools/call',{'name':name,'arguments':args})[0]
                for name in NAMES[6:]:
                    result=call('discover_tools',{'query':name,'limit':3})
                    assert name in json.dumps(result['result'])
                if locked:
                    # Direct file tools stay in both advertisements; read-only
                    # sessions do not depend on wrapper write grants.
                    response=call('read_file',{'path':str(read)})
                else:
                    response=call('invoke_tool',{'name':'read_file','input':{'path':str(read)}})
                assert 'progressive-read-proof' in json.dumps(response['result'])
                target=work/('denied.txt' if locked else 'written.txt')
                args={'name':'write_file','input':{'path':str(target),'content':'progressive-write-proof'}}
                if locked:
                    try:call('invoke_tool',args)
                    except RuntimeError as exc:assert 'DSCO_ALLOW_WRITE=0' in str(exc) and 'governance_block' in str(exc)
                    else:raise AssertionError('indirect write bypassed capability denial')
                    assert not target.exists()
                else:
                    call('invoke_tool',args);assert target.read_text()=='progressive-write-proof'
                verification['denied' if locked else 'trusted']={'discovered_file_tools':NAMES[6:],'indirect_read':not locked,'direct_read':locked,'write_denied' if locked else 'indirect_write':True}
            finally:host.close()
    result={'passed':True,'builds':builds,'requests':requests,'verification':verification,'mcp_binary':str(a.binary.resolve()),'mcp_binary_sha256':hashlib.sha256(a.binary.read_bytes()).hexdigest(),'scope':'Production native request/tool builder with exact builtin schema literals; empty conversation and unchanged runtime extras. Full and compact request bytes include their existing respective system prompts. No model requests. Live MCP checks exercise discovery and gated indirect file operations in compact mode.'}
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'passed':True,'requests':requests,'verification':verification},indent=2))
if __name__=='__main__':main()
