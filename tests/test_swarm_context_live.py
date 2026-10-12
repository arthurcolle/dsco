#!/usr/bin/env python3
"""Opt-in live native DSCO canary; two pinned workers, $0.20 total child budget."""
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/'dsco'
with tempfile.TemporaryDirectory(prefix='dsco-context-canary-') as directory:
    cwd = Path(directory).resolve()
    env = dict(os.environ, DSCO_PRICING_OFFLINE='1')
    with (cwd/'stderr.log').open('w+') as err:
        p = subprocess.Popen([str(BINARY),'mcp','serve','--toolsets','all','--tier','trusted'],
                             cwd=cwd,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                             stderr=err,text=True,bufsize=1)
        seq=0
        def call(method,params):
            global seq
            seq+=1
            p.stdin.write(json.dumps(dict(jsonrpc='2.0',id=seq,method=method,params=params))+'\n');p.stdin.flush()
            deadline=time.monotonic()+40
            while time.monotonic()<deadline:
                if not select.select([p.stdout],[],[],1)[0]: continue
                line=p.stdout.readline()
                if not line: raise RuntimeError('MCP exited')
                try: reply=json.loads(line)
                except ValueError: continue
                if reply.get('id')!=seq: continue
                assert 'error' not in reply,reply
                if method=='initialize': return reply
                result=reply['result'];assert not result.get('isError'),result
                return json.loads(''.join(x.get('text','') for x in result['content']))
            raise TimeoutError(method)
        try:
            call('initialize',{'protocolVersion':'2024-11-05','capabilities':{},'clientInfo':{'name':'context-canary','version':'1'}})
            p.stdin.write(json.dumps({'jsonrpc':'2.0','method':'notifications/initialized'})+'\n');p.stdin.flush()
            marker='CONTEXT_CANARY_7351'
            spawn=call('tools/call',{'name':'swarm','arguments':{
                'action':'create','name':'context-canary','cwd':str(cwd),'budget':.2,'max_worker_turns':6,'max_tokens':256,
                'system_prompt':'Harmless runtime canary. Respond with exactly '+marker+'. Do not read files or invoke tools.',
                'tasks':[{'provider':'openai-codex','model':'gpt-5.6-luna','executor':'dsco','task':'Return the marker specified in your custom system instructions.'} for _ in range(2)]}})
            assert spawn['agents_spawned']==2,spawn
            end=time.monotonic()+180
            while True:
                result=call('tools/call',{'name':'swarm','arguments':{'action':'collect','group_id':spawn['group_id'],'group_generation':spawn['group_generation'],'timeout':10}})
                if result.get('complete'): break
                if time.monotonic()>end: raise TimeoutError('worker deadline')
            assert len(result.get('results',[]))==2,result
            for row in result['results']:
                assert row['status']=='done' and marker in row.get('output',''),row
                assert row['model']=='gpt-5.6-luna',row
            print('PASS actual MCP create: two native pinned workers received custom marker; collected both')
        finally:
            p.stdin.close()
            try: p.wait(timeout=8)
            except subprocess.TimeoutExpired: p.terminate();p.wait(timeout=5)
