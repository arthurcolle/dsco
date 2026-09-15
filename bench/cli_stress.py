#!/usr/bin/env python3
"""Repeated live CLI timing with deterministic grading and immutable run records."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import unicodedata

import cli_compare as base

TASKS = dict(base.TASKS)
TASKS['aggregate_records'] = {
    'stratum': 'runtime smoke: read, compute, write and verify',
    'files': {'records.json': json.dumps([
        {'category': 'abc'[i % 3], 'amount': (i * 17) % 101 - 50}
        for i in range(120)
    ]) + '\n'},
    'prompt': '''Read records.json in the current directory. Create totals.json
containing one object per category, keyed by category, with integer count and sum
fields computed from the records. Use local tools to read back and verify the
written file. Finish with a brief confirmation. No other deliverables are needed.''',
    'grader': r'''
import json
from collections import defaultdict
with open('records.json') as f: records=json.load(f)
expected=defaultdict(lambda: {'count':0,'sum':0})
for record in records:
 expected[record['category']]['count']+=1
 expected[record['category']]['sum']+=record['amount']
with open('totals.json') as f: actual=json.load(f)
print(json.dumps({'passed':int(actual==dict(expected)),'total':1,'errors':[] if actual==dict(expected) else ['incorrect category totals']}))
'''}
TASKS['intervals'] = {
    'stratum': 'holdout: integer interval set operations',
    'files': {'intervals.py': 'def subtract(source, cuts):\n    raise NotImplementedError\n'},
    'prompt': '''Implement subtract(source, cuts) in intervals.py using standard Python only.
Inputs are iterables of integer endpoint pairs representing half-open intervals
[start,end). Consume each iterable once. Ignore empty intervals; raise ValueError
for start>end. Return the sorted canonical list of disjoint (start,end) tuples
representing union(source) minus union(cuts), merging adjacent output intervals.
Accept unsorted, duplicate, overlapping, adjacent and negative intervals; do not
mutate inputs. Endpoints may exceed 64 bits. Do not enumerate points in intervals.
Implement the function on disk, add and run proportionate tests, and finish.''',
    'grader': r'''
import json,random,copy
from intervals import subtract
passed=total=0;errors=[]
def check(source,cuts,expected=None,iterator=False):
 global passed,total
 total+=1;before=copy.deepcopy((source,cuts))
 if expected is None:
  points=set()
  for a,b in source:points.update(range(a,b))
  for a,b in cuts:points.difference_update(range(a,b))
  expected=[]
  for x in sorted(points):
   if expected and expected[-1][1]==x:expected[-1]=(expected[-1][0],x+1)
   else:expected.append((x,x+1))
 try:
  result=subtract(iter(source) if iterator else source,iter(cuts) if iterator else cuts)
  ok=result==expected and (iterator or (source,cuts)==before)
 except Exception:ok=False
 passed+=bool(ok)
 if not ok and len(errors)<8:errors.append(repr((source,cuts,expected)))
for source,cuts in [([],[]),([(0,9)],[(2,3),(6,8)]),([(-3,0),(0,4)],[(1,2)]),([(0,0)],[]),([(1,2)],[(0,3)])]:
 check(source,cuts);check(source,cuts,iterator=True)
rng=random.Random(88331)
for i in range(120):
 source=[tuple(sorted((rng.randrange(-20,20),rng.randrange(-20,20)))) for _ in range(rng.randrange(12))]
 cuts=[tuple(sorted((rng.randrange(-20,20),rng.randrange(-20,20)))) for _ in range(rng.randrange(12))]
 check(source,cuts);check(source,cuts,iterator=True)
n=10**100
check([(-n,n)],[(0,n-1)],[(-n,0),(n-1,n)])
for source,cuts in [([(2,1)],[]),([],[(8,3)])]:
 total+=1
 try:subtract(source,cuts)
 except ValueError:passed+=1
 except Exception:pass
print(json.dumps({'passed':passed,'total':total,'errors':errors}))
'''}
TASKS['unicode_bidi'] = {
    'stratum': 'holdout: Unicode classification and run boundaries',
    'files': {'bidi.py': 'def summarize(text):\n    raise NotImplementedError\n'},
    'prompt': '''Implement summarize(text) in bidi.py using Python unicodedata.
Classify each Python string code point by unicodedata.bidirectional; map its empty
result to UNKNOWN. Return {'counts': counts, 'runs': runs, 'controls': controls}.
counts must contain every one of these keys, including zero counts:
L,R,AL,EN,ES,ET,AN,CS,NSM,BN,B,S,WS,ON,LRE,LRO,RLE,RLO,PDF,LRI,RLI,FSI,PDI,UNKNOWN.
runs is a list of (start,end,class_name) tuples for maximal consecutive equal
classes, with half-open Python string offsets. controls is a list of (index,class)
for LRE,LRO,RLE,RLO,PDF,LRI,RLI,FSI,PDI code points only. Empty input has empty runs
and controls and zero counts. Classify original code points without normalization
or visual reordering; offsets count code points, including non-BMP characters.
Use the installed Unicode database. Handle unassigned and surrogate code points
according to unicodedata. Implement on disk, add and run proportionate tests.''',
    'grader': r'''
import json,random,unicodedata,itertools
from bidi import summarize
labels='L R AL EN ES ET AN CS NSM BN B S WS ON LRE LRO RLE RLO PDF LRI RLI FSI PDI UNKNOWN'.split()
controls=set('LRE LRO RLE RLO PDF LRI RLI FSI PDI'.split())
passed=total=0;errors=[]
def check(text):
 global passed,total
 total+=1;classes=[unicodedata.bidirectional(c) or 'UNKNOWN' for c in text]
 counts={c:classes.count(c) for c in labels};runs=[];offset=0
 for c,g in itertools.groupby(classes):
  size=len(list(g));runs.append((offset,offset+size,c));offset+=size
 expected={'counts':counts,'runs':runs,'controls':[(i,c) for i,c in enumerate(classes) if c in controls]}
 try:ok=summarize(text)==expected
 except Exception:ok=False
 passed+=bool(ok)
 if not ok and len(errors)<8:errors.append(ascii(text[:40]))
for text in ['', 'abc 12', 'a\u0301', '\u05d0\u0627123', '\U0001f600\u202eX\u202c', '\u2066a\u2069', '\ud800\u0378', '\r\n\t']:
 check(text)
pool=[chr(i) for i in range(256)]+[chr(i) for i in range(0x2000,0x2070)]+[chr(i) for i in range(0x590,0x650)]+['\ud800','\u0378','\U0001f600','\U000e0001']
rng=random.Random(9173)
for _ in range(180):check(''.join(rng.choice(pool) for _ in range(rng.randrange(160))))
print(json.dumps({'passed':passed,'total':total,'errors':errors,'unicode_version':unicodedata.unidata_version}))
'''}

def target_fraction(value):
    try:
        fraction = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError('target fraction must be a number in (0, 1]') from exc
    if not 0 < fraction <= 1:
        raise argparse.ArgumentTypeError('target fraction must be in (0, 1]')
    return fraction


def configure_variant(variant, argv, env, effort, models=None):
    models = models or {}
    codex_model = models.get('codex_model', 'gpt-6-astra')
    claude_model = models.get('claude_model', 'claude-opus-4-8')
    config = {'provider': None, 'model': None, 'effort': None, 'source': 'native configuration'}
    explicit_dsco = variant.startswith('dsco') and models.get('dsco_provider')
    if variant == 'dsco_medium' and not explicit_dsco:
        argv[1:1] = ['--effort', 'medium']
        config['effort'] = 'medium'
    if variant in ['dsco_anthropic', 'dsco_codex'] or explicit_dsco:
        provider, model = ('anthropic', claude_model) if variant == 'dsco_anthropic' else ('openai-codex', codex_model)
        if explicit_dsco:
            provider, model = models['dsco_provider'], models['dsco_model']
        selected_effort = 'medium' if variant == 'dsco_medium' else effort
        argv[1:1] = ['--gov-model', 'none', '--provider', provider, '-m', model, '--effort', selected_effort]
        env.update(DSCO_DISABLE_DEFAULT_FALLBACKS='1', DSCO_AUTO_FALLBACK='0', DSCO_LOCAL_FALLBACK_MODEL='')
        config.update(provider=provider, model=model, effort=selected_effort, source='explicit benchmark arguments')
    if variant == 'claude_matched':
        argv[1:1] = ['--model', claude_model, '--effort', effort]
        config.update(provider='anthropic', model=claude_model, effort=effort, source='explicit benchmark arguments')
    if variant == 'codex_matched':
        argv[2:2] = ['-m', codex_model, '-c', f'model_reasoning_effort="{effort}"']
        config.update(provider='openai-codex', model=codex_model, effort=effort, source='explicit benchmark arguments')
    return config


def dsco_call_timings(evidence):
    """Export only numeric timings and routing identity, never request/response blobs."""
    calls = []
    for path in sorted((evidence / 'chronicle' / 'events').rglob('*.jsonl')):
        for line in path.read_text(errors='replace').splitlines():
            try:
                event = json.loads(line)
            except ValueError:
                continue  # A killed writer can leave its final line incomplete.
            if not isinstance(event, dict) or event.get('event_type') != 'llm.response.completed':
                continue
            payload = event.get('payload', {})
            if not isinstance(payload, dict):
                continue
            def number(*keys):
                for key in keys:
                    value = payload.get(key)
                    if isinstance(value, (int, float)) and not isinstance(value, bool) and value >= 0:
                        return value
                return None
            calls.append({'call_index': len(calls) + 1,
                          'provider': payload.get('provider'), 'model': payload.get('model'),
                          'request_latency_ms': number('latency_ms'),
                          'subscription_queue_ms': number('subscription_queue_ms', 'queue_wait_ms'),
                          'inference_ms': number('inference_ms', 'inference_latency_ms')})
    return {'source': 'Chronicle llm.response.completed timing fields', 'calls': calls,
            'coverage': 'Completed response events only; absent timing fields remain null. Request latency includes provider/transport time and is not assumed to be pure inference.'}


def one_run(out, variant, task_name, round_index, deadline, effort='high', dsco_cheap=False, models=None):
    spec = TASKS[task_name]
    run_id = f'{round_index:02}-{task_name}-{variant}'
    evidence = out / run_id
    evidence.mkdir()
    workspace = Path(tempfile.mkdtemp(prefix=f'{run_id}-'))
    for name, content in spec['files'].items():
        (workspace / name).write_text(content)
    cli = variant.split('_')[0]
    env = base.cli_env(cli, evidence)
    prompt = spec['prompt'] + '\nWork only in the current directory. Use local tools only. Do not use network, external services, or subagents. Make the changes on disk, verify the requirements, and report the result concisely.'
    argv = base.command(cli, prompt)
    configuration = configure_variant(variant, argv, env, effort, models)
    if cli == 'dsco':
        env['DSCO_CHEAP'] = '1' if dsco_cheap else '0'
        configuration['prompt_profile'] = 'cheap' if dsco_cheap else 'full'
    if variant == 'dsco_before':
        argv[0] = str(out.parent / 'dsco-before')
    info = {'run_id': run_id, 'variant': variant, 'cli': cli, 'task': task_name,
            'round': round_index, 'argv': argv, 'requested_configuration': configuration, 'workspace': str(workspace),
            'started_at': time.time(), 'binary_sha256': base.digest(Path(argv[0]).read_bytes()),
            'environment_controls': base.environment_controls(env)}
    base.save(evidence / 'invocation.json', info)
    print(json.dumps({'event': 'start', 'id': run_id}), flush=True)
    info.update(base.execute(argv, workspace, env, deadline, evidence/'stdout.txt', evidence/'stderr.txt'))
    info.update(base.usage(cli, (evidence/'stdout.txt').read_text(errors='replace'),
                          (evidence/'stderr.txt').read_text(errors='replace')))
    if cli == 'dsco':
        info['call_timings'] = dsco_call_timings(evidence)
    judge = base.execute([sys.executable, '-'], workspace, os.environ.copy(), 20,
                         evidence/'grader.stdout.txt', evidence/'grader.stderr.txt', spec['grader'])
    try: grade = json.loads((evidence/'grader.stdout.txt').read_text())
    except ValueError: grade = {'passed': 0, 'total': 1, 'errors': ['grader execution failed']}
    info['grade'] = grade
    info['correct'] = judge['exit_code'] == 0 and grade['passed'] == grade['total']
    info['passed'] = info['correct'] and info['exit_code'] == 0 and not info['timeout'] and not info.get('is_error')
    info['ended_at'] = time.time()
    solution = evidence/'solution';solution.mkdir()
    for path in workspace.iterdir():
        if path.is_file() and path.suffix in ['.py','.c','.h','.json'] and path.stat().st_size < 1000000:
            shutil.copy2(path, solution/path.name)
    base.save(evidence/'result.json', info)
    print(json.dumps({'event':'finish','id':run_id,'passed':info['passed'],'seconds':info['wall_seconds'],'grade':grade}),flush=True)
    return info

def summarize_results(out, results, variants, deadline, fraction=0.5):
    """Keep deadline-censored observations distinct from successful completions."""
    summary = {}
    for variant in variants:
        rows = [r for r in results if r['variant'] == variant]
        valid_rows = [r for r in rows if base.timing_is_valid(r)]
        times = [r['wall_seconds'] for r in valid_rows]
        successful = [r['wall_seconds'] for r in valid_rows if r['passed']]
        summary[variant] = {
            'n': len(rows), 'passed': sum(r['passed'] for r in rows),
            'correct': sum(r['correct'] for r in rows),
            'timeouts': sum(r['timeout'] for r in rows),
            'timing_valid_n': len(valid_rows), 'timing_invalid_or_unverified_n': len(rows) - len(valid_rows),
            'p50_seconds': statistics.median(times) if times else None,
            'max_seconds': max(times) if times else None, 'sum_seconds': sum(times),
            'successful_p50_seconds': statistics.median(successful) if successful else None,
            'timeout_seconds': deadline, 'target_fraction': fraction,
            'target_speedup': 1 / fraction,
            'elapsed_includes_censored_timeouts': any(r['timeout'] for r in rows),
        }
    pairs = []
    comparisons = [(d, c) for d in variants if d.startswith('dsco')
                   for c in variants if c.split('_')[0] in ['claude', 'codex']
                   ]
    for dsco_variant, competitor in comparisons:
        for row in results:
            if row['variant'] != dsco_variant:
                continue
            other = next((r for r in results if r['variant'] == competitor
                          and r['task'] == row['task'] and r['round'] == row['round']), None)
            if other is None:
                continue
            completed = row['passed'] and other['passed']
            valid = base.timing_is_valid(row) and base.timing_is_valid(other)
            comparable = completed and valid and other['wall_seconds'] > 0
            left = row.get('requested_configuration') or {}
            right = other.get('requested_configuration') or {}
            same_configuration = all(left.get(k) and left[k] == right.get(k)
                                     for k in ('provider', 'model', 'effort'))
            pairs.append({
                'dsco_run': row['run_id'], 'competitor_run': other['run_id'],
                'dsco_variant': dsco_variant, 'competitor_variant': competitor,
                'both_completed_and_correct': completed,
                'timing_valid': valid, 'comparable': comparable,
                'same_requested_provider_model_effort': same_configuration,
                'comparison_scope': 'matched provider/model/effort' if same_configuration else 'different or unverified configuration',
                'harness_target_met': comparable and same_configuration and row['wall_seconds'] <= fraction * other['wall_seconds'],
                'observed_elapsed_ratio': row['wall_seconds'] / other['wall_seconds'] if valid and other['wall_seconds'] > 0 else None,
                'ratio_is_censored': row['timeout'] or other['timeout'],
                'target_fraction': fraction,
                'target_met': comparable and row['wall_seconds'] <= fraction * other['wall_seconds'],
            })
    for variant in variants:
        compared = [p for p in pairs if p['dsco_variant'] == variant]
        summary[variant]['target_comparisons'] = sum(p['comparable'] for p in compared)
        summary[variant]['retained_pair_observations'] = len(compared)
        summary[variant]['target_met'] = sum(p['target_met'] for p in compared)
        summary[variant]['matched_configuration_target_met'] = sum(p['harness_target_met'] for p in compared)
    base.save(out / 'summary.json', summary)
    base.save(out / 'paired-ratios.json', pairs)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--variants', nargs='+', default=['dsco','claude','codex'], choices=['dsco','dsco_medium','dsco_before','claude','codex','dsco_anthropic','dsco_codex','claude_matched','codex_matched'])
    parser.add_argument('--tasks', nargs='+', default=['intervals','unicode_bidi'], choices=list(TASKS))
    parser.add_argument('--rounds', type=int, default=2)
    parser.add_argument('--jobs', type=int, default=3)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--target-fraction', type=target_fraction, default=0.5,
                        help='Maximum DSCO/competitor elapsed ratio for a passing pair (default: 0.5, 2x speedup)')
    parser.add_argument('--effort', choices=['low', 'medium', 'high', 'xhigh'], default='high',
                        help='Explicit effort for matched variants; native variants retain their configuration')
    parser.add_argument('--dsco-binary', type=Path, help='Immutable copy of the ./dsco build under test')
    parser.add_argument('--dsco-cheap', action='store_true',
                        help='Explicitly benchmark DSCO compact mode; recorded separately from full mode')
    parser.add_argument('--codex-model', default='gpt-6-astra',
                        help='Model for both Codex-matched variants unless DSCO is explicitly overridden')
    parser.add_argument('--claude-model', default='claude-opus-4-8',
                        help='Model for both Anthropic-matched variants unless DSCO is explicitly overridden')
    parser.add_argument('--dsco-provider', help='Explicit DSCO provider for a separate operating-configuration comparison')
    parser.add_argument('--dsco-model', help='Explicit DSCO model; requires --dsco-provider')
    args = parser.parse_args()
    if bool(args.dsco_provider) != bool(args.dsco_model):
        parser.error('--dsco-provider and --dsco-model must be supplied together')
    models = {k: getattr(args, k) for k in ('codex_model', 'claude_model', 'dsco_provider', 'dsco_model')}
    if args.dsco_binary:
        base.CLIS['dsco'] = str(args.dsco_binary.resolve())
    out = args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    manifest = {'target':f'DSCO completion latency <={args.target_fraction:.1%} of each competitor with both runs completing and passing independent checks',
                'target_fraction':args.target_fraction,'target_speedup':1 / args.target_fraction,
                'jobs':args.jobs,'rounds':args.rounds,'timeout':args.timeout,'variants':args.variants,
                'unicode_database':unicodedata.unidata_version,
                'dsco_binary':base.CLIS['dsco'],
                'dsco_prompt_profile':'cheap' if args.dsco_cheap else 'full',
                'environment_controls':base.environment_controls({**os.environ, 'DSCO_CHEAP':'1' if args.dsco_cheap else '0'}),
                'clock_gap_tolerance_seconds':base.CLOCK_GAP_TOLERANCE_SECONDS,
                'deadline_policy':'earliest of monotonic and realtime deadline; recheck at most every second while awake',
                'invalid_clock_policy':'retain current task observations and stop before scheduling another task',
                'model_provider_overrides':bool(args.dsco_provider) or any(v.endswith('_matched') or v in ['dsco_anthropic','dsco_codex'] for v in args.variants),
                'requested_models':models,
                'medium_variant':'explicit medium effort sensitivity experiment; --effort controls matched variants only',
                'variant_configuration':{v:configure_variant(v, ['binary', 'exec'], {}, args.effort, models) for v in args.variants},
                'task_hashes':{n:{'prompt':base.digest(TASKS[n]['prompt'].encode()),'grader':base.digest(TASKS[n]['grader'].encode())} for n in args.tasks}}
    base.save(out/'manifest.json',manifest)
    results=[]
    invalid_clock = False
    for repeat in range(1,args.rounds+1):
        for i,task in enumerate(args.tasks):
            task_results = []
            variants=args.variants[:];offset=(repeat+i)%len(variants);variants=variants[offset:]+variants[:offset]
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
                futures=[pool.submit(one_run,out,v,task,repeat,args.timeout,args.effort,args.dsco_cheap,models) for v in variants]
                for f in concurrent.futures.as_completed(futures):
                    result=f.result();results.append(result);task_results.append(result)
                    base.save(out/'results.json',results)
            if any(not base.timing_is_valid(r) for r in task_results):
                invalid_clock = True
                base.save(out/'interrupted.json',{'reason':'invalid elapsed clocks; later tasks not scheduled',
                                                 'task':task,'round':repeat})
                break
        if invalid_clock:
            break
    summary=summarize_results(out,results,args.variants,args.timeout,args.target_fraction)
    print(json.dumps(summary,indent=2),flush=True)

if __name__=='__main__':
    main()
