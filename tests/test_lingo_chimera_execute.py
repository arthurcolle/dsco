#!/usr/bin/env python3
"""Native Lingo execution contract tests. HTTP fixtures never perform inference."""
import argparse
import copy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

from test_lingo_systems import route_fixture

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = {"format": "dsco.chimera.direct/1", "execution_id": "11111111-2222-4333-8444-555555555555",
            "request_sha256": "a" * 64, "plan_sha256": "b" * 64}
PLAN = "local l=require('lingo');local c=require('lingo.chimera');local p=c.preferences{allowed_models={'fixture/model'}}:plan{task='Explain consumed execution IDs',strategy='direct',max_calls=1,max_output_tokens=128,max_expected_cost_usd=.01,max_worst_case_cost_usd=.01};"


def completion():
    return {"id": "provider-fixture", "model": "dsco-router/chimera:latest",
            "choices": [{"message": {"role": "assistant", "content": "Execution IDs prevent duplicate dispatch."}}],
            "chimera": {"selected_model": "fixture/model"},
            "execution": {"contract": copy.deepcopy(CONTRACT), "status": "completed", "dispatch_attempts": 1,
                          "router_retries": 0, "provider_fallbacks": False,
                          "budget_basis": "planner_estimate_not_billing_guarantee",
                          "measured": {"provider": "openrouter", "requested_model": "fixture/model", "reported_model": "fixture/model-revision",
                                       "provider_response_id": "provider-fixture", "usage": {"prompt_tokens": 20, "completion_tokens": 10, "cost": .000006},
                                       "elapsed_ms": 42, "run_id": "fixture-run", "attempt_id": "fixture-attempt", "request_id": "fixture-request"}}}


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--binary', default=str(ROOT / 'dsco'))
    parser.add_argument('--report', default=str(ROOT / 'reports/lingo-chimera-execution-20260910/native-fixtures.json'))
    args = parser.parse_args(); calls = []; mode = ['success']; checks = []
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['content-length']))); calls.append((self.path, body))
            status = 200
            if self.path == '/v1/route':
                data = route_fixture(); data['execution_contract'] = CONTRACT
            elif self.path == '/v1/chat/completions':
                data = completion()
                if mode[0] == 'mismatch': data['execution']['contract']['plan_sha256'] = 'c' * 64
                elif mode[0] == 'model_mismatch': data['execution']['measured']['requested_model'] = 'other/model'
                elif mode[0] == 'extra_attempt': data['execution']['dispatch_attempts'] = 2
                elif mode[0] == 'malformed': data = {'error': 'not a completion'}
                elif mode[0] == 'changed': status, data = 409, {'error': {'code': 'plan_changed', 'message': 'changed'}}
                elif mode[0] == 'uncertain': status, data = 504, {'error': {'code': 'upstream_error'}}
                elif mode[0] == 'usage_unknown': data['execution']['measured']['usage'] = None
                elif mode[0] == 'too_large': data['padding'] = 'x' * 131072
            else: status, data = 404, {}
            raw = json.dumps(data).encode(); self.send_response(status)
            self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(raw)))
            self.end_headers(); self.wfile.write(raw)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='lingo-chimera-native-') as directory:
            env = {'PATH': os.environ.get('PATH','/usr/bin:/bin'), 'DSCO_ENV_FILE':'/dev/null',
                   'DSCO_PRICING_OFFLINE':'1','DSCO_SECURE_STORE_NO_PROMPT':'1','DSCO_GOV_MODEL':'standard',
                   'DSCO_NO_AUTO_SUPERVISE':'1','DSCO_NO_COLOR':'1', 'DSCO_ALLOW_RUN':'1',
                   'CHIMERA_HOST':f'http://127.0.0.1:{server.server_port}', 'CHIMERA_API_KEY':'fixture'}
            def invoke(source, changes=None):
                proc = subprocess.run([args.binary,'lingo','eval',source],cwd=directory,env=env | (changes or {}),capture_output=True,text=True,timeout=20)
                assert proc.returncode == 0, (proc.stdout,proc.stderr)
                return json.loads(proc.stdout)['value']
            def check(name, function):
                calls.clear(); mode[0] = name
                function(); checks.append(name); print('PASS',name,flush=True)
            def succeeds():
                result=invoke(PLAN+'return p:execute()')
                assert result['executed'] is True and result['completion']['execution']['measured']['elapsed_ms']==42
                assert [path for path,_ in calls]==['/v1/route','/v1/chat/completions']
                assert calls[1][1]['expected_plan']==CONTRACT
                assert calls[0][1]=={k:v for k,v in calls[1][1].items() if k!='expected_plan'}
            check('success',succeeds)
            def immutable_capture():
                invoke(PLAN+"p.request.task='tampered';p.policy.cost_weight=999;return p:execute()")
                assert calls[1][1]['messages'][0]['content']=='Explain consumed execution IDs'
                assert calls[1][1]['routing_policy']['cost_weight']!=999
            check('private_capture',immutable_capture)
            def single_attempt():
                result=invoke(PLAN+'p:execute();local ok,e=l.try(function() return p:execute() end);return {ok=ok,error=e}')
                assert result['ok'] is False and 'already attempted' in result['error'] and len(calls)==2
            check('single_attempt',single_attempt)
            def scenario_does_not_consume():
                result=invoke(PLAN+"local w=l.world{id='pure-attempt'};local ok,e=l.try(function() return w:scenario('hypothetical',function() return p:execute() end) end);assert(not ok);return p:execute()")
                assert result['executed'] is True and len(calls)==2
            check('scenario_rejection_does_not_consume',scenario_does_not_consume)
            def rejects(expected):
                result=invoke(PLAN+'local ok,e=l.try(function() return p:execute() end);return {ok=ok,error=e}')
                assert result['ok'] is False and expected in result['error'], result
                assert len(calls)==2
            for name in ('mismatch','model_mismatch','extra_attempt','malformed','uncertain','too_large'):
                check(name,lambda:rejects('outcome_unknown'))
            check('changed',lambda:rejects('plan_changed'))
            def unknown_usage():
                result=invoke(PLAN+'return p:execute()')
                assert result['completion']['execution']['measured']['usage'] is None
            check('usage_unknown',unknown_usage)
            def scope_denied():
                result=invoke("local l=require('lingo');local ok,e=l.try(function() "+PLAN+"return p:execute() end);return {ok=ok,error=e}",{'DSCO_ALLOW_NET':'allowed.invalid'})
                assert result['ok'] is False and not calls
            check('destination_scope_denied',scope_denied)
            def overrides_rejected():
                result=invoke(PLAN+"local ok,e=l.try(function() return p:execute{task='other'} end);return {ok=ok,error=e}")
                assert result['ok'] is False and 'no arguments' in result['error'] and len(calls)==1
            check('execution_overrides_rejected',overrides_rejected)
    finally:
        server.shutdown(); server.server_close(); thread.join(timeout=2)
    path=Path(args.report);path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps({'scope':'Native binary with counted HTTP fixtures; no inference','passed':len(checks),'checks':checks},indent=2)+'\n')
    print(str(path))


if __name__=='__main__':main()
