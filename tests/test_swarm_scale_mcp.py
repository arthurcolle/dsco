#!/usr/bin/env python3
"""Live scale dispatch. Default is offline; --live makes ONE native inference worker."""
import argparse,json,os,pathlib,re,select,subprocess,tempfile,time
p=argparse.ArgumentParser();p.add_argument('--live',action='store_true');p.add_argument('--artifact-dir',required=True);args=p.parse_args()
repo=pathlib.Path(__file__).resolve().parents[1];binary=str(repo/'dsco');art=pathlib.Path(args.artifact_dir).resolve();art.mkdir(parents=True,exist_ok=True)
transcript=[]
class Server:
    def __init__(self,cwd,deny=False):
        env=os.environ.copy()
        # Keep operator hard denials. Use standard governance, not a bypass.
        env.update(DSCO_GOV_MODEL='standard',DSCO_PRICING_OFFLINE='1',DSCO_NO_AUTO_SUPERVISE='1')
        env.pop('DSCO_GOV_BYPASS',None);env.pop('DSCO_MCP_GOV_PINNED',None)
        if deny:env['DSCO_ALLOW_RUN']='0'
        self.log=open(art/('denied.stderr' if deny else 'server.stderr'),'w')
        self.p=subprocess.Popen([binary,'mcp','serve','--toolsets','all','--tier','trusted'],cwd=cwd,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.log,text=True)
        self.id=0
        self.call('initialize',{'protocolVersion':'2024-11-05','capabilities':{},'clientInfo':{'name':'scale-proof','version':'1'}})
    def call(self,method,params):
        self.id+=1;req={'jsonrpc':'2.0','id':self.id,'method':method,'params':params}
        self.p.stdin.write(json.dumps(req)+'\n');self.p.stdin.flush()
        ready,_,_=select.select([self.p.stdout],[],[],100)
        assert ready,'MCP response deadline; inspect worker status, do not blindly resubmit'
        reply=json.loads(self.p.stdout.readline());assert reply.get('id')==self.id,reply
        transcript.append({'request':req,'response':reply})
        return reply
    def tool(self,arguments,name='swarm'):
        reply=self.call('tools/call',{'name':name,'arguments':arguments})
        response=reply.get('result',{});texts='\n'.join(x.get('text','') for x in response.get('content',[]))
        try:value=json.loads(texts)
        except json.JSONDecodeError:value={'text':texts}
        return response,value
    def close(self):
        if self.p.poll() is None:
            self.p.stdin.close()
            try:self.p.wait(timeout=8)
            except subprocess.TimeoutExpired:self.p.terminate();self.p.wait(timeout=5)
        self.log.close()
with tempfile.TemporaryDirectory(prefix='dsco-scale-proof-') as work:
    s=Server(work)
    try:
        request={'action':'scale','pure_tasks':True,'dry_run':True,'tasks':['immutable A']*64+['immutable B']*64,'budget':.12}
        response,plan=s.tool(request)
        assert not response.get('isError') and plan['logical_tasks']==128 and plan['unique_tasks']==2,plan
        assert plan['logical_to_unique_task']==[0]*64+[1]*64 and plan['planned_request']['budget']==.12
        print('PASS: live MCP scale plan coalesces 128 logical jobs to two exact unique tasks')
        response,bad=s.tool({'action':'scale','tasks':['x','x']})
        assert response.get('isError') and bad['submitted'] is False
        if args.live:
            task={'task':'Return exactly SCALE_OK. Do not use tools, delegate, or write files.','provider':'openai-codex','model':'gpt-5.6-luna'}
            response,launch=s.tool({'action':'scale','name':'scale-live-proof','pure_tasks':True,'tasks':[task]*64,'budget':.12,'max_worker_turns':1,'max_tokens':128,'system_prompt':'Complete the bounded task directly. No tools or children.'})
            assert not response.get('isError') and launch['unique_tasks']==1,launch
            execution=launch['execution'];assert execution['agents_spawned']==1,execution
            worker=execution['agent_ids'][0];gid=execution['group_id']
            assert launch['logical_worker_ids']==[worker]*64 and not launch['result_reconciliation_required']
            response,joined=s.tool({'action':'wait','id':worker,'timeout':80},'agent')
            assert not response.get('isError') and 'SCALE_OK' in json.dumps(joined),joined
            clean=re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',joined['output']).strip()
            assert joined['status']=='done' and clean.splitlines()[-1]=='SCALE_OK',joined
            resolved={worker:clean.splitlines()[-1]}
            assert [resolved[w] for w in launch['logical_worker_ids']]==['SCALE_OK']*64
            response,collected=s.tool({'action':'collect','group_id':gid,'wait_ms':1})
            assert not response.get('isError') and collected.get('complete') is True,collected
            (art/'live-result.json').write_text(json.dumps({'launch':launch,'joined':joined,'collected':collected},indent=2)+'\n')
            assert json.loads((art/'live-result.json').read_text())['launch']['unique_tasks']==1
            print('PASS: 64 logical tasks -> ONE pinned native DSCO worker -> successful output and collected terminal group')
    finally:s.close()
    s=Server(work,deny=True)
    try:
        response,denied=s.tool({'action':'scale','pure_tasks':True,'tasks':['x','x']})
        assert response.get('isError') and ('disabled' in json.dumps(denied).lower() or 'denied' in json.dumps(denied).lower()),denied
        print('PASS: live DSCO_ALLOW_RUN=0 denies scale before worker execution')
    finally:s.close()
    # Preserve isolated native receipts before temporary state goes away.
    for child in pathlib.Path(work).rglob('*.RESULT.json'):
        dest=art/'native-receipts'/child.relative_to(work);dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(child.read_bytes());assert dest.read_bytes()==child.read_bytes()
(art/'mcp-transcript.json').write_text(json.dumps(transcript,indent=2)+'\n')
assert json.loads((art/'mcp-transcript.json').read_text())==transcript
