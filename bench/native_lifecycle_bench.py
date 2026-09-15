#!/usr/bin/env python3
"""Paired real-CLI full startup and cleanup; missing prompt prevents inference."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import time


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0))
        return sock.getsockname()[1]


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--before',type=Path,required=True)
    parser.add_argument('--after',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--rounds',type=int,default=5)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    binaries={'before':args.before.resolve(),'after':args.after.resolve()}
    controls=dict(DSCO_PERF='json',DSCO_PRICING_OFFLINE='1',DSCO_NO_AUTO_INTERACTIVE='1',
                  DSCO_TRUST_OPT_OUT='1',DSCO_PEERS=' ')
    env=os.environ.copy();env.update(controls)
    records=[]
    for i in range(args.rounds):
        for label in (['before','after'] if i%2==0 else ['after','before']):
            child_env=env.copy();child_env.update(DSCO_MESH_PORT=str(free_port()),DSCO_HTTP_PORT=str(free_port()))
            argv=[str(binaries[label]),'--profile','full']
            start=time.perf_counter();real_start=time.time()
            p=subprocess.run(argv,env=child_env,stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=15)
            elapsed=time.perf_counter()-start;realtime=time.time()-real_start
            prefix=out/f'{i:02}-{label}'
            prefix.with_suffix('.stdout').write_text(p.stdout);prefix.with_suffix('.stderr').write_text(p.stderr)
            assert p.returncode==1 and 'error: prompt required' in p.stderr,(label,p.returncode)
            assert abs(elapsed-realtime)<1,'suspend or clock jump invalidates timing'
            marks=[]
            for line in p.stderr.splitlines():
                if not line.startswith('{'):continue
                try:value=json.loads(line)
                except ValueError:continue
                if value.get('type')=='startup':marks.append(value)
            ready=next(m['total_ms'] for m in marks if m.get('label')=='startup ready')
            row={'round':i,'variant':label,'argv':argv,'exit_code':p.returncode,
                 'elapsed_ms':elapsed*1000,'realtime_ms':realtime*1000,'clock_gap_ms':(realtime-elapsed)*1000,
                 'startup_ready_ms':ready,'remaining_elapsed_ms':elapsed*1000-ready,
                 'mesh_port':child_env['DSCO_MESH_PORT'],'http_port':child_env['DSCO_HTTP_PORT'],'startup_marks':marks}
            records.append(row);print(json.dumps({k:v for k,v in row.items() if k not in ['argv','startup_marks']}),flush=True)
    summary={}
    for label in binaries:
        rows=[r for r in records if r['variant']==label]
        summary[label]={k:{'median':statistics.median(r[k] for r in rows),'min':min(r[k] for r in rows),'max':max(r[k] for r in rows)}
                        for k in ['elapsed_ms','startup_ready_ms','remaining_elapsed_ms']}
    result={'scope':'real binary --profile full startup and normal atexit cleanup; expected missing-prompt exit1, no inference',
            'controls':controls,'binary_sha256':{k:hashlib.sha256(p.read_bytes()).hexdigest() for k,p in binaries.items()},
            'rounds':args.rounds,'records':records,'summary':summary}
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(summary,indent=2))


if __name__=='__main__':main()
