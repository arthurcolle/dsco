#!/usr/bin/env python3
"""Native Codex -> Kimi recovery after an executed write; entirely local HTTP."""
import http.server, json, os, sqlite3, subprocess, tempfile, threading, sys
from pathlib import Path
from test_stream_completion import event
from test_context_recovery_binary import verify_protocol
binary=Path(sys.argv[1] if len(sys.argv)>1 else './dsco').resolve()
interactive='--interactive' in sys.argv
disabled='--disabled' in sys.argv
with tempfile.TemporaryDirectory(prefix='dsco-provider-continuity-') as tmp:
    root=Path(tmp); (root/'.dsco').mkdir()
    (root/'.dsco/codex_models.json').write_text(json.dumps({'models':[{'slug':'gpt-6-astra','supported_in_api':True,'context_window':272000,'priority':1}]}))
    output=root/'once.txt'; requests=[]; errors=[]; state={}
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*_): pass
        def do_POST(self):
            try:
                req=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                requests.append((self.path,req)); stage=len(requests)
                assert stage<=3,(stage,self.path)
                assert 'CONTINUITY_OBJECTIVE' in json.dumps(req)
                if stage<=2: verify_protocol(req)
                if stage==1:
                    assert self.path=='/responses'
                    item={'type':'function_call','id':'item1','name':'write_file','call_id':'write-once',
                          'arguments':json.dumps({'path':str(output),'content':'ONCE'})}
                    data=event({'type':'response.created','response':{'id':'r1'}})
                    data+=event({'type':'response.output_item.added','output_index':0,'item':item})
                    data+=event({'type':'response.output_item.done','output_index':0,'item':item})
                    data+=event({'type':'response.completed','response':{'status':'completed','usage':{'input_tokens':10,'output_tokens':5}}})
                elif stage==2:
                    assert output.read_text()=='ONCE'; state['mtime']=output.stat().st_mtime_ns
                    self.send_response(401); self.send_header('Content-Type','application/json'); self.end_headers()
                    self.wfile.write(b'{"error":{"message":"fixture expired credential","type":"authentication_error"}}'); return
                else:
                    assert self.path.endswith('/chat/completions'),self.path
                    assert req['model']=='k3',req['model']
                    assert output.stat().st_mtime_ns==state['mtime'],'write replayed'
                    messages=req['messages']; encoded=json.dumps(messages)
                    assert 'write-once' in encoded and 'verified write' in encoded,'tool history lost on provider switch'
                    data='data: '+json.dumps({'id':'r3','choices':[{'index':0,'delta':{'content':'CONTINUITY_FINISHED'},'finish_reason':None}]})+'\n\n'
                    data+='data: '+json.dumps({'id':'r3','choices':[{'index':0,'delta':{},'finish_reason':'stop'}],'usage':{'prompt_tokens':10,'completion_tokens':5}})+'\n\n'
                    data+='data: [DONE]\n\n'
                self.send_response(200); self.send_header('Content-Type','text/event-stream'); self.send_header('Content-Length',str(len(data.encode()))); self.end_headers(); self.wfile.write(data.encode())
            except Exception as exc: errors.append(repr(exc)); self.close_connection=True
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    thread=threading.Thread(target=server.serve_forever,daemon=True); thread.start()
    base=f'http://127.0.0.1:{server.server_port}'
    env=dict(HOME=tmp,TMPDIR=tmp,PATH='/usr/bin:/bin:/usr/sbin:/sbin',DSCO_ENV_FILE='/dev/null',
        DSCO_CHATGPT_OAUTH_TOKEN='fixture',DSCO_CHATGPT_ACCOUNT_ID='fixture',DSCO_CHATGPT_BASE_URL=base+'/responses',
        KIMI_CODE_OAUTH_TOKEN='fixture-kimi',KIMI_CODE_API_BASE=base+'/v1',
        DSCO_DISABLE_SHARED_HOME_OAUTH='1',DSCO_PRICING_OFFLINE='1',DSCO_SECURE_STORE_NO_PROMPT='1',
        DSCO_NO_AUTO_SUPERVISE='1',DSCO_NO_SUPERVISE='1',DSCO_DURABLE_AUTOWAKE='0',
        DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1',DSCO_AUTO_GOAL='0',DSCO_HARD_TURN_CEILING='5',
        DSCO_BUDGET='0',DSCO_TOOL_PROXY='1',DSCO_TOOL_ALLOWLIST='write_file,context_evict',
        DSCO_SYSTEM_PROMPT='Preserve instructions and finish the task.',DSCO_AUTO_FALLBACK='1',
        DSCO_CROSS_PROVIDER_FALLBACK='0' if disabled else '1',DSCO_CONTINUITY_PROVIDERS='kimi-code',
        DSCO_CHRONICLE_MODE='full-local',DSCO_CHRONICLE_DIR=str(root/'chronicle'))
    try:
        p=subprocess.run([str(binary),*(['--tui','-i'] if interactive else []),'--profile','worker','--provider','openai-codex','-m','gpt-6-astra',
            '--trust-tier','trusted','--approval-mode','never','-p','CONTINUITY_OBJECTIVE: write once then finish.'],
            env=env,cwd=tmp,text=True,input='/quit\n' if interactive else None,capture_output=True,timeout=90)
        assert not errors,errors
        if disabled:
            assert len(requests)==2 and 'CONTINUITY_FINISHED' not in p.stdout,(len(requests),p.stdout)
            print(json.dumps({'passed':True,'opt_out_respected':True,'requests':len(requests)}))
            sys.exit(0)
        assert p.returncode==0,(p.returncode,p.stderr[-4000:])
        assert len(requests)==3 and 'CONTINUITY_FINISHED' in (p.stdout+p.stderr),(len(requests),p.stdout,p.stderr[-1500:])
        assert output.read_text()=='ONCE' and output.stat().st_mtime_ns==state['mtime']
        db=sqlite3.connect(root/'chronicle/indexes/chronicle.sqlite')
        reqs=db.execute("SELECT trace_id,span_id,json_extract(payload_json,'$.model') FROM events WHERE event_type='llm.request.created' ORDER BY seq").fetchall()
        responses=db.execute("SELECT trace_id,span_id,json_extract(payload_json,'$.model'),json_extract(payload_json,'$.response_status') FROM events WHERE event_type='llm.response.completed' ORDER BY seq").fetchall()
        assert len(reqs)==len(responses)==3,(interactive,reqs,responses)
        assert all(reqs[i][:2]==responses[i][:2] for i in range(3)),(reqs,responses)
        assert [row[3] for row in responses]==['completed','failed','completed'],responses
        assert responses[2][2]=='kimi-code/k3' and reqs[2][2]==responses[2][2],responses
        db.close()
        print(json.dumps({'passed':True,'requests':3,'from':'openai-codex','to':'kimi-code','history_preserved':True,'write_executed_once':True,'real_inference':False,'interactive':interactive}))
    finally:
        server.shutdown(); server.server_close(); thread.join(timeout=3)
