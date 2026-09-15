#!/usr/bin/env python3
"""Production MCP conformance: competing OS processes, real checkers, no LLM."""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shlex
import sqlite3
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

class Board:
    def __init__(self, binary, root):
        self.binary = str(Path(binary).resolve())
        self.root = Path(root)
        self.path = str(self.root / 'board.sqlite')
        self.env = os.environ.copy()
        for key in list(self.env):
            if key.startswith(('DSCO_ALLOW_', 'DSCO_GOV_', 'DSCO_MCP_', 'DSCO_EXECUTION_')):
                self.env.pop(key)
        self.env.update(DSCO_GOV_MODEL='standard', DSCO_NO_COLOR='1',
                        DSCO_CHRONICLE_DIR=str(self.root / 'chronicle'),
                        DSCO_RUNS_DIR=str(self.root / 'runs'))
        self.trace = []

    def raw(self, arguments, overrides=None):
        init = {'jsonrpc':'2.0','id':1,'method':'initialize','params':{
            'protocolVersion':'2024-11-05','capabilities':{},
            'clientInfo':{'name':'blackboard-conformance','version':'1'}}}
        call = {'jsonrpc':'2.0','id':2,'method':'tools/call',
                'params':{'name':'blackboard','arguments':arguments}}
        proc = subprocess.run([self.binary,'mcp','serve','--toolsets','all','--tier','trusted'],
            input=json.dumps(init)+'\n'+json.dumps(call)+'\n', text=True,
            capture_output=True, env=self.env | (overrides or {}), cwd=self.root, timeout=25)
        rows = []
        for line in proc.stdout.splitlines():
            try: rows.append(json.loads(line))
            except ValueError: pass
        response = next((r for r in rows if r.get('id') == 2), None)
        assert response is not None, (proc.returncode, proc.stdout[-2000:], proc.stderr[-2000:])
        result = response.get('result', {})
        text = '\n'.join(c.get('text','') for c in result.get('content',[]) if c.get('type')=='text')
        error = bool(response.get('error') or result.get('isError'))
        try: data = json.loads(text)
        except ValueError: data = {'text':text, 'rpc_error':response.get('error')}
        self.trace.append({'action':arguments.get('action'),'task':arguments.get('task'),
                           'error':error,'result':data})
        return error, data

    def call(self, action, *, expect_error=False, env=None, **fields):
        error, data = self.raw(dict(action=action,path=self.path,**fields), env)
        assert error == expect_error, (action, fields, error, data)
        return data

    def create(self, task, check, dependencies=()):
        return self.call('create',task=task,title=task,check=check,dependencies=list(dependencies))

    def claim(self, task, owner='worker', ttl=60):
        r = self.call('claim',task=task,owner=owner,ttl=ttl)
        if r['claimed']: r['owner']=owner
        return r

    def owned(self, action, lease, **fields):
        return self.call(action,**{k:lease[k] for k in ('task','generation','owner','token')},**fields)

    def state(self, task):
        return self.call('status',task=task)['tasks'][0]


def checker(code):
    return 'python3 -c ' + shlex.quote(
        'import json,os; s=json.load(open(os.environ["DSCO_BLACKBOARD_SNAPSHOT"])); ' + code)


def wait_for(path, timeout=10):
    until=time.monotonic()+timeout
    while not path.exists():
        assert time.monotonic()<until, f'checker did not reach barrier: {path}'
        time.sleep(.02)


def run(binary, directory):
    b=Board(binary,directory)
    checks=[]
    def passed(label):
        checks.append(label)
        print('PASS:',label,flush=True)

    absent=b.root/'absent.sqlite'
    error,_=b.raw({'action':'status','path':str(absent)})
    assert error and not absent.exists()
    passed('inspection of a missing board does not create it')

    valid=checker('assert s["payload"] == "valid"; print("exact payload passed")')
    assert b.create('evaluator',valid)['created']
    assert not b.create('evaluator',valid)['created']
    b.call('create',task='evaluator',title='changed',check=valid,expect_error=True)
    b.create('search',valid,['evaluator'])
    assert not b.claim('search')['claimed']
    with ThreadPoolExecutor(2) as pool:
        results=list(pool.map(lambda owner:b.claim('evaluator',owner), ['worker-A','worker-B']))
    assert sum(r['claimed'] for r in results)==1
    lease=next(r for r in results if r['claimed'])
    passed('two real MCP processes race: exactly one claims; dependency remains blocked')

    bad=dict(lease,owner='intruder')
    b.owned('publish',bad,payload='valid',expect_error=True)
    bad=dict(lease,token='0'*32)
    b.owned('renew',bad,expect_error=True)
    assert b.owned('renew',lease,ttl=120)['renewed']
    pub=b.owned('publish',lease,payload='invalid')
    assert not b.claim('search')['claimed']
    result=b.call('verify',artifact=pub['artifact'])
    assert not result['passed'] and not result['accepted']
    assert b.state('evaluator')['state']=='candidate'
    b.owned('publish',lease,payload='different',expect_error=True)
    assert b.owned('publish',lease,payload='invalid')['artifact']==pub['artifact']
    passed('wrong owners/tokens rejected; failed real check cannot release dependents; publish retry is idempotent')

    b.call('invalidate',task='evaluator',generation=lease['generation']+1,reason='wrong revision',expect_error=True)
    inv=b.call('invalidate',task='evaluator',generation=lease['generation'],reason='replace failing candidate')
    assert inv['invalidated']==['evaluator','search']
    b.owned('publish',lease,payload='valid',expect_error=True)
    lease=b.claim('evaluator','worker-A')
    pub=b.owned('publish',lease,payload='valid')
    assert pub['sha256']==hashlib.sha256(b'valid').hexdigest()
    verified=b.call('verify',artifact=pub['artifact'])
    assert verified['accepted'] and verified['passed']
    assert b.call('verify',artifact=pub['artifact'])['already_accepted']
    child=b.claim('search','worker-B')
    assert child['inputs']==[{'task':'evaluator','artifact':pub['artifact'],'sha256':pub['sha256']}]
    childpub=b.owned('publish',child,payload='valid')
    assert b.call('verify',artifact=childpub['artifact'])['accepted']
    historical=b.call('status',artifact=pub['artifact'])
    assert historical['payload']=='valid' and historical['checks'][0]['accepted_at_check']
    passed('real verifier accepts exact SHA-256 bytes; next process consumes the pinned accepted input')

    b.call('invalidate',task='evaluator',generation=lease['generation'],reason='contract implementation replaced')
    assert b.state('search')['state']=='pending' and not b.claim('search')['claimed']
    b.call('verify',artifact=childpub['artifact'],expect_error=True)
    assert b.call('status',artifact=childpub['artifact'])['checks'][0]['accepted_at_check']
    passed('upstream invalidation revokes accepted descendants but preserves historical evidence')

    b.create('expiring',valid)
    stale=b.claim('expiring','same-worker',ttl=1)
    time.sleep(1.1)
    b.owned('renew',stale,expect_error=True)
    b.owned('publish',stale,payload='valid',expect_error=True)
    newer=b.claim('expiring','same-worker')
    assert newer['generation']>stale['generation'] and newer['token']!=stale['token']
    b.owned('publish',stale,payload='valid',expect_error=True)
    pub=b.owned('publish',newer,payload='valid')
    assert b.call('verify',artifact=pub['artifact'])['accepted']
    passed('expiry/restart recover work; old attempt with the same owner name is fenced')

    barrier=b.root/'checker-started'; release=b.root/'checker-release'
    slow = "python3 - <<'PY_CHECK'\nimport json,os,time,pathlib\ns=json.load(open(os.environ['DSCO_BLACKBOARD_SNAPSHOT']))\nassert s['payload']=='valid'\n"
    slow += f"pathlib.Path({str(barrier)!r}).touch()\nend=time.monotonic()+10\nwhile not pathlib.Path({str(release)!r}).exists():\n assert time.monotonic()<end\n time.sleep(.02)\nprint('checker passed after barrier')\nPY_CHECK"
    b.create('race-root',valid)
    root=b.claim('race-root'); rootpub=b.owned('publish',root,payload='valid')
    assert b.call('verify',artifact=rootpub['artifact'])['accepted']
    b.create('race-child',slow,['race-root'])
    race=b.claim('race-child'); racepub=b.owned('publish',race,payload='valid')
    with ThreadPoolExecutor(1) as pool:
        checking=pool.submit(b.call,'verify',artifact=racepub['artifact'])
        wait_for(barrier)
        b.call('invalidate',task='race-root',generation=root['generation'],reason='input changed during checker')
        release.touch()
        result=checking.result(timeout=20)
    assert result['passed'] and not result['accepted'] and not result['current']
    assert b.state('race-child')['state']=='pending'
    passed('checker runs without a DB write lock; successful stale check is recorded and rejected')

    b.create('gated',valid)
    gated=b.claim('gated'); gatedpub=b.owned('publish',gated,payload='valid')
    b.call('verify',artifact=gatedpub['artifact'],env={'DSCO_ALLOW_RUN':'0'},expect_error=True)
    b.call('claim',task='gated',owner='x',env={'DSCO_ALLOW_WRITE':'0'},expect_error=True)
    b.call('renew',task='gated',owner=gated['owner'],token=gated['token'],generation=gated['generation'],env={'DSCO_ALLOW_WRITE':b.path},expect_error=True)
    assert b.call('renew',task='gated',owner=gated['owner'],token=gated['token'],generation=gated['generation'],env={'DSCO_ALLOW_WRITE':str(b.root)})['renewed']
    assert b.call('status',task='gated',env={'DSCO_ALLOW_WRITE':'0','DSCO_ALLOW_RUN':'0'})['tasks']
    other=b.root/'allowed';other.mkdir()
    b.call('invalidate',task='gated',generation=gated['generation'],reason='scope probe',env={'DSCO_ALLOW_WRITE':str(other)},expect_error=True)
    # Stored checker text gets a SECOND gate check with the inherited tier.
    b.create('nested-gate', 'printf should-not-run; curl --version')
    n=b.claim('nested-gate'); np=b.owned('publish',n,payload='valid')
    denied=b.call('verify',artifact=np['artifact'],env={'DSCO_ALLOW_NET':'0'})
    assert not denied['accepted'] and not denied['passed'] and 'DSCO_ALLOW_NET' in denied['output']
    passed('live MCP respects read/write/run/path grants and nested checker network denial')

    b.call('create',task='orphan',title='bad',check=valid,dependencies=['absent'],expect_error=True)
    assert not b.call('status',task='orphan')['tasks']
    b.call('create',task='self',title='bad',check=valid,dependencies=['self'],expect_error=True)
    b.call('create',task='duplicate',title='bad',check=valid,dependencies=['gated','gated'],expect_error=True)
    for fields in ({'ttl':0},{'ttl':1.5},{'ttl':True},{'owner':'x\x00y'},{'generation':2}):
        b.call('claim',task='gated',**({'owner':'x'}|fields),expect_error=True)
    b.call('status',task='gated',payload='not allowed',expect_error=True)
    passed('malformed requests and invalid dependency contracts roll back atomically')

    cursor=0; events=[]
    while True:
        page=b.call('events',after=cursor,limit=3)
        events+=page['events'];cursor=page['next_cursor']
        if not page['page_full']:break
    seq=[e['seq'] for e in events]
    assert seq==sorted(set(seq)) and len(events)>20
    assert any(e['kind']=='check_stale' for e in events)
    assert b.call('events',after=cursor)['events']==[]
    with sqlite3.connect(b.path) as db:
        assert db.execute('pragma integrity_check').fetchone()[0]=='ok'
        assert db.execute('pragma foreign_key_check').fetchall()==[]
    task_cursor=0; tasks=[]
    while True:
        page=b.call('status',after=task_cursor,limit=2)
        tasks += [t['task'] for t in page['tasks']]
        task_cursor=page['next_cursor']
        if not page['page_full']: break
    assert len(tasks)==len(set(tasks)) and set(tasks)=={'evaluator','search','expiring','race-root','race-child','gated','nested-gate'}
    passed('bounded task and event cursors survive fresh processes with complete history and database integrity')
    return {'checks':checks,'count':len(checks),'events':len(events),'trace':b.trace}


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',default=str(ROOT/'dsco'))
    parser.add_argument('--report',type=Path)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='dsco-blackboard-test-') as directory:
        report=run(args.binary,directory)
    report['binary_sha256']=hashlib.sha256(Path(args.binary).read_bytes()).hexdigest()
    if args.report:
        args.report.parent.mkdir(parents=True,exist_ok=True)
        args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(f"PASS: {report['count']} blackboard conformance scenarios")

if __name__=='__main__':main()
