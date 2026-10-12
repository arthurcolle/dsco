#!/usr/bin/env python3
"""Real elapsed-time ladder, streamed receipts, frozen executables, local work."""
import argparse, asyncio, hashlib, json, os, shutil, sys, tempfile, time, shlex
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
DEFAULT=[1,5,15,35,75,150,300]
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
async def main(args):
    durations=[round(float(x)*1000) for x in args.seconds.split(',')]
    assert durations and durations==sorted(set(durations)) and min(durations)>0 and max(durations)<=3600000
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=True)
    assert not (out/'manifest.json').exists(),'Use a new output directory for each immutable experiment'
    frozen=out/'executables';frozen.mkdir()
    for name,path in {'dsco':Path(args.binary).resolve(strict=True),'core':ROOT/'build/long-cycles/core','core-poll':ROOT/'build/long-cycles/core-poll','mcp':ROOT/'build/long-cycles/mcp','job-wait':ROOT/'build/long-cycles/job-wait'}.items():shutil.copy2(path,frozen/name)
    sources=['src/mcp.c','src/cancel_wait.c','src/tools.c','src/async_runtime.c','src/async_join.c','src/scheduler.c','src/scheduler_lifetime.c','src/scheduler_io.c','src/event_loop.c','src/shell_jobs.c','src/shell_job_events.c','tests/harness_long_cycles.c','tests/mcp_long_cycles_fixture.c','tests/cancel_wait_measured.c','tests/long_job_wait.c','tests/test_harness_long_cycles.py']
    manifest={'schema':'dsco.long_cycles.v1','durations_ms':durations,'repeat':args.repeat,'executables':{p.name:digest(p) for p in frozen.iterdir()},'sources':{p:digest(ROOT/p) for p in sources},'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'local_only':True,'provider_requests':0,'concurrent_lanes':8,'fixture_watchdog_seconds':sum(durations)*args.repeat/1000+90}
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    records=[];ledger=(out/'results.jsonl').open('x');started=time.monotonic()
    def record(row):
        records.append(row);ledger.write(json.dumps(row,sort_keys=True)+'\n');ledger.flush();os.fsync(ledger.fileno());print(json.dumps(row),flush=True)
    async def run_lane(name,command):
        p=await asyncio.create_subprocess_exec(*map(str,command),stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE,start_new_session=True)
        async def consume():
            async for line in p.stdout:
                row=json.loads(line);assert row['passed'];row['profile']=name;record(row)
            rc=await p.wait();stderr=(await p.stderr.read()).decode(errors='replace');assert rc==0,(name,rc,stderr)
        try:await asyncio.wait_for(consume(),manifest['fixture_watchdog_seconds'])
        finally:
            if p.returncode is None:p.terminate();await p.wait()
    async def jobs():
        with tempfile.TemporaryDirectory(prefix='dsco-long-cycle-jobs-') as home:
            home=Path(home);binary=str(frozen/'dsco');env={k:v for k,v in os.environ.items() if not k.startswith(('DSCO_','TOOLS_API_'))}
            env.update(HOME=str(home),DSCO_NO_AUTO_SUPERVISE='1',DSCO_PRICING_OFFLINE='1',DSCO_SECURE_STORE_NO_PROMPT='1',DSCO_GOV_MODEL='standard')
            active=[]
            async def call(data):
                p=await asyncio.create_subprocess_exec(binary,'--tool-exec-raw','bash',json.dumps(data),cwd=home,env=env,stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE)
                try:stdout,stderr=await asyncio.wait_for(p.communicate(),20)
                finally:
                    if p.returncode is None:p.terminate();await p.wait()
                assert p.returncode==0,(p.returncode,stdout[-1500:],stderr[-1500:]);return json.loads(stdout)
            try:
                for repetition in range(args.repeat):
                    for duration in durations:
                        marker=f'OWNED_LONG_CYCLE_{repetition}_{duration}'
                        script=f'import time,sys;time.sleep({duration/1000!r});sys.stdout.write({marker!r})'
                        command=shlex.join([sys.executable,'-c',script]);start=time.monotonic()
                        job=await call({'command':command,'run_in_background':True,'timeout_seconds':0});jid=job['job_id'];active.append(jid)
                        assert job['execution_timeout_seconds']==0 and job['deadline_policy']=='completion'
                        # A waiting client is dispensable; operation ownership survives.
                        client=await asyncio.create_subprocess_exec(binary,'--tool-exec-raw','bash',json.dumps({'action':'wait','job_id':jid,'wait_seconds':30}),cwd=home,env=env,stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE)
                        await asyncio.sleep(.12) # one scheduled fault, no polling loop
                        if client.returncode is None:client.terminate()
                        await asyncio.wait_for(client.communicate(),5)
                        p=await asyncio.create_subprocess_exec(str(frozen/'job-wait'),str(home/'.dsco/shell-jobs'/jid),str(job['supervisor_pid']),stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE)
                        try:stdout,stderr=await asyncio.wait_for(p.communicate(),duration/1000+20)
                        finally:
                            if p.returncode is None:p.terminate();await p.wait()
                        assert p.returncode==0,(stdout,stderr);observation=json.loads(stdout)
                        terminal=await call({'action':'output','job_id':jid});active.remove(jid)
                        assert terminal['status']=='completed' and terminal['output']==marker and terminal['execution_timeout_seconds']==0,terminal
                        assert terminal['event_waits']<=12 and observation['kernel_waits']<=32
                        record({'profile':'frozen-cli-durable-jobs','lane':'durable-job','duration_ms':duration,'repetition':repetition,'elapsed_ms':(time.monotonic()-start)*1000,'owner_cpu_ms':observation['owner_cpu_ms'],'kernel_waits':observation['kernel_waits'],'supervisor_waits':terminal['event_waits'],'observer_disconnected':True,'output_sha256':hashlib.sha256(marker.encode()).hexdigest(),'job_id':jid,'passed':True})
            finally:
                for jid in active:
                    try:await call({'action':'cancel','job_id':jid})
                    except Exception:pass
    schedule=[str(d) for _ in range(args.repeat) for d in durations]
    tasks=[run_lane(f'{backend}-{lane}',[frozen/backend,lane,*schedule]) for backend in ('core','core-poll') for lane in ('async','scheduler','timer')]
    tasks+=[run_lane('mcp-stdio',[frozen/'mcp',*schedule]),jobs()]
    error=None
    try:
        async with asyncio.TaskGroup() as group:
            for task in tasks:group.create_task(task)
    except BaseException as e:error=repr(e)
    finally:ledger.close()
    assert all(digest(frozen/k)==v for k,v in manifest['executables'].items())
    expected=8*len(durations)*args.repeat
    summary={'passed':error is None and len(records)==expected,'error':error,'expected_records':expected,'verified_records':len(records),'longest_verified_cycle_ms':max((r['duration_ms'] for r in records),default=0),'elapsed_seconds':time.monotonic()-started,'results_sha256':digest(out/'results.jsonl'),'manifest_sha256':digest(out/'manifest.json'),'limits':['Controlled local waits and owned processes; no inference quality or remote service reliability proof','Real elapsed times; no time acceleration','No claim of a globally optimal harness','Fixture watchdogs stop failed experiments; explicit runtime execution policies unchanged']}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary),flush=True)
    assert summary['passed'],summary
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--binary',default='./dsco');p.add_argument('--seconds',default=','.join(map(str,DEFAULT)));p.add_argument('--repeat',type=int,default=1);p.add_argument('--output',required=True);a=p.parse_args();assert a.repeat>0;asyncio.run(main(a))
