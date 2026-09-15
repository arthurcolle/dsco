#!/usr/bin/env python3
"""A tiny airline recovery pipeline using the real DSCO blackboard tool.

Two independent processes publish and verify an evaluator and a cost function.
A third worker consumes their exact accepted versions and searches 125 plans.
This is a coordination demonstration, not a production airline optimizer.
"""
from concurrent.futures import ThreadPoolExecutor
import argparse
import itertools
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT=Path(__file__).resolve().parents[2]
EVALUATOR='''def evaluate(plan):
    if set(plan) != {"F1", "F2", "F3"}: return False
    aircraft, crews = set(), set()
    for leg in plan.values():
        if leg is None: continue
        if not isinstance(leg, dict) or set(leg) != {"aircraft", "crew"}: return False
        a, c = leg["aircraft"], leg["crew"]
        if a not in {"A1", "A2"} or c not in {"C1", "C2"}: return False
        if a in aircraft or c in crews: return False
        aircraft.add(a); crews.add(c)
    return True
'''
COST='''def cost(plan):
    return sum(100 if leg is None else int(leg["aircraft"] == "A2") for leg in plan.values())
'''
CHECK_PREFIX='import json, os\ns=json.load(open(os.environ["DSCO_BLACKBOARD_SNAPSHOT"]))\n'
GOOD={'F1':{'aircraft':'A1','crew':'C1'},'F2':{'aircraft':'A2','crew':'C2'},'F3':None}
EVAL_CHECK=CHECK_PREFIX+f'''ns={{}}; exec(s['payload'],ns)
f=ns['evaluate']
assert f({GOOD!r})
assert not f({{'F1':{{'aircraft':'A1','crew':'C1'}},'F2':{{'aircraft':'A1','crew':'C2'}},'F3':None}})
assert not f({{'F1':{{'aircraft':'A1','crew':'C1'}},'F2':{{'aircraft':'A2','crew':'C1'}},'F3':None}})
assert not f({{}})
assert not f({{'F1':{{'aircraft':'A9','crew':'C1'}},'F2':None,'F3':None}})
print('evaluator: valid plan, aircraft conflict, crew conflict, missing flights, unknown aircraft checked')
'''
COST_CHECK=CHECK_PREFIX+f'''ns={{}}; exec(s['payload'],ns)
f=ns['cost']
assert f({GOOD!r}) == 101
assert f({{'F1':None,'F2':None,'F3':None}}) == 300
print('cost: cancellation and aircraft costs checked')
'''
SEARCH_CHECK=CHECK_PREFIX+'''import itertools
ns={}
assert {i['task'] for i in s['inputs']} == {'evaluator','cost'}
for i in s['inputs']: exec(i['payload'],ns)
r=json.loads(s['payload'])
options=[None]+[{'aircraft':a,'crew':c} for a in ['A1','A2'] for c in ['C1','C2']]
plans=[dict(zip(['F1','F2','F3'],x)) for x in itertools.product(options,repeat=3)]
feasible=[p for p in plans if ns['evaluate'](p)]
assert ns['evaluate'](r['plan'])
assert r['cost'] == ns['cost'](r['plan']) == min(map(ns['cost'],feasible)) == 101
assert r['evaluated'] == len(plans) == 125
print('search: feasible result and global minimum checked against all 125 plans using pinned inputs')
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',default=str(ROOT/'dsco'))
    parser.add_argument('--directory',type=Path,required=True)
    args=parser.parse_args()
    directory=args.directory.resolve();directory.mkdir(parents=True,exist_ok=True)
    path=directory/'board.sqlite'
    if path.exists():parser.error('board already exists; choose a fresh directory to retain prior evidence')
    binary=str(Path(args.binary).resolve())
    env=os.environ.copy()
    env['DSCO_CHRONICLE_DIR']=str(directory/'chronicle')
    env['DSCO_RUNS_DIR']=str(directory/'runs')
    def call(action,**kwargs):
        payload={'action':action,'path':str(path),**kwargs}
        result=subprocess.run([binary,'--tool-exec','blackboard',json.dumps(payload)],
                              text=True,capture_output=True,env=env,cwd=directory,timeout=30)
        try:r=json.loads(result.stdout)
        except ValueError:raise RuntimeError(result.stdout+result.stderr)
        if result.returncode or not r['ok']:raise RuntimeError(r)
        return r['result']
    for task,check,deps in [('evaluator',EVAL_CHECK,[]),('cost',COST_CHECK,[]),('search',SEARCH_CHECK,['evaluator','cost'])]:
        call('create',task=task,title=f'Airline recovery: {task}',check='python3 -c '+shlex.quote(check),dependencies=deps)
    assert not call('claim',task='search',owner='search-worker')['claimed']
    print('Search is blocked until both input artifacts are accepted.',flush=True)
    def publish(task,payload):
        lease=call('claim',task=task,owner=task+'-worker',ttl=120)
        assert lease['claimed']
        candidate=call('publish',task=task,owner=task+'-worker',generation=lease['generation'],token=lease['token'],payload=payload)
        receipt=call('verify',artifact=candidate['artifact'])
        assert receipt['accepted'],receipt
        return candidate,receipt
    with ThreadPoolExecutor(2) as workers:
        evaluator=workers.submit(publish,'evaluator',EVALUATOR)
        cost=workers.submit(publish,'cost',COST)
        producers={'evaluator':evaluator.result(),'cost':cost.result()}
    print('Evaluator and cost accepted in independent worker processes.',flush=True)
    lease=call('claim',task='search',owner='search-worker',ttl=120)
    assert lease['claimed']
    ns={}
    for ref in lease['inputs']:
        artifact=call('status',artifact=ref['artifact'])
        assert artifact['sha256']==ref['sha256']
        exec(artifact['payload'],ns)
    options=[None]+[{'aircraft':a,'crew':c} for a in ['A1','A2'] for c in ['C1','C2']]
    plans=[dict(zip(['F1','F2','F3'],x)) for x in itertools.product(options,repeat=3)]
    feasible=[p for p in plans if ns['evaluate'](p)]
    best=min(feasible,key=ns['cost'])
    solution={'plan':best,'cost':ns['cost'](best),'evaluated':len(plans)}
    published=call('publish',task='search',owner='search-worker',generation=lease['generation'],token=lease['token'],payload=json.dumps(solution,sort_keys=True))
    verified=call('verify',artifact=published['artifact'])
    assert verified['accepted'],verified
    evidence={'solution':solution,'inputs':lease['inputs'],'producers':producers,
              'search':{'publication':published,'verification':verified},
              'status':call('status'),'events':call('events',limit=100)}
    (directory/'result.json').write_text(json.dumps(evidence,indent=2)+'\n')
    print(json.dumps(solution,indent=2))
    print(f'Accepted result, pinned input versions, check receipts, and event history: {directory / "result.json"}')
    print(f'Durable board: {path}')

if __name__=='__main__':main()
