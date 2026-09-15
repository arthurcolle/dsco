#!/usr/bin/env python3
"""Measure production IPC claims with populated queues and verify their policy."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import sqlite3
import statistics
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--before-source',required=True,type=Path)
    a=p.parse_args(); out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    assert not list(out.glob('*.sqlite')), 'use a fresh directory to retain evidence'
    cc=shlex.split(os.environ.get('CC','cc'))
    evidence={}
    for name,source in [('before',a.before_source.resolve()),('after',ROOT/'src/ipc.c')]:
        command=cc+['-O2','-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L','-I',str(ROOT/'include'),str(ROOT/'bench/ipc_scale_bench.c'),str(source),*[str(ROOT/f'src/{x}.c') for x in ['json_util','env_config','event_loop']],'-lsqlite3','-lm','-o',str(out/name)]
        subprocess.run(command,check=True,timeout=60)
        evidence[name]={'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'compile_argv':command}
    def run(name,mode,path,worker='bench-worker',count=128):
        completed=subprocess.run([str(out/name),mode,str(path),worker,str(count)],capture_output=True,text=True,timeout=45,check=True)
        return json.loads(completed.stdout)
    def clone(source,destination):
        # SQLite's backup API includes committed WAL pages; copying only the
        # main file can silently omit fixture rows while a writer is open.
        src=sqlite3.connect(source); dst=sqlite3.connect(destination)
        try: src.backup(dst)
        finally: dst.close(); src.close()
    records=[]
    for size in [1000,100000]:
        seed=out/f'seed-{size}.sqlite'; run('before','init',seed)
        with sqlite3.connect(seed) as db:
            db.executemany("INSERT INTO tasks(target_agent_id,assigned_to,created_by,priority,status,description,created_at) VALUES(?,?,'setup',?,?, 'unrelated history',?)",((f'other-{i%64}',f'other-{i%64}',i%17,'done' if i%5==0 else 'pending',float(i)) for i in range(size)))
            db.commit()
        for mode in ['targeted','shared','idle']:
            fixture=out/f'fixture-{size}-{mode}.sqlite'; clone(seed,fixture)
            with sqlite3.connect(fixture) as db:
                if mode!='idle':
                    db.executemany("INSERT INTO tasks(target_agent_id,assigned_to,created_by,priority,status,description,created_at) VALUES(?,?,'setup',?,'pending','eligible work',?)",((target,target,(i%5 if target else 100+i%5),float(i%11)) for target in ['bench-worker',''] for i in range(64)))
                    db.commit()
                predicate="target_agent_id='bench-worker'" if mode=='targeted' else "target_agent_id IN ('bench-worker','')"
                expected=[row[0] for row in db.execute("SELECT id FROM tasks WHERE status='pending' AND "+predicate+" ORDER BY CASE WHEN target_agent_id='bench-worker' THEN 0 ELSE 1 END,priority DESC,created_at ASC,id ASC")]
            count=64 if mode=='targeted' else 128
            for repeat in range(2):
                names=['before','after'] if repeat==0 else ['after','before']
                for name in names:
                    path=out/f'{name}-{size}-{mode}-{repeat}.sqlite'; clone(fixture,path)
                    result=run(name,mode,path,count=count)
                    ids=[s['id'] for s in result['samples'] if s['id']]
                    assert ids==expected[:count], (name,mode,'ordering or eligibility mismatch',ids,expected)
                    with sqlite3.connect(path) as db:
                        assert db.execute('PRAGMA integrity_check').fetchone()==('ok',)
                        assert db.execute("SELECT COUNT(*) FROM tasks WHERE description='unrelated history' AND status='pending'").fetchone()[0]==size*4//5
                        plan=list(db.execute("EXPLAIN QUERY PLAN SELECT id FROM tasks WHERE status='pending' AND target_agent_id='bench-worker' ORDER BY priority DESC,created_at ASC LIMIT 1"))
                        pages=db.execute('PRAGMA page_count').fetchone()[0]
                    records.append({'variant':name,'rows_of_unrelated_history':size,'mode':mode,'repeat':repeat,'result':result,'target_probe_plan':plan,'database_pages':pages})
                    (out/'records.json').write_text(json.dumps(records,indent=2)+'\n')
    # Four simultaneous independent processes atomically consume a shared queue.
    race=out/'race.sqlite'; run('after','init',race)
    with sqlite3.connect(race) as db:
        db.executemany("INSERT INTO tasks(target_agent_id,assigned_to,created_by,priority,status,description,created_at) VALUES('','','setup',?,'pending','race',?)",((i%7,float(i)) for i in range(1200)))
        db.commit()
    workers=[subprocess.Popen([str(out/'after'),'race',str(race),f'racer-{i}','2000'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) for i in range(4)]
    try:
        for w in workers: assert w.stdout.readline().strip()=='ready'
        for w in workers: w.stdin.write('g\n'); w.stdin.flush()
        results=[]
        for w in workers:
            stdout,stderr=w.communicate(timeout=30); assert w.returncode==0,stderr
            results.append(json.loads(stdout))
    finally:
        for w in workers:
            if w.poll() is None: w.kill(); w.wait()
    claimed=[s['id'] for result in results for s in result['samples']]
    assert len(claimed)==1200 and len(set(claimed))==1200
    with sqlite3.connect(race) as db:
        assert db.execute("SELECT COUNT(*) FROM tasks WHERE status='done'").fetchone()[0]==1200
    summary={'scope':'Real production SQLite IPC claim latency; WAL and synchronous=NORMAL unchanged; elapsed claims exclude completion, connection startup, fixture population, and model inference.','builds':evidence,'comparisons':[],'verification':{'exact_order_and_target_policy':True,'unrelated_tasks_untouched':True,'sqlite_integrity_checks':True,'competing_processes':4,'unique_completed_claims':1200,'workers_completed':[r['claimed'] for r in results]}}
    for size in [1000,100000]:
        for mode in ['targeted','shared','idle']:
            item={'history_rows':size,'mode':mode}
            for name in ['before','after']:
                vals=[s['ms'] for r in records if r['variant']==name and r['mode']==mode and r['rows_of_unrelated_history']==size for s in r['result']['samples']]
                item[name]={'count':len(vals),'p50_ms':statistics.median(vals),'p95_ms':sorted(vals)[int(.95*(len(vals)-1))]}
            item['median_reduction_percent']=100*(1-item['after']['p50_ms']/item['before']['p50_ms'])
            summary['comparisons'].append(item)
    (out/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,indent=2))

if __name__=='__main__': main()
