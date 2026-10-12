#!/usr/bin/env python3
"""Offline cumulative sub-spill-threshold rolling test; no inference or GUI."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import sys
from test_stream_completion import event
from test_context_recovery_binary import verify_protocol, full_events

binary = Path(sys.argv[1] if len(sys.argv) > 1 else './dsco').resolve()
turns = 55
payload = 'x' * 18000
target_bytes = int(os.environ.get('DSCO_TEST_REQUEST_TARGET_BYTES', '131072'))
requests, request_bytes, errors = [], [], []
with tempfile.TemporaryDirectory(prefix='dsco-rolling-headless-') as tmp:
    root = Path(tmp)
    (root / '.dsco').mkdir()
    (root / '.dsco/codex_models.json').write_text(json.dumps({'models': [
        {'slug': 'gpt-6-astra', 'supported_in_api': True, 'context_window': 272000,
         'default_reasoning_level': 'low', 'priority': 1}]}))
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_POST(self):
            try:
                body = self.rfile.read(int(self.headers['Content-Length']))
                request_bytes.append(len(body))
                req = json.loads(body)
                requests.append(req)
                stage = len(requests)
                assert stage <= turns + 1
                verify_protocol(req)
                assert 'PRESERVE_ROLLING_OBJECTIVE' in json.dumps(req)
                if stage > 1:
                    assert (root / f'{stage-1}.txt').read_text() == payload
                    ids = [x.get('call_id') for x in req['input'] if x.get('type') == 'function_call']
                    assert f'call-{stage-1}' in ids, 'latest exchange lost'
                data = event({'type':'response.created','response':{'id':f'r{stage}'}})
                if stage <= turns:
                    item = {'type':'function_call','id':f'i{stage}','name':'write_file',
                            'call_id':f'call-{stage}', 'arguments':json.dumps(
                                {'path':str(root / f'{stage}.txt'),'content':payload})}
                    data += event({'type':'response.output_item.added','output_index':0,'item':item})
                    data += event({'type':'response.output_item.done','output_index':0,'item':item})
                else:
                    data += event({'type':'response.output_text.delta','delta':'ROLLING_FINISHED'})
                data += event({'type':'response.completed','response':{'status':'completed',
                    'usage':{'input_tokens':100,'output_tokens':10,
                             'input_tokens_details':{'cached_tokens':0}}}})
                self.send_response(200); self.send_header('Content-Type','text/event-stream')
                self.send_header('Content-Length',str(len(data.encode()))); self.end_headers()
                self.wfile.write(data.encode())
            except Exception as exc:
                errors.append(repr(exc)); self.close_connection = True
    server = http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    thread = threading.Thread(target=server.serve_forever,daemon=True); thread.start()
    env = dict(HOME=tmp,TMPDIR=tmp,PATH='/usr/bin:/bin:/usr/sbin:/sbin',
        DSCO_MAX_INPUT_TOKENS='262144',DSCO_CHATGPT_OAUTH_TOKEN='fixture',
        DSCO_REQUEST_TARGET_BYTES=str(target_bytes),
        DSCO_CHATGPT_ACCOUNT_ID='fixture',DSCO_CHATGPT_BASE_URL=f'http://127.0.0.1:{server.server_port}/responses',
        DSCO_DISABLE_SHARED_HOME_OAUTH='1',DSCO_ENV_FILE='/dev/null',DSCO_PRICING_OFFLINE='1',
        DSCO_SECURE_STORE_NO_PROMPT='1',DSCO_DISABLE_DEFAULT_FALLBACKS='1',
        DSCO_AUTO_FALLBACK='0',DSCO_DYNAMIC_FAILOVER='0',DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1',
        DSCO_NO_AUTO_SUPERVISE='1',DSCO_NO_SUPERVISE='1',DSCO_DURABLE_AUTOWAKE='0',
        DSCO_TOOL_PROXY='1',DSCO_TOOL_ALLOWLIST='write_file,context_evict',
        DSCO_AUTO_GOAL='0',DSCO_AUTO_COMPACT='1',DSCO_HARD_TURN_CEILING='60',DSCO_BUDGET='0',
        DSCO_CONTEXT_DB=str(root/'context.db'),DSCO_CHRONICLE_DIR=str(root/'chronicle'),
        DSCO_CHRONICLE_MODE='full-local',DSCO_SYSTEM_PROMPT='Preserve instructions and complete tools.')
    if os.environ.get('DSCO_TEST_USE_DEFAULT_TARGET') == '1':
        env.pop('DSCO_REQUEST_TARGET_BYTES')
    try:
        proc = subprocess.run([str(binary),'--profile','worker','--provider','openai-codex',
            '-m','gpt-6-astra','--trust-tier','trusted','--approval-mode','never',
            '-p','PRESERVE_ROLLING_OBJECTIVE: write the requested files and finish.'],
            cwd=tmp,env=env,capture_output=True,text=True,timeout=150)
        assert not errors, errors
        assert proc.returncode == 0, (proc.returncode,proc.stderr[-3000:])
        assert len(requests) == turns+1, (len(requests),proc.stdout[-1000:],proc.stderr[-1000:])
        assert 'ROLLING_FINISHED' in proc.stdout
        rows=full_events(root)
        budgets=[r['payload'] for r in rows if r.get('event_type')=='request.input_budget']
        archives=[r['payload'] for r in rows if r.get('event_type')=='context.rolling' and r['payload'].get('changed')]
        assert len(budgets)==turns+1 and all(b['admitted'] for b in budgets),budgets
        assert max(b['after_tokens'] for b in budgets)<=262144
        if target_bytes:
            assert len(archives)>=6,len(archives)
            assert max(request_bytes) <= target_bytes, max(request_bytes)
            assert any(b.get('wire_target_triggered') for b in budgets), budgets
            assert all(b.get('wire_target_satisfied') for b in budgets), budgets
        else:
            assert all(not b.get('wire_target_triggered') for b in budgets), budgets
        for i in range(1,turns+1): assert (root/f'{i}.txt').read_text()==payload
        print(json.dumps({'passed':True,'requests':len(requests),'archive_cycles':len(archives),
             'argument_bytes_processed':turns*len(payload),'estimated_argument_tokens_processed':turns*len(payload)//3,
             'max_admitted_tokens':max(b['after_tokens'] for b in budgets),'max_wire_bytes':max(request_bytes),
             'wire_target_bytes':target_bytes,
             'final_observed':True,'real_inference':False}))
    finally:
        server.shutdown(); server.server_close(); thread.join(timeout=3)
