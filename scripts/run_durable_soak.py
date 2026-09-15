#!/usr/bin/env python3
"""Twenty-minute, bounded live DSCO/planner endurance proof. No inference while idle."""
import argparse, concurrent.futures, hashlib, json, os, re, shutil, subprocess, sys, time
from pathlib import Path


def main():
    p=argparse.ArgumentParser(); p.add_argument('--planner-repo',required=True); p.add_argument('--output',required=True); p.add_argument('--duration',type=float,default=1200); a=p.parse_args()
    sys.path.insert(0,a.planner_repo)
    from cognitive_agent.planning import PlannerStore, StaleAttempt
    from cognitive_agent.executor import execute_one
    root=Path(__file__).resolve().parents[1]; out=Path(a.output).resolve(); out.mkdir(parents=True,exist_ok=True)
    db=out/'planner.sqlite'; binary=out/'dsco-snapshot'
    if db.exists(): raise SystemExit('Refusing to overwrite existing planner state')
    shutil.copy2(root/'dsco', binary)
    start=time.monotonic(); started=time.time(); receipts=[]; expected={}; futures={}; launched=0; peak=0; cancelled=False; old=None; recovered=False; seq=0
    lanes=[('openai','gpt-5.6-luna','low'),('groq','qwen/qwen3.8-27b','none'),('deepseek','deepseek-v4-flash','low')]
    def emit(kind,**data):
        value=dict(event=kind,elapsed_s=time.monotonic()-start,at=time.time(),**data)
        with (out/'events.jsonl').open('a') as f: f.write(json.dumps(value)+'\n')
        if kind!='heartbeat': print(json.dumps(value),flush=True)
    def add(store,id,n,parent=None,deps=(),priority=0):
        nums=[n%17+2,3,n%17+7,1]; answer={'sorted':sorted(nums),'checksum':sum(nums)}
        expected[id]=answer
        store.add_goal(id,f'For the integer list {nums}, return exactly one JSON object with sorted (ascending list) and checksum (sum of the four original integer values). No prose.',parent_id=parent,depends_on=deps,priority=priority,acceptance=['exact JSON values'],max_attempts=1)
    def verify(goal,output):
        clean=re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',output)
        try: value=json.loads(clean.strip())
        except ValueError: return None
        return {'exact JSON values':'Standalone JSON and offline integer/list equality verified: '+json.dumps(value)} if value==expected[goal['id']] else None
    def run(index):
        lane=lanes[index%len(lanes)]
        with PlannerStore(db) as store:
            return execute_one(store,binary=str(binary),provider=lane[0],model=lane[1],effort=lane[2],worker=f'soak-{index}',artifacts=out/'workers',verify=verify,timeout=45,budget=.06,cwd=str(root))
    with PlannerStore(db) as store:
        add(store,'abandoned',99,priority=1000)
        # Explicit fault injection: acquire and abandon a genuine wall-clock lease.
        store.db.execute("UPDATE goals SET max_attempts=2 WHERE id='abandoned'")
        old=store.claim('simulated-crashed-owner',lease_seconds=3)
        store.add_goal('campaign','Deterministic durable workload',kind='group',acceptance=['all child groups verified'])
        store.add_goal('initial','Initial parallel tasks',kind='group',parent_id='campaign',acceptance=['all children verified'])
        add(store,'cancel-live',100,priority=999)
        for i in range(8): add(store,f'initial-{i}',i,parent='initial')
    (out/'manifest.json').write_text(json.dumps({'started_at':started,'duration_s':a.duration,'binary':str(binary),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'lanes':lanes,'max_concurrent':8,'child_budget':.06,'timeout_s':45,'lease_s':60,'known_budget':1.5,'batches_s':[0,30,60,120,300,600,900],'fault_injection':'abandoned wall-clock lease and cancelled live attempt'},indent=2))
    emit('started')
    schedule=[30,60,120,300,600,900]; next_heartbeat=0; next_status=0; cancel_token=None
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        while time.monotonic()-start<a.duration or futures:
            elapsed=time.monotonic()-start
            for future in list(futures):
                if future.done():
                    index=futures.pop(future)
                    try: receipt=future.result()
                    except Exception as exc: receipt={'status':'harness_error','error':repr(exc),'known_accounted_usd':None}
                    if receipt: receipts.append(receipt); emit('worker_finished',worker=index,status=receipt['status'],known_accounted_usd=receipt.get('known_accounted_usd'))
            # Reopen SQLite every scheduler iteration; each worker owns its own connection.
            with PlannerStore(db) as store:
                if old and elapsed>3.2 and not recovered:
                    result=store.recover(); recovered=True
                    try: store.complete(old['id'],old['attempt_token'],'late abandoned result',{'exact JSON values':'injected late result'})
                    except StaleAttempt: emit('abandoned_stale_rejected',recovery=result)
                if not cancelled and peak >= 8:
                    for file in (out/'workers').glob('*/receipt.json') if (out/'workers').exists() else []:
                        try: receipt=json.loads(file.read_text())
                        except (ValueError,OSError): continue
                        if receipt.get('goal_id')=='cancel-live' and receipt.get('status')=='running':
                            cancel_token=receipt['attempt_token']; before=time.monotonic(); store.enqueue_steering('cancel','cancel-live'); store.recover(); cancelled=True
                            try: store.complete('cancel-live',cancel_token,'late cancelled output',{'exact JSON values':'injected stale completion'})
                            except StaleAttempt: emit('cancel_stale_rejected',pid=receipt['pid'],steering_latency_s=time.monotonic()-before)
                            break
                for due in schedule[:]:
                    if elapsed>=due:
                        group=f'batch-{due}'; store.add_goal(group,'Work injected after resident wait',kind='group',parent_id='campaign',acceptance=['all children verified'])
                        add(store,group+'-a',due,parent=group,priority=-10)
                        add(store,group+'-b',due+1,parent=group,deps=(group+'-a',),priority=0)
                        before=time.monotonic(); store.enqueue_steering('reprioritize',group+'-a',priority=100); store.recover()
                        emit('batch_injected',due_s=due,steering_latency_s=time.monotonic()-before); schedule.remove(due)
                store.recover(); snap=store.snapshot()
                for goal in reversed(snap['goals']):
                    if goal['kind']=='group' and goal['status']=='pending':
                        kids=[g for g in snap['goals'] if g['parent_id']==goal['id']]
                        if kids and all(g['status']=='completed' for g in kids) and (goal['id']!='campaign' or not schedule):
                            store.finalize(goal['id'],'All direct children independently verified',{c:'Every child has completed offline validation' for c in goal['acceptance']})
                snap=store.snapshot(); ready=snap['metrics']['ready']
                while ready>len(futures) and len(futures)<8 and elapsed<a.duration and (launched+1)*.06<=1.5 and sum(r.get('known_accounted_usd') or 0 for r in receipts)+.06*(len(futures)+1)<=1.5:
                    futures[pool.submit(run,launched)]=launched; launched+=1
                active=[]
                for f in (out/'workers').glob('*/receipt.json') if (out/'workers').exists() else []:
                    try: r=json.loads(f.read_text())
                    except (ValueError,OSError): continue
                    if r.get('status')=='running' and r.get('pid'):
                        ps=subprocess.run(['ps','-o','pid=,rss=,%cpu=','-p',str(r['pid'])],capture_output=True,text=True).stdout.split()
                        if len(ps)==3: active.append({'pid':int(ps[0]),'rss_kib':int(ps[1]),'cpu_pct':float(ps[2])})
                peak=max(peak,len(active))
                if elapsed>=next_heartbeat:
                    emit('heartbeat',queue=snap['metrics'],active=active,launched=launched,known_accounted_usd=sum(r.get('known_accounted_usd') or 0 for r in receipts),reserved_launch_ceiling_usd=launched*.06); next_heartbeat=elapsed+2
                if elapsed>=next_status:
                    print(json.dumps({'progress_s':elapsed,'queue':snap['metrics'],'active':len(active),'known_usd':sum(r.get('known_accounted_usd') or 0 for r in receipts)}),flush=True); next_status=elapsed+60
            time.sleep(.25)
    with PlannerStore(db) as store:
        snap=store.snapshot(); events=store.events()
    pids=[r['pid'] for r in receipts if r.get('pid')]; survivors=[]
    for pid in pids:
        if subprocess.run(['ps','-o','pid=','-p',str(pid)],capture_output=True,text=True).stdout.strip(): survivors.append(pid)
    costs=[c for r in receipts for c in r.get('cost_records',[])]; known=sum(r.get('known_accounted_usd') or 0 for r in receipts)
    assertions={'duration_at_least_requested':time.monotonic()-start>=a.duration,'peak_eight_live':peak==8,'max_eight_live':peak<=8,'cancel_fenced':cancelled and any(r['status']=='fenced' for r in receipts),'abandoned_recovered':any(e['event']=='lease_recovered' and e['goal_id']=='abandoned' for e in events),'stale_rejected':sum(e['event']=='stale_result_rejected' for e in events)>=2,'all_batches_injected':not schedule,'no_running_goals':snap['metrics']['running']==0,'no_survivors':not survivors,'known_under_budget':known<=1.5,'all_required_work_verified':all(g['status']=='completed' for g in snap['goals'] if g['id']!='cancel-live')}
    summary={'duration_s':time.monotonic()-start,'started_at':started,'finished_at':time.time(),'launched':launched,'receipts':len(receipts),'peak_live':peak,'known_accounted_usd':known,'unpriced_records':sum(r.get('unpriced_records',0) for r in receipts),'incomplete_receipts':sum(bool(r.get('incomplete_cost_receipt')) for r in receipts),'survivors':survivors,'assertions':assertions,'all_acceptance_passed':all(assertions.values()),'metrics':snap['metrics']}
    for name,value in [('summary',summary),('snapshot',snap),('planner-events',events),('receipts',receipts),('cost-records',costs)]: (out/(name+'.json')).write_text(json.dumps(value,indent=2))
    emit('finished',**summary)
    return 0 if all(assertions.values()) else 1

if __name__=='__main__': raise SystemExit(main())
