#!/usr/bin/env python3
"""Scale dispatch. Defaults offline; owned mode uses a local SSE provider."""
import argparse,http.server,json,os,pathlib,re,select,subprocess,tempfile,threading,time
p=argparse.ArgumentParser();p.add_argument('--live',action='store_true');p.add_argument('--owned-interplay',action='store_true');p.add_argument('--artifact-dir',required=True);args=p.parse_args()
repo=pathlib.Path(__file__).resolve().parents[1];binary=str(repo/'dsco');art=pathlib.Path(args.artifact_dir).resolve();art.mkdir(parents=True,exist_ok=True)
transcript=[]
class Server:
    def __init__(self,cwd,deny=False,extra_env=None,label='server'):
        env=os.environ.copy()
        # Keep operator hard denials. Use standard governance, not a bypass.
        env.update(DSCO_GOV_MODEL='standard',DSCO_PRICING_OFFLINE='1',DSCO_NO_AUTO_SUPERVISE='1')
        if extra_env:env.update(extra_env)
        env.pop('DSCO_GOV_BYPASS',None);env.pop('DSCO_MCP_GOV_PINNED',None)
        if deny:env['DSCO_ALLOW_RUN']='0'
        self.log=open(art/('denied.stderr' if deny else f'{label}.stderr'),'w')
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
        for logical in (128,256,512):
            request={'action':'scale','pure_tasks':True,'dry_run':True,'tasks':['immutable A']*(logical//2)+['immutable B']*(logical//2),'budget':.12}
            response,plan=s.tool(request)
            assert not response.get('isError') and plan['logical_tasks']==logical and plan['unique_tasks']==2,plan
            assert plan['logical_to_unique_task']==[0]*(logical//2)+[1]*(logical//2) and plan['planned_request']['budget']==.12
            print(f'PASS: live MCP scale plan coalesces {logical} logical jobs to two exact unique tasks')
        response,ordered=s.tool({'action':'scale','pure_tasks':True,'dry_run':True,'tasks':[
            {'task':'same','options':{'a':1,'b':2}},
            {'options':{'b':2,'a':1},'task':'same'},
            {'task':'different'}]})
        assert not response.get('isError') and ordered['unique_tasks']==2,ordered
        assert ordered['logical_to_unique_task']==[0,0,1] and len(ordered['planned_request']['tasks'])==2
        print('PASS: live MCP scale plan coalesces nested object key permutations')
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
            response,collected=s.tool({'action':'collect','group_id':gid,'group_generation':execution['group_generation'],'wait_ms':1})
            assert not response.get('isError') and collected.get('complete') is True,collected
            (art/'live-result.json').write_text(json.dumps({'launch':launch,'joined':joined,'collected':collected},indent=2)+'\n')
            assert json.loads((art/'live-result.json').read_text())['launch']['unique_tasks']==1
            print('PASS: 64 logical tasks -> ONE pinned native DSCO worker -> successful output and collected terminal group')
    finally:s.close()
    if args.owned_interplay:
        state={'active':0,'max':0,'requests':0};state_lock=threading.Lock()
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self,*unused):pass
            def do_POST(self):
                self.rfile.read(int(self.headers['Content-Length']))
                with state_lock:state['active']+=1;state['max']=max(state['max'],state['active']);state['requests']+=1
                try:
                    time.sleep(.3)
                    events=[{'type':'response.created','response':{'id':'owned-swarm-response'}},{'type':'response.output_text.delta','delta':'SWARM_ADAPTER_OK'},{'type':'response.completed','response':{'status':'completed','usage':{'input_tokens':10,'output_tokens':5,'input_tokens_details':{'cached_tokens':0}}}}]
                    body=''.join('data: '+json.dumps(event)+'\n\n' for event in events).encode()
                    self.send_response(200);self.send_header('Content-Type','text/event-stream');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body);self.wfile.flush()
                finally:
                    with state_lock:state['active']-=1
        provider=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);provider.daemon_threads=True
        provider_thread=threading.Thread(target=provider.serve_forever,daemon=True);provider_thread.start()
        owned_env=dict(HOME=work,TMPDIR=work,DSCO_ENV_FILE='/dev/null',DSCO_CHATGPT_OAUTH_TOKEN='owned-fixture-only',DSCO_CHATGPT_ACCOUNT_ID='owned-fixture-account',DSCO_CHATGPT_BASE_URL=f'http://127.0.0.1:{provider.server_port}/responses',OPENAI_API_KEY='owned-fixture-only',DSCO_DISABLE_SHARED_HOME_OAUTH='1',DSCO_DISABLE_CODEX_OAUTH_DISCOVERY='1',DSCO_PRICING_OFFLINE='1',DSCO_SECURE_STORE_NO_PROMPT='1',DSCO_TOOLMGMT='0',DSCO_DISABLE_DEFAULT_FALLBACKS='1',DSCO_AUTO_FALLBACK='0',DSCO_DYNAMIC_FAILOVER='0',DSCO_AUTO_GOAL='0',DSCO_REQUIRE_TOOL_USE='0',DSCO_PIXEL_TUI='0',DSCO_MCP_HEADLESS='0',DSCO_NO_AUTO_SUPERVISE='1',DSCO_NO_SUPERVISE='1',DSCO_CHATGPT_GLOBAL_GATE='1',DSCO_CHATGPT_LANE_CONCURRENCY='8',DSCO_CHATGPT_GATE_MAX_WAIT_MS='10000',DSCO_CHATGPT_MIN_INTERVAL_MS='0',DSCO_CHATGPT_SWIMLANE='owned-swarm-interplay',DSCO_SWARM_MAX_CHILDREN='512',DSCO_BUDGET='0.05',DSCO_MAX_TOKENS='32',DSCO_HARD_TURN_CEILING='2')
        s=Server(work,extra_env=owned_env,label='owned-interplay')
        owned_rows=[]
        try:
            for logical in (128,256,512):
                tasks=[]
                for i in range(logical):
                    unique=i%8;tasks.append({'task':f'Return exactly SWARM_ADAPTER_OK_{unique}.','provider':'openai-codex','model':'gpt-6-astra'})
                before=state['requests'];started=time.monotonic()
                response,launch=s.tool({'action':'scale','name':f'owned-interplay-{logical}','pure_tasks':True,'tasks':tasks,'budget':.05,'max_worker_turns':1,'max_tokens':32,'system_prompt':'Complete directly without tools or children.'})
                assert not response.get('isError') and launch['logical_tasks']==logical and launch['unique_tasks']==8,launch
                execution=launch['execution'];gid=execution['group_id'];assert execution['agents_spawned']==8,execution
                response,collected=s.tool({'action':'collect','group_id':gid,'group_generation':execution['group_generation'],'timeout':30})
                assert not response.get('isError') and collected.get('complete') is True,collected
                assert collected['group_generation']==execution['group_generation'],collected
                assert len(collected['results'])==8 and all(row['status']=='done' and 'SWARM_ADAPTER_OK' in row['output'] for row in collected['results']),collected
                physical=state['requests']-before;assert physical==8,(logical,physical,state)
                assert state['max']<=8,state
                owned_rows.append({'logical_requests':logical,'unique_adapter_requests':physical,'reduction_factor':logical/physical,'max_simultaneous_transports':state['max'],'wall_seconds':round(time.monotonic()-started,3)})
            assert state['requests']==24,state
            (art/'owned-interplay.json').write_text(json.dumps({'passed':True,'provider_requests':state['requests'],'rows':owned_rows},indent=2)+'\n')
            print('PASS: owned swarm adapters coalesce 128/256/512 logical requests into eight physical requests per wave')
        finally:
            s.close();provider.shutdown();provider.server_close();provider_thread.join(timeout=3)
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
