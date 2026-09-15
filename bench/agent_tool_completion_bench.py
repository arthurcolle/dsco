#!/usr/bin/env python3
"""Pair production agent worker/collector control around real local file reads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def function(source, name):
    match = re.search(r'^static [^\n;]*\b' + name + r'\([^;{]*\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}\n', match.start()) + 3]


def unit(path):
    source = path.read_text()
    types = source[source.index('#define CONCURRENT_TOOL_MAX'):source.index('static double now_ms(void);')]
    worker = function(source, 'concurrent_tool_thread')
    launch_begin = source.index('for (int ci = 0; ci < conc_count; ci++)', source.index('/* ── Launch all concurrent'))
    launch = source[launch_begin:source.index('/* ── Execute serial', launch_begin)]
    collect_begin = source.index('bool conc_collected[CONCURRENT_TOOL_MAX]')
    sink_begin = source.index('content_block_t *blk =', collect_begin)
    tail_begin = source.index('free(s->result);', sink_begin)
    collect_end = source.index('if (conc_count > 0 && !pixel_tui_session_active())', tail_begin)
    collector = source[collect_begin:sink_begin] + 'bench_collect(s, ci);\n' + source[tail_begin:collect_end]
    modern = 'concurrent_tool_completion_t' in types
    helpers = (function(source, 'concurrent_tool_done') + function(source, 'concurrent_tool_wait')) if modern else ''
    tools = (ROOT / 'src/tools.c').read_text()
    wd = tools[tools.index('_Thread_local volatile int tl_tool_cancelled = 0;'):tools.index('/* Per-tool timeout overrides')]
    template = (ROOT / 'bench/agent_tool_completion_bench.c').read_text()
    parts = {'@TYPES@': types, '@WATCHDOG@': wd, '@WORKER@': worker, '@HELPERS@': helpers,
             '@LAUNCH@': launch, '@COLLECTOR@': collector}
    for marker, code in parts.items(): template = template.replace(marker, code)
    return ('#define COMPLETION_CONDITION 1\n' if modern else '') + template, {k: hashlib.sha256(v.encode()).hexdigest() for k,v in parts.items()}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--before-source', type=Path, required=True)
    p.add_argument('--pairs', type=int, default=30)
    a = p.parse_args(); out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    builds = {}
    paths = {'before': a.before_source.resolve(), 'after': ROOT / 'src/agent.c'}
    for name, path in paths.items():
        code, hashes = unit(path); cpath = out / f'{name}.c'; cpath.write_text(code)
        command = ['cc', '-O2', '-g', '-std=c11', '-D_DARWIN_C_SOURCE', '-pthread',
                   '-I', str(ROOT/'include'), str(cpath), str(ROOT/'src/tool_telemetry.c'),
                   str(ROOT/'src/capability.c'), str(ROOT/'src/json_util.c'), str(ROOT/'src/env_config.c'), '-o', str(out/name)]
        subprocess.run(command, check=True, timeout=30)
        builds[name] = {'source': str(path), 'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                        'production_section_hashes': hashes, 'compiler_argv': command}
    fixture = out / 'fixture.txt'; fixture.write_text('local-file-read-proof:' + 'q'*4096)
    env = dict(os.environ)
    for key in list(env):
        if key.startswith('DSCO_ALLOW_'): env.pop(key)
    samples = []
    for count, delay in [(2,0),(8,0),(32,0),(8,1000),(8,20000)]:
        for pair in range(a.pairs):
            for name in (['before','after'] if pair%2==0 else ['after','before']):
                run = subprocess.run([str(out/name),str(fixture),str(count),str(delay),'sample'],
                                     text=True, capture_output=True, check=True, env=env, timeout=5)
                samples.append({'variant':name,'count':count,'delay_us':delay,'pair':pair,**json.loads(run.stdout)})
    scenarios = []
    for count,delay in [(2,0),(8,0),(32,0),(8,1000),(8,20000)]:
        row={'count':count,'delay_us':delay}
        for name in paths:
            values=[s for s in samples if s['count']==count and s['delay_us']==delay and s['variant']==name]
            row[name]={metric+'_p50':statistics.median(v[metric] for v in values)
                       for metric in ['total_ms','collection_tail_ms','cpu_ms']}
            row[name]['total_ms_p95']=sorted(v['total_ms'] for v in values)[int(.95*len(values))-1]
        row['median_reduction_percent']=100*(1-row['after']['total_ms_p50']/row['before']['total_ms_p50'])
        scenarios.append(row)
    command = builds['after']['compiler_argv'].copy()
    command[command.index('-O2')] = '-O1'
    command.insert(1,'-fsanitize=address,undefined')
    command[-1] = str(out/'regressions')
    subprocess.run(command,check=True,timeout=30)
    checks=[]
    try:
        subprocess.run([str(out/'before'),str(fixture),'8','0','thread_failure'],
                       capture_output=True,text=True,env=env,timeout=.3,check=True)
    except subprocess.TimeoutExpired:
        checks.append({'mode':'before_thread_creation_failure','expected_hang_reproduced':True,'bounded_timeout_seconds':.3})
    else:
        raise AssertionError('baseline unexpectedly handled failed worker creation')
    for mode in ['precompleted','spurious','interrupt','denied','timeout','thread_failure','mixed_failure','read_write_disabled','race']:
        run=subprocess.run([str(out/'regressions'),str(fixture),'8','0',mode],capture_output=True,text=True,
                           check=True,env=env,timeout=25)
        checks.append({'mode':mode,**json.loads(run.stdout)})
    (out/'samples.json').write_text(json.dumps(samples,indent=2)+'\n')
    result={'passed':True,'pairs_per_scenario':a.pairs,'builds':builds,'scenarios':scenarios,'checks':checks,
            'scope':'Exact production agent worker, watchdog, launch, and collector control; collection UI/cache/Chronicle body replaced by assertions. Tool dispatch boundary uses real capability.c then bounded local file IO. Headless -p scheduling and model time are not measured. No root build or model calls.'}
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'passed':True,'scenarios':scenarios,'checks':checks},indent=2))

if __name__ == '__main__': main()
