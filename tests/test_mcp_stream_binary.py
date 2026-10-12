#!/usr/bin/env python3
"""Framing and request identity through the actual governed CLI, local peer only."""
import argparse, hashlib, json, os, subprocess, sys, tempfile
from pathlib import Path
VALUE="OWNED_MCP_STREAM_π_🦉:"+"a"*80000

def server(log):
    for line in sys.stdin:
        request=json.loads(line)
        if 'id' not in request: continue
        method=request.get('method')
        if method=='initialize':
            result={'protocolVersion':'2024-11-05','capabilities':{'tools':{}},'serverInfo':{'name':'owned-stream','version':'1'}}
        elif method=='tools/list':
            result={'tools':[{'name':'fixture_read','description':'Owned framing fixture.','inputSchema':{'type':'object','properties':{'text':{'type':'string','description':'x'*32000}}}}]}
        elif method=='tools/call':
            with open(log,'a') as f:f.write(json.dumps(request)+'\n')
            result={'content':[{'type':'text','text':VALUE}],'isError':False}
        else: result={}
        messages=[{'jsonrpc':'2.0','method':'notifications/progress','params':{}},
                  {'jsonrpc':'2.0','id':request['id']+1000000,'result':{'content':[{'type':'text','text':'WRONG_REPLY'}]}},
                  {'jsonrpc':'2.0','id':request['id'],'result':result}]
        # A single peer write contains multiple records and crosses 4096-byte reads.
        sys.stdout.write(''.join(json.dumps(m,ensure_ascii=False)+'\n' for m in messages));sys.stdout.flush()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--binary',default='./dsco');parser.add_argument('--output',required=True);args=parser.parse_args()
    binary=str(Path(args.binary).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix='dsco-owned-mcp-stream-') as d:
        root=Path(d);log=root/'calls.jsonl'
        (root/'.mcp.json').write_text(json.dumps({'mcpServers':{'owned-stream':{'command':sys.executable,'args':[str(Path(__file__).resolve()),'--server',str(log)]}}}))
        env=os.environ.copy()
        for k in list(env):
            if k.startswith('DSCO_ALLOW_') or k in ('DSCO_GOV_BYPASS','DSCO_MCP_GOV_PINNED'):env.pop(k)
        env.update(DSCO_MCP_SERVER='owned-stream',DSCO_TRUST_TIER='trusted',DSCO_GOV_MODEL='standard',DSCO_PRICING_OFFLINE='1',DSCO_NO_AUTO_SUPERVISE='1',DSCO_SECURE_STORE_NO_PROMPT='1')
        command=[binary,'--tool-exec','mcp__owned-stream__fixture_read','{}']
        env['DSCO_ALLOW_NET']='0'
        denied=subprocess.run(command,cwd=root,env=env,text=True,capture_output=True,timeout=30)
        assert denied.returncode!=0 and 'disabled' in denied.stdout.lower(),(denied.stdout,denied.stderr)
        assert not log.exists(),'denied call reached peer'
        env['DSCO_ALLOW_NET']='1'
        permitted=subprocess.run(command,cwd=root,env=env,text=True,capture_output=True,timeout=30)
        assert permitted.returncode==0,(permitted.stdout[-2000:],permitted.stderr)
        response=json.loads(permitted.stdout);assert response['ok'],response
        result=response['result']
        if not isinstance(result,str):result=json.dumps(result,ensure_ascii=False)
        assert VALUE in result and 'WRONG_REPLY' not in result
        calls=[json.loads(line) for line in log.read_text().splitlines()]
        assert len(calls)==1 and calls[0]['params']['name']=='fixture_read'
        record={'passed':True,'binary':binary,'sha256':hashlib.sha256(Path(binary).read_bytes()).hexdigest(),'checks':['denied before tool transport','exactly one permitted tools/call','notifications and wrong IDs filtered','large schema discovered across pipe read boundaries','exact 80000-character UTF8 tool result delivered'],'tool_calls':len(calls),'result_sha256':hashlib.sha256(VALUE.encode()).hexdigest(),'limits':['Local stdio peer; no paid inference or external MCP service','Test subprocess timeout bounds fixture only; transport execution policy unchanged']}
        Path(args.output).write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))
if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1]=='--server':server(sys.argv[2])
    else:main()
