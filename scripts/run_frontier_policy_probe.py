#!/usr/bin/env python3
"""One bounded live selection plus no-inference rejection checks over native MCP."""
import argparse,copy,json,os,queue,re,signal,subprocess,threading,time
from pathlib import Path

def main():
    p=argparse.ArgumentParser();p.add_argument('--policy',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--binary',type=Path,required=True);a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=False);env=os.environ.copy()
    for k in list(env):
        if k.startswith('DSCO_'):env.pop(k)
    env.update(DSCO_BUDGET='.10',DSCO_CHILD_BUDGET='.05',DSCO_TOOL_CHOICE='none',DSCO_HARD_TURN_CEILING='1',DSCO_MAX_TOKENS='100',DSCO_ALLOW_RUN='1',DSCO_ALLOW_WRITE='0',DSCO_ALLOW_NET='1',DSCO_ALLOW_SECRETS='0',DSCO_ALLOW_CONTROL='0',DSCO_DISABLE_DEFAULT_FALLBACKS='1',DSCO_AUTO_FALLBACK='0',DSCO_MCP_SERVER='',DSCO_MCP_SERVERS='',DSCO_MCP_HEADLESS='0',DSCO_COST_LEDGER_PATH=str(a.output.resolve()/'costs.jsonl'),DSCO_CHRONICLE_MODE='off',DSCO_RUNS_DIR=str(a.output.resolve()/'runs'))
    log=(a.output/'stderr.log').open('w');proc=subprocess.Popen([str(a.binary.resolve()),'mcp','serve','--toolsets','all','--tier','trusted'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True,env=env,start_new_session=True)
    q=queue.Queue();threading.Thread(target=lambda:[q.put(x) for x in proc.stdout],daemon=True).start();seq=0;events=[]
    def call(method,params):
        nonlocal seq
        seq+=1;proc.stdin.write(json.dumps({'jsonrpc':'2.0','id':seq,'method':method,'params':params})+'\n');proc.stdin.flush();deadline=time.monotonic()+45
        while time.monotonic()<deadline:
            try:d=json.loads(q.get(timeout=1))
            except queue.Empty:continue
            if d.get('id')==seq:
                events.append({'method':method,'params':params,'response':d});(a.output/'rpc.json').write_text(json.dumps(events,indent=2));return d
        raise TimeoutError(method)
    def swarm(args):
        d=call('tools/call',{'name':'swarm','arguments':args});r=d.get('result',{})
        text=''.join(x.get('text','') for x in r.get('content',[]));return bool(r.get('isError') or d.get('error')),json.loads(text)
    try:
        call('initialize',{'protocolVersion':'2024-11-05','capabilities':{},'clientInfo':{'name':'frontier-policy-proof','version':'1'}})
        spec=json.loads(a.policy.read_text());spec['task']='For the integer list [8, 3, 1, 5], return exactly one standalone JSON object with sorted (ascending list) and checksum (sum). No prose, commands, code fences, or outer array.'
        rejected=[]
        for label,modify in [('expired',lambda s:s['frontier_policy'].update(expires_at=1)),('coverage',lambda s:s.update(one_per_provider=True)),('wrong_workload',lambda s:s.update(workload='unrelated')),('unpriced',lambda s:[l.update(unpriced_attempts=1) for l in s['frontier_policy']['lanes']])]:
            bad=copy.deepcopy(spec);modify(bad);err,result=swarm(bad);assert err,(label,result);rejected.append({'case':label,'result':result})
        err,created=swarm(spec);assert not err,created
        gid=created['group_id'];err,result=swarm({'action':'collect','group_id':gid,'timeout':30});assert not err and result.get('complete'),result
        assert len(result['results'])==1 and created['agents_spawned']==1
        child=result['results'][0];assert child['exit_code']==0 and child['provider']==created['lanes'][0]['provider'] and child['model']==created['lanes'][0]['model']
        lines=re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',child['output']).splitlines()
        while lines and (not lines[0].strip() or lines[0].lstrip().startswith(('↻ autonomy active','usage:'))):lines.pop(0)
        actual=json.loads('\n'.join(lines));assert actual=={'sorted':[1,3,5,8],'checksum':17}
        (a.output/'summary.json').write_text(json.dumps({'rejections':rejected,'created':created,'collected':result,'independent_validation':{'passed':True,'exact_result':actual}},indent=2))
        print(json.dumps({'created':created,'collected':result}))
    finally:
        proc.stdin.close()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=5)
        log.close()

if __name__=='__main__':main()
