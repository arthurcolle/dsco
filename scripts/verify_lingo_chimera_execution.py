#!/usr/bin/env python3
"""One real guarded inference call through DSCO/Lingo and an isolated Router.

Explicit invocation performs inference using inherited OPENROUTER_API_KEY.
Only the fixed inexpensive model and the supplied source excerpt are sent.
Credentials stay in child environment and an ephemeral encrypted BYOK row.
No automatic retry, provider substitution or service reuse is performed.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
ROUTER = ROOT.parent / 'dsco-router'
DEFAULT_MODEL = 'openai/gpt-4.1-nano'
MAX_TOKENS = 384
CATALOG_URL = 'https://openrouter.ai/api/v1/models'
PROGRAM_PATH = ROOT / 'examples/lingo/chimera-analysis.lingo'
SEED = '''import json,os
from dsco_router.auth.keys import hash_api_key
from dsco_router.auth.credentials import encrypt_credential
from dsco_router.catalog.sync import build_catalog,write_catalog
from dsco_router.control_plane.db import connect
from pathlib import Path
catalog=build_catalog(json.loads(Path(os.environ['RAW_CATALOG']).read_text())['data'])
write_catalog(catalog,Path(os.environ['DSCO_ROUTER_CATALOG_PATH']))
db=connect(os.environ['DSCO_ROUTER_DATABASE_URL'])
db.execute("INSERT INTO organizations(id,name) VALUES ('proof','Isolated execution proof')")
db.execute("INSERT INTO projects(id,org_id,name) VALUES ('proof','proof','Isolated execution proof')")
db.execute("INSERT INTO api_keys(id,org_id,project_id,key_hash,key_prefix,name,scopes_json) VALUES (?,?,?,?,?,?,?)",
 ('proof','proof','proof',hash_api_key(os.environ['LINGO_SEED_KEY']),'isolated','Execution proof',json.dumps(['inference'])))
db.execute("INSERT INTO provider_credentials(id,org_id,project_id,provider,secret_ciphertext,label,source,auth_type,billing_mode) VALUES (?,?,?,?,?,?,?,?,?)",
 ('proof-byok','proof','proof','openrouter',encrypt_credential(os.environ['OPENROUTER_API_KEY']),'Ephemeral BYOK','byok','api_key','direct'))
db.commit();db.close()
'''


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--model',choices=['openai/gpt-4.1-nano','openai/gpt-4.1-mini'],default=DEFAULT_MODEL)
    parser.add_argument('--prior',type=Path)
    parser.add_argument('--report',default=str(ROOT/'reports/lingo-chimera-execution-20260910/live-provider.json'))
    args=parser.parse_args()
    model_id=args.model
    prior=json.loads(args.prior.read_text()) if args.prior else None
    previous=prior['lingo']['value']['result'] if prior else None
    program=PROGRAM_PATH.read_text()
    if not os.getenv('OPENROUTER_API_KEY'):
        raise SystemExit('OPENROUTER_API_KEY must be present; no provider fallback is used')
    sources=[ROOT/'src/lingo_chimera.c',ROOT/'lingo/chimera.lua',ROUTER/'src/dsco_router/routing/plan_contract.py',
             ROUTER/'src/dsco_router/api/openai.py',ROUTER/'src/dsco_router/providers/transport.py',
             ROUTER/'src/dsco_router/routing/chimera.py',PROGRAM_PATH]
    identities={str(path):digest(path) for path in sources}
    source_path=ROUTER/'src/dsco_router/routing/plan_contract.py'
    source=source_path.read_text()
    task=('Review this real Router execution contract. Return only one JSON object with keys summary, checks, risk. '
          'checks must contain exactly two objects, one with symbol="verify_expected", one with symbol="claim_execution", '
          'each with invariant explaining the actual source mechanism. risk must explain one remaining limitation '
          'of at-most-once dispatch versus physical provider completion or price estimates. Do not invent functions. '
          'Do not include markdown. Keep the answer concise. Source follows:\n'+source)
    if previous: task=previous['request']['task']
    assert len(task.encode())<=16384
    report={'scope':'One real upstream completion through isolated authenticated Router and actual DSCO/Lingo',
            'started_at':datetime.now(timezone.utc).isoformat(),'sources':identities,'task_source_sha256':digest(source_path)}
    with tempfile.TemporaryDirectory(prefix='lingo-chimera-real-') as directory:
        work=Path(directory); binary=work/'dsco';shutil.copy2(ROOT/'dsco',binary)
        report['binary_sha256']=digest(binary)
        # Public catalog read, bounded; credentials are not sent to this call.
        with urllib.request.urlopen(CATALOG_URL,timeout=15) as response:
            raw=response.read(8*1024*1024+1)
        assert len(raw)<=8*1024*1024,'catalog size bound'
        catalog=json.loads(raw);model=next(item for item in catalog['data'] if item['id']==model_id)
        pricing=model['pricing'];prompt_price=float(pricing['prompt']);completion_price=float(pricing['completion'])
        # A conservative byte-as-token bound keeps this particular experiment
        # below one cent even though Router planner budgets are estimates.
        estimate=(len(task.encode())+256)*prompt_price+MAX_TOKENS*completion_price
        previous_cost=float(previous['completion']['execution']['measured']['usage']['cost']) if previous else 0.0
        assert prompt_price>=0 and completion_price>=0 and estimate+previous_cost<.01,'current quote exceeds total proof budget'
        report['current_catalog']={'url':CATALOG_URL,'sha256':hashlib.sha256(raw).hexdigest(),
                                  'model':model_id,'pricing':pricing,'byte_token_upper_estimate_usd':estimate,'previous_reported_cost_usd':previous_cost,'experiment_estimate_usd':estimate+previous_cost}
        raw_path=work/'catalog-raw.json';raw_path.write_bytes(raw)
        db=work/'router.sqlite';token=secrets.token_hex(32)
        with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        origin=f'http://127.0.0.1:{port}'
        env={key:os.environ[key] for key in ('PATH','HOME','LANG','TMPDIR') if key in os.environ}
        env.update(PYTHONPATH=str(ROUTER/'src'),DSCO_ROUTER_DATABASE_URL='sqlite:///'+str(db),
                   DSCO_ROUTER_CATALOG_PATH=str(work/'catalog.json'),RAW_CATALOG=str(raw_path),
                   DSCO_ROUTER_SESSION_SECRET=secrets.token_hex(32),DSCO_ROUTER_DEMO_SALT=secrets.token_hex(32),
                   DSCO_ROUTER_CREDENTIAL_ENCRYPTION_KEY=secrets.token_hex(32),LINGO_SEED_KEY=token,
                   OPENROUTER_API_KEY=os.environ['OPENROUTER_API_KEY'],NO_PROXY='127.0.0.1,localhost')
        python=str(ROUTER/'.venv/bin/python')
        seeded=subprocess.run([python,'-c',SEED],cwd=work,env=env,capture_output=True,text=True,timeout=30)
        assert seeded.returncode==0,'isolated Router seed failed (private logs withheld)'
        # The inference key now exists only in the encrypted isolated tenant row.
        env.pop('OPENROUTER_API_KEY');env.pop('LINGO_SEED_KEY')
        logfile=work/'router.log'
        with logfile.open('w') as logs:
            service=subprocess.Popen([python,'-m','uvicorn','dsco_router.app:create_app','--factory','--host','127.0.0.1','--port',str(port),'--log-level','warning'],cwd=work,env=env,stdout=logs,stderr=logs)
            try:
                for _ in range(100):
                    if service.poll() is not None:raise RuntimeError('Router exited before readiness (private logs withheld)')
                    try:
                        with urllib.request.urlopen(origin+'/health',timeout=.3) as response:
                            if response.status==200:break
                    except Exception:time.sleep(.05)
                else:raise RuntimeError('Router did not become ready')
                lingo_env={key:env[key] for key in ('PATH','HOME','LANG','TMPDIR') if key in env}
                lingo_env.update(CHIMERA_HOST=origin,CHIMERA_API_KEY=token,DSCO_ENV_FILE='/dev/null',
                                 DSCO_PRICING_OFFLINE='1',DSCO_SECURE_STORE_NO_PROMPT='1',DSCO_GOV_MODEL='standard',
                                 DSCO_NO_AUTO_SUPERVISE='1',DSCO_ALLOW_RUN='1',DSCO_ALLOW_NET='127.0.0.1')
                executed=subprocess.run([str(binary),'lingo','eval',program,json.dumps({'model':model_id,'task':task,'tokens':MAX_TOKENS,'previous':previous})],cwd=work,env=lingo_env,capture_output=True,text=True,timeout=60)
                try:result=json.loads(executed.stdout)
                except ValueError:raise RuntimeError('Lingo did not return a JSON receipt; no automatic retry') from None
                report['lingo']=result
                assert executed.returncode==0,'Lingo execution failed; receipt retained; no automatic retry'
                value=result['value'];completed=value['result']['completion'];measured=completed['execution']['measured']
                assert result['tool_calls']==2 and completed['execution']['dispatch_attempts']==1
                assert completed['execution']['contract']==value['plan']['decision']['execution_contract']
                assert measured['requested_model']==model_id and measured['provider']=='openrouter'
                # Lingo computes acceptance for retained and new observations
                # in the same typed world. Validation failure never skips traces.
                report['task_validation']=value['current']
                report['previous_validation']=value.get('previous')
                report['execution_verified']=True
                report['same_world_reopened']=value['reopened']==value['current']
                assert report['same_world_reopened']
                if previous:assert value['previous']['accepted'] is False
                try:report['analysis']=json.loads(completed['choices'][0]['message']['content'])
                except ValueError:report['analysis']=None
                plan=value['plan'];request=plan['request']
                wire={'model':'dsco-router/chimera:latest','messages':[{'role':'user','content':request['task']}],
                      'max_tokens':request['max_output_tokens'],'routing_policy':plan['policy'],
                      'orchestration_policy':{k:v for k,v in request.items() if k not in ('task','max_output_tokens')},
                      'expected_plan':plan['decision']['execution_contract']}
                # Replay the same stable ID: server must refuse without another provider dispatch.
                replay=urllib.request.Request(origin+'/v1/chat/completions',data=json.dumps(wire).encode(),
                                              headers={'Authorization':'Bearer '+token,'Content-Type':'application/json'})
                try:
                    with urllib.request.urlopen(replay,timeout=10) as response:raise AssertionError('duplicate execution was not refused')
                except urllib.error.HTTPError as error:
                    denial=json.loads(error.read(4096));assert error.code==409 and denial['error']['code']=='execution_already_submitted'
                    report['replay']={'http_status':error.code,'code':denial['error']['code']}
                conn=sqlite3.connect(db)
                report['fabric']={'runs':conn.execute('SELECT count(*) FROM fabric_runs').fetchone()[0],
                                  'attempts':conn.execute('SELECT count(*) FROM fabric_attempts').fetchone()[0],
                                  'request_status':conn.execute('SELECT status FROM requests').fetchone()[0]}
                conn.close();assert report['fabric']=={'runs':1,'attempts':1,'request_status':'completed'}
                report['passed']=bool(value['current']['accepted'])
            finally:
                service.terminate()
                try:service.wait(timeout=5)
                except subprocess.TimeoutExpired:service.kill();service.wait(timeout=5)
                report['runtime_sources_unchanged']=all(digest(path)==identities[str(path)] for path in sources)
                path=Path(args.report);path.parent.mkdir(parents=True,exist_ok=True)
                path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'passed':report.get('passed',False),'report':args.report,
                      'actual_provider_usage':report.get('lingo',{}).get('value',{}).get('result',{}).get('completion',{}).get('execution',{}).get('measured',{}).get('usage')}))


if __name__=='__main__':main()
