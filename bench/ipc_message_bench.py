#!/usr/bin/env python3
"""Paired production IPC message benchmarks with backlog, policy and failure checks."""
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
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--before-source',type=Path,required=True)
    args=parser.parse_args(); out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    assert not list(out.glob('*.sqlite')), 'use a fresh output directory'
    builds={}
    for name,source in [('before',args.before_source.resolve()),('after',ROOT/'src/ipc.c')]:
        command=shlex.split(os.environ.get('CC','cc'))+['-O2','-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L','-I',str(ROOT/'include'),str(ROOT/'bench/ipc_message_bench.c'),str(source),*[str(ROOT/f'src/{x}.c') for x in ['json_util','env_config','event_loop']],'-lsqlite3','-lm','-o',str(out/name)]
        subprocess.run(command,check=True,timeout=60)
        builds[name]={'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'compiler_argv':command}
    def run(name,mode,db,batch=256,rounds=1,identity='recipient'):
        result=subprocess.run([str(out/name),mode,str(db),identity,str(batch),str(rounds)],capture_output=True,text=True,check=True,timeout=30)
        return json.loads(result.stdout)
    def clone(src,dst):
        source=sqlite3.connect(src); target=sqlite3.connect(dst)
        try: source.backup(target)
        finally: target.close(); source.close()
    def insert(db,target,topic,body,created,read=None,sender='sender'):
        db.execute('INSERT INTO messages(from_agent,to_agent,topic,body,created_at,read_at) VALUES(?,?,?,?,?,?)',(sender,target,topic,body,created,read))
    records=[]
    for history in [0,100000]:
        seed=out/f'seed-{history}.sqlite'; run('before','init',seed)
        db=sqlite3.connect(seed)
        with db:
            db.executemany("INSERT INTO messages(from_agent,to_agent,topic,body,created_at,read_at) VALUES('other-sender',?,'other','history',?,?)",((f'other-{i%64}',float(i),1.0 if i%5==0 else None) for i in range(history)))
        db.close()
        for mode in ['recv','topic','inbox']:
            for batch in [1,32,256]:
                count=128 if batch==1 else 2048
                fixture=out/f'fixture-{history}-{mode}-{batch}.sqlite'; clone(seed,fixture)
                db=sqlite3.connect(fixture)
                with db:
                    for i in range(count): insert(db,'recipient' if i%2 else '', 'chosen',str(i),float(i))
                    for i in range(64): insert(db,'recipient','different','filtered',float(count+i),1.0 if mode!='topic' else None)
                condition="read_at IS NULL AND to_agent IN ('recipient','')"+ (" AND topic='chosen'" if mode=='topic' else '')
                order='DESC' if mode=='inbox' else 'ASC'
                expected=[x[0] for x in db.execute(f'SELECT id FROM messages WHERE {condition} ORDER BY created_at {order}')][:count]
                db.close()
                for repeat in range(2):
                    for name in (['before','after'] if repeat==0 else ['after','before']):
                        path=out/f'{name}-{history}-{mode}-{batch}-{repeat}.sqlite'; clone(fixture,path)
                        result=run(name,mode,path,batch,count//batch)
                        messages=[m for b in result['batches'] for m in b['messages']]
                        assert [m['id'] for m in messages]==expected
                        assert all(not m['read'] and m['from']=='sender' and m['to'] in ['recipient',''] for m in messages)
                        db=sqlite3.connect(path)
                        assert db.execute("SELECT COUNT(*) FROM messages WHERE body='history' AND read_at IS NULL").fetchone()[0]==history*4//5
                        assert db.execute("SELECT COUNT(*) FROM messages WHERE topic='chosen' AND read_at IS NOT NULL").fetchone()[0]==count
                        assert db.execute('PRAGMA integrity_check').fetchone()==('ok',)
                        db.close()
                        records.append({'variant':name,'history':history,'mode':mode,'batch':batch,'repeat':repeat,'result':result})
                        (out/'records.json').write_text(json.dumps(records,indent=2)+'\n')
    # A failed ACK retains the row unread, while following rows still commit.
    failure={}
    for name in ['before','after']:
        path=out/f'failure-{name}.sqlite'; run(name,'init',path)
        db=sqlite3.connect(path)
        with db:
            for i,body in enumerate(['first','reject','last']): insert(db,'recipient','chosen',body,float(i))
            db.execute("CREATE TRIGGER reject_ack BEFORE UPDATE OF read_at ON messages WHEN OLD.body='reject' BEGIN SELECT RAISE(ABORT,'rejected ack'); END")
        db.close(); result=run(name,'recv',path)
        db=sqlite3.connect(path); states=list(db.execute('SELECT body,read_at IS NOT NULL FROM messages ORDER BY id')); db.close()
        assert states==[('first',1),('reject',0),('last',1)] and result['total']==3
        failure[name]=states
    invalid=run('after','invalid',out/'failure-after.sqlite')
    # Concurrent sender and two independent directed inboxes; all messages must
    # arrive once, with the correct sender/topic and per-recipient send order.
    path=out/'concurrent.sqlite'; run('after','init',path)
    children=[]
    try:
        for mode,identity in [('live','receiver-a'),('live','receiver-b'),('send','sender')]:
            child=subprocess.Popen([str(out/'after'),mode,str(path),identity],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            children.append(child); assert child.stdout.readline().strip()=='ready'
        for child in children: child.stdin.write('g\n'); child.stdin.flush()
        live=[]
        for child in children:
            stdout,stderr=child.communicate(timeout=20); assert child.returncode==0,stderr
            live.append(json.loads(stdout))
    finally:
        for child in children:
            if child.poll() is None: child.kill(); child.wait()
    ids=[]
    for identity,result in zip(['receiver-a','receiver-b'],live):
        messages=[m for b in result['batches'] for m in b['messages']]
        assert [int(m['body']) for m in messages]==list(range(256))
        assert all(m['from']=='sender' and m['to']==identity and m['topic']=='live' for m in messages)
        ids.extend(m['id'] for m in messages)
    assert len(ids)==len(set(ids))==512 and live[2]['sent']==512
    summary={'scope':'Production message draining/acknowledgement on real SQLite WAL NORMAL, two reversed-order runs per scenario. Payload selection and individual ACK commits unchanged. No inference timing.','builds':builds,'comparisons':[],'verification':{'exact_order_and_delivery':True,'unrelated_backlog_untouched':True,'topic_filter_preserved':True,'ack_failure_isolation':failure,'invalid_arguments':invalid,'concurrent_directed_delivery_unique':512}}
    for history in [0,100000]:
        for mode in ['recv','topic','inbox']:
            for batch in [1,32,256]:
                row={'history':history,'mode':mode,'batch':batch}
                for name in ['before','after']:
                    vals=[b['ms'] for r in records if r['history']==history and r['mode']==mode and r['batch']==batch and r['variant']==name for b in r['result']['batches']]
                    row[name]={'count':len(vals),'p50_ms':statistics.median(vals),'p95_ms':sorted(vals)[int(.95*(len(vals)-1))]}
                row['median_reduction_percent']=100*(1-row['after']['p50_ms']/row['before']['p50_ms'])
                summary['comparisons'].append(row)
    (out/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,indent=2))
if __name__=='__main__': main()
