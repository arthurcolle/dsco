#!/usr/bin/env python3
"""Live, paired CLI pilot. Standard library only; no production source edits.

Run: python3 bench/cli_compare.py --output reports/cli-comparison-YYYYMMDD
Model/provider selection is left to each CLI's existing configuration.
Graders run from stdin after completion and are not placed in agent workspaces.
"""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
import platform
import re
import signal
import shutil
import sqlite3
import statistics
import subprocess
import sys
import tempfile
import time
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLIS = {'codex': '/opt/homebrew/bin/codex', 'claude': '/opt/homebrew/bin/claude',
        'dsco': str(ROOT / 'dsco')}
TASKS = {
    'ring': {
        'stratum': 'C implementation and memory safety',
        'prompt': '''Implement ring.c to satisfy ring.h, using only standard C11.
rb_init creates an int FIFO with capacity cap, returns 1 on success, 0 for cap=0,
allocation failure or size overflow; failed init leaves a safely destroyable empty
object. rb_destroy frees storage and zeroes every field; repeat destroy is safe.
rb_push returns 1 on success, 0 when full; rb_pop returns 1 on success, 0 when empty,
and must leave *out unchanged when empty. rb_size returns the element count.
rb_push_many adds all n values in FIFO order, returning 1; if insufficient space,
return 0 and change nothing. n=0 succeeds even with values=NULL.
Support arbitrary positive capacities, wraparound, and INT_MIN/INT_MAX values.
Assume valid object/output pointers and values when n>0. Do not change ring.h.
Compile and test your implementation. Deliver the working files.''',
        'files': {
            'ring.h': '''#ifndef RING_H
#define RING_H
#include <stddef.h>
typedef struct { int *data; size_t capacity, head, count; } ring;
int rb_init(ring *r, size_t cap);
void rb_destroy(ring *r);
int rb_push(ring *r, int value);
int rb_pop(ring *r, int *out);
size_t rb_size(const ring *r);
int rb_push_many(ring *r, const int *values, size_t n);
#endif
''',
            'ring.c': '#include "ring.h"\n/* Implement the API here. */\n'},
        'grader': r'''
import ctypes as c, json, random, subprocess, collections
r=subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC','ring.c','-o','ring.so'],capture_output=True,text=True)
if r.returncode:
 print(json.dumps({'passed':0,'total':1,'errors':['compile: '+r.stderr[-2000:]]}));raise SystemExit()
lib=c.CDLL('./ring.so')
class Ring(c.Structure):
 _fields_=[('data',c.POINTER(c.c_int)),('capacity',c.c_size_t),('head',c.c_size_t),('count',c.c_size_t)]
lib.rb_init.argtypes=[c.POINTER(Ring),c.c_size_t]
lib.rb_destroy.argtypes=[c.POINTER(Ring)]
lib.rb_push.argtypes=[c.POINTER(Ring),c.c_int]
lib.rb_pop.argtypes=[c.POINTER(Ring),c.POINTER(c.c_int)]
lib.rb_size.argtypes=[c.POINTER(Ring)];lib.rb_size.restype=c.c_size_t
lib.rb_push_many.argtypes=[c.POINTER(Ring),c.POINTER(c.c_int),c.c_size_t]
passed=total=0;errors=[]
def check(x,label):
 global passed,total
 total+=1;passed+=bool(x)
 if not x and len(errors)<12:errors.append(label)
for cap in [0,c.c_size_t(-1).value]:
 q=Ring();check(lib.rb_init(c.byref(q),cap)==0,'invalid init')
 check(not q.data and q.capacity==q.head==q.count==0,'failed init reset');lib.rb_destroy(c.byref(q))
for cap in [1,2,3,7,16]:
 q=Ring();check(lib.rb_init(c.byref(q),cap)==1,'init');ref=collections.deque();rng=random.Random(813+cap)
 for step in range(350):
  op=rng.randrange(3)
  if op==0:
   value=rng.choice([-2147483648,2147483647,rng.randint(-100,100)]);ok=len(ref)<cap
   check(lib.rb_push(c.byref(q),value)==int(ok),'push status')
   if ok:ref.append(value)
  elif op==1:
   out=c.c_int(847);ok=bool(ref);expected=ref.popleft() if ok else 847
   check(lib.rb_pop(c.byref(q),c.byref(out))==int(ok) and out.value==expected,'pop FIFO and no mutation')
  else:
   n=rng.randrange(cap+3);values=[rng.randint(-50,50) for _ in range(n)];arr=(c.c_int*n)(*values) if n else None;ok=n<=cap-len(ref)
   check(lib.rb_push_many(c.byref(q),arr,n)==int(ok),'bulk atomic status')
   if ok:ref.extend(values)
  check(lib.rb_size(c.byref(q))==len(ref),'size')
 lib.rb_destroy(c.byref(q));lib.rb_destroy(c.byref(q))
 check(not q.data and q.capacity==q.head==q.count==0,'destroy reset')
print(json.dumps({'passed':passed,'total':total,'errors':errors}))
''',
    },
    'dag': {
        'stratum': 'Python deterministic graph algorithm',
        'prompt': '''Implement plan(graph) in planner.py, Python standard library only.
graph maps string task names to iterables of prerequisite names. Include names
which occur only as prerequisites. Duplicate prerequisites count once. Return
{'order': [...], 'levels': {...}}. At each topological-sort step choose the
lexicographically smallest currently ready task (not a batch of ready tasks).
levels maps every task to 0 if it has no prerequisites, else one plus the largest
prerequisite level. Empty graph returns empty order and levels. Accept one-shot
iterators as prerequisite values. Do not mutate input. If a cycle exists, raise
ValueError('cycle: ' + ','.join(sorted(blocked))), where blocked is ALL nodes left
unprocessed by Kahn's algorithm, including downstream dependents of cycles.
Add and run tests; deliver the working implementation.''',
        'files': {'planner.py': 'def plan(graph):\n    raise NotImplementedError\n'},
        'grader': r'''
import json,random,copy
from planner import plan
passed=total=0;errors=[]
def reference(g):
 nodes=set(g)
 for deps in g.values():nodes.update(deps)
 deps={n:set(g.get(n,[])) for n in nodes};done=[];levels={}
 while len(done)<len(nodes):
  ready=sorted(n for n in nodes if n not in levels and deps[n]<=levels.keys())
  if not ready:return 'cycle: '+','.join(sorted(nodes-levels.keys()))
  n=ready[0];levels[n]=max((levels[d]+1 for d in deps[n]),default=0);done.append(n)
 return {'order':done,'levels':levels}
def check(g,iterator=False):
 global passed,total
 total+=1;expected=reference(g);before=copy.deepcopy(g)
 try:
  result=plan({k:iter(v) for k,v in g.items()} if iterator else g)
  ok=result==expected and (iterator or g==before)
 except ValueError as e:ok=isinstance(expected,str) and str(e)==expected
 except Exception as e:ok=False
 passed+=bool(ok)
 if not ok and len(errors)<8:errors.append(repr(g))
cases=[{}, {'a':[]}, {'b':['a','a']}, {'a':['z'],'b':[],'z':[]}, {'x':['x'],'down':['x'],'free':[]}, {'a':['b'],'b':['a'],'c':['b'],'d':[]}, {'z':['a'],'m':['z']}]
for g in cases:check(g);check(g,True)
rng=random.Random(90210)
for i in range(120):
 names=[f'n{x:02}' for x in range(rng.randrange(1,22))];rng.shuffle(names)
 g={n:[p for p in names[:k] if rng.random()<.3] for k,n in enumerate(names)}
 if i%4==0:g[names[0]].append(names[-1])
 if i%5==0:g.pop(names[-1])
 check(g);check(g,True)
print(json.dumps({'passed':passed,'total':total,'errors':errors}))
''',
    },
    'ttl_cache': {
        'stratum': 'Python stateful API with time and eviction semantics',
        'prompt': '''Implement TTLCache in cache.py, standard library only.
TTLCache(capacity) raises ValueError if capacity<=0. Public methods:
put(key, value, ttl, now), get(key, now, default=None), delete(key, now), size(now).
Every method first expires ALL entries whose deadline<=now. put stores the value
with deadline now+ttl and marks it most recently used. ttl<=0 deletes an existing
key and stores nothing. Inserting over capacity evicts the least recently used
live key. Updating a live key replaces its value/deadline without evicting another
key, and marks it most recent. A successful get marks its key most recent; a miss
returns default. delete returns True iff a live key was removed, else False.
size returns live entry count and does not change recency. put returns None.
Values may be None or False; keys are hashable. Times are explicit finite numbers
and nondecreasing within a cache instance; do not use wall-clock time.
Add and run tests; deliver the working implementation.''',
        'files': {'cache.py': 'class TTLCache:\n    pass\n'},
        'grader': r'''
import collections,json,random
from cache import TTLCache
passed=total=0;errors=[]
def check(x,label):
 global passed,total
 total+=1;passed+=bool(x)
 if not x and len(errors)<10:errors.append(label)
for cap in [0,-1]:
 try:TTLCache(cap);check(False,'invalid capacity')
 except ValueError:check(True,'invalid capacity')
for cap in [1,2,5]:
 cache=TTLCache(cap);ref=collections.OrderedDict();now=0;rng=random.Random(710+cap)
 for step in range(600):
  now+=rng.choice([0,0,.5,1,2]);key=rng.randrange(8)
  for k in list(ref):
   if ref[k][1]<=now:del ref[k]
  op=rng.randrange(4)
  if op==0:
   value=rng.choice([None,False,0,'v',17]);ttl=rng.choice([-1,0,.5,2,10])
   ref.pop(key,None)
   if ttl>0:
    ref[key]=(value,now+ttl)
    while len(ref)>cap:ref.popitem(last=False)
   check(cache.put(key,value,ttl,now) is None,'put return')
  elif op==1:
   expected='MISSING'
   if key in ref:expected=ref[key][0];ref.move_to_end(key)
   actual=cache.get(key,now,'MISSING')
   check(type(actual)==type(expected) and actual==expected,'get and recency')
  elif op==2:
   expected=key in ref;ref.pop(key,None)
   check(cache.delete(key,now) is expected,'delete')
  else:check(cache.size(now)==len(ref),'size')
  check(cache.size(now)==len(ref),'post-operation size')
print(json.dumps({'passed':passed,'total':total,'errors':errors}))
''',
    },
}

def digest(data):
    return hashlib.sha256(data).hexdigest()

def save(path, data):
    path.write_text(json.dumps(data, indent=2) + '\n')

CLOCK_GAP_TOLERANCE_SECONDS = 1.0
ENVIRONMENT_CONTROL_KEYS = ('DSCO_CHATGPT_GLOBAL_GATE', 'DSCO_CHATGPT_GATE_MAX_WAIT_MS',
                            'DSCO_CHATGPT_TRACE', 'DSCO_PROFILE', 'DSCO_CHEAP')

def environment_controls(env):
    """Persist only named non-secret benchmark controls, never the environment."""
    return {key: env.get(key) for key in ENVIRONMENT_CONTROL_KEYS}

def elapsed_timing(start, realtime_start, end, realtime_end):
    elapsed = end - start
    realtime = realtime_end - realtime_start
    gap = realtime - elapsed
    valid = elapsed >= 0 and realtime >= 0 and abs(gap) <= CLOCK_GAP_TOLERANCE_SECONDS
    return {'wall_seconds': round(elapsed, 4), 'realtime_seconds': round(realtime, 4),
            'clock_gap_seconds': round(gap, 4), 'timing_valid': valid,
            'clock_gap_tolerance_seconds': CLOCK_GAP_TOLERANCE_SECONDS,
            'process_started_at': realtime_start, 'process_ended_at': realtime_end,
            'timing_invalid_reason': None if valid else 'realtime/monotonic clock disagreement'}

def timing_is_valid(row):
    # Older records without both-clock evidence cannot establish valid timing.
    return row.get('timing_valid') is True

def wait_until_deadline(process, timeout, monotonic_start, realtime_start):
    """Recheck both clocks after resume; Python's timeout clock can exclude suspend."""
    while True:
        remaining = min(timeout - (time.perf_counter() - monotonic_start),
                        timeout - (time.time() - realtime_start))
        if remaining <= 0:
            raise subprocess.TimeoutExpired(process.args, timeout)
        try:
            process.wait(timeout=min(remaining, 1.0))
            return
        except subprocess.TimeoutExpired:
            pass

def execute(argv, cwd, env, timeout, stdout_path, stderr_path, stdin=None):
    start = time.perf_counter()
    realtime_start = time.time()
    with ExitStack() as stack:
        out = stack.enter_context(stdout_path.open('w'))
        err = stack.enter_context(stderr_path.open('w'))
        child_input = subprocess.DEVNULL
        if stdin:
            # A private anonymous file avoids a blocked input pipe across timed
            # waits. Grader source is never written into the task workspace.
            child_input = stack.enter_context(tempfile.TemporaryFile(mode='w+', encoding='utf-8'))
            child_input.write(stdin)
            child_input.seek(0)
        p = subprocess.Popen(argv, cwd=cwd, env=env, stdin=child_input,
                             stdout=out, stderr=err, start_new_session=True, text=True)
        timed_out = False
        try:
            wait_until_deadline(p, timeout, start, realtime_start)
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass  # The process/group can exit between the deadline and signal.
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                p.wait()
    end = time.perf_counter()
    realtime_end = time.time()
    return {'exit_code': p.returncode, 'timeout': timed_out,
            **elapsed_timing(start, realtime_start, end, realtime_end)}

def cli_env(name, evidence):
    env = os.environ.copy()
    # The benchmark permits only local task work. No remote connector work.
    for k in ['DSCO_MCP_SERVER', 'DSCO_MCP_SERVERS', 'DSCO_DURABLE_AGENT_ID', 'DSCO_SUBAGENT']:
        env.pop(k, None)
    if name == 'dsco':
        env.update(DSCO_MCP_HEADLESS='0', DSCO_TOOLMGMT='0', DSCO_NO_AUTO_INTERACTIVE='1',
                   DSCO_HARD_TURN_CEILING='24', DSCO_BUDGET='2',
                   DSCO_BASELINE_DB=str(evidence / 'baseline.sqlite'),
                   DSCO_CHRONICLE_DIR=str(evidence / 'chronicle'))
    if name == 'claude' and not env.get('CLAUDE_CODE_OAUTH_TOKEN'):
        # Existing same-user Claude OAuth, already cached by DSCO. Never persist
        # the token in commands, results, or workspaces.
        p = Path.home() / '.dsco/claude-oauth.json'
        if p.exists():
            oauth = json.loads(p.read_text()).get('claudeAiOauth', {})
            if oauth.get('expiresAt', 0) > time.time() * 1000:
                env['CLAUDE_CODE_OAUTH_TOKEN'] = oauth['accessToken']
    return env

def command(name, prompt):
    if name == 'codex':
        args = [CLIS[name], 'exec', '--ephemeral', '--skip-git-repo-check', '--json',
                '-s', 'workspace-write', '-c', 'approval_policy="never"']
        config = tomllib.loads((Path.home() / '.codex/config.toml').read_text())
        for server in config.get('mcp_servers', {}):
            if not re.fullmatch(r'[A-Za-z0-9_-]+', server):
                raise ValueError(f'Unsupported MCP config key: {server}')
            args += ['-c', f'mcp_servers.{server}.enabled=false']
        return args + [prompt]
    if name == 'claude':
        return [CLIS[name], '-p', prompt, '--output-format', 'json', '--no-session-persistence',
                '--strict-mcp-config', '--mcp-config', '{"mcpServers":{}}',
                '--tools', 'Bash,Read,Write,Edit,Glob,Grep',
                '--allowedTools', 'Bash,Read,Write,Edit,Glob,Grep',
                '--max-budget-usd', '2']
    return [CLIS[name], '-p', prompt]

def usage(name, stdout, stderr):
    if name == 'codex':
        events = []
        for line in stdout.splitlines():
            try: events.append(json.loads(line))
            except ValueError: pass
        completed = [e for e in events if e.get('type') == 'turn.completed']
        return {'usage': completed[-1].get('usage') if completed else None,
                'completion_event': bool(completed),
                'tool_calls': sum(e.get('type') == 'item.completed' and e.get('item', {}).get('type') in
                                  ['command_execution', 'file_change', 'mcp_tool_call'] for e in events)}
    if name == 'claude':
        try:
            d = json.loads(stdout)
            return {k: d.get(k) for k in ['usage', 'modelUsage', 'total_cost_usd', 'num_turns',
                                        'is_error', 'permission_denials', 'result']}
        except ValueError:
            return {'usage': None}
    clean = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', stderr)
    turns = re.findall(r'usage: input=(\d+) output=(\d+)', clean)
    routes = re.findall(r'provider (\S+) · model (\S+)', clean)
    return {'usage': {'input_tokens': sum(int(a) for a, b in turns),
                      'output_tokens': sum(int(b) for a, b in turns)} if turns else None,
            'routes': sorted(set(routes)), 'accounted_turns': len(turns),
            'cost_lines': [line.strip() for line in clean.splitlines() if 'usage:' in line]}

def write_report(out):
    results = json.loads((out / 'results.json').read_text())
    manifest = json.loads((out / 'manifest.json').read_text())
    startup = json.loads((out / 'startup.json').read_text())
    for result in results:
        evidence = out / result['run_id']
        solution = evidence / 'solution'
        solution.mkdir(exist_ok=True)
        for path in Path(result['workspace']).iterdir():
            if path.is_file() and path.suffix in ['.c', '.h', '.py'] and path.stat().st_size < 1024 * 1024:
                shutil.copy2(path, solution / path.name)
        if result['cli'] == 'dsco' and (evidence / 'baseline.sqlite').exists():
            responses = []
            for event_file in (evidence / 'chronicle/events').rglob('*.jsonl'):
                for line in event_file.read_text().splitlines():
                    event = json.loads(line)
                    if event.get('event_type') == 'llm.response.completed':
                        responses.append(event['payload'])
            if responses:
                result['usage'] = {k: sum(p['usage'].get(k, 0) for p in responses)
                                   for k in ['input_tokens', 'output_tokens', 'cache_read_tokens',
                                             'cache_write_tokens', 'reasoning_tokens']}
                result['usage_source'] = 'chronicle llm.response.completed events'
                result['estimated_cost_usd'] = sum(p.get('cost_usd', 0) for p in responses)
                result['accounted_turns'] = len(responses)
            with sqlite3.connect(f'file:{evidence}/baseline.sqlite?mode=ro', uri=True) as conn:
                row = conn.execute("select ts_epoch from events where title='session_end' order by id desc limit 1").fetchone()
            if row:
                result['session_end_elapsed_seconds'] = round(row[0] - (evidence / 'invocation.json').stat().st_mtime, 4)
                result['post_session_end_seconds'] = round(result['wall_seconds'] - result['session_end_elapsed_seconds'], 4)
            save(evidence / 'result.json', result)
    save(out / 'results.json', results)
    lines = ['# Codex vs. Claude Code vs. DSCO-CLI', '',
             'Live local pilot: three paired coding tasks, one attempt per task and CLI, '
             f"{manifest['timeout_seconds']} seconds per attempt. Independent deterministic graders inspect actual files.", '',
             '| CLI | Completed and passed | Correct artifacts | Total elapsed | Median task time | Warm `--help` p50 |',
             '|---|---:|---:|---:|---:|---:|']
    summary = {}
    for name in CLIS:
        rows = [r for r in results if r['cli'] == name]
        if not rows:
            continue
        correct = sum(r['grader_process']['exit_code'] == 0 and r['grade']['passed'] == r['grade']['total'] for r in rows)
        valid_rows = [r for r in rows if timing_is_valid(r)]
        total = sum(r['wall_seconds'] for r in valid_rows)
        median = statistics.median(r['wall_seconds'] for r in valid_rows) if valid_rows else None
        summary[name] = {'completed_and_passed': sum(r['passed'] for r in rows), 'correct_artifacts': correct,
                         'n': len(rows), 'total_seconds': total, 'median_seconds': median,
                         'timeouts': sum(r['timeout'] for r in rows),
                         'timing_valid_n': len(valid_rows),
                         'timing_invalid_or_unverified_n': len(rows) - len(valid_rows)}
        median_text = f'{median:.1f}s' if median is not None else 'N/A'
        lines.append(f"| {name} | {summary[name]['completed_and_passed']}/{len(rows)} | {correct}/{len(rows)} | "
                     f"{total:.1f}s | {median_text} | {startup[name]['p50_ms']:.1f}ms |")
    lines += ['', 'A successful run must exit cleanly within the deadline and pass every independent check. '
              'Correct artifacts produced by a timed-out process are shown separately. Individual checks '
              'are correlated assertions, not thousands of independent benchmark tasks.', '',
              '| Task | CLI | Elapsed | Checks passed | Outcome |', '|---|---|---:|---:|---|']
    for task in TASKS:
        for name in CLIS:
            for r in results:
                if (r['task'], r['cli']) != (task, name):
                    continue
                outcome = 'PASS' if r['passed'] else ('TIMEOUT' if r['timeout'] else 'FAIL')
                timing_note = '' if timing_is_valid(r) else ' (invalid/unverified timing)'
                lines.append(f"| {task} | {name} | {r['wall_seconds']:.1f}s{timing_note} | "
                             f"{r['grade']['passed']}/{r['grade']['total']} | {outcome} |")
    lines += ['', '## Configuration actually exercised', '',
              f"- Codex: {manifest['binaries']['codex']['version']}; configured model "
              f"`{manifest['codex_configuration'].get('model')}`, effort `{manifest['codex_configuration'].get('model_reasoning_effort')}`."]
    models = sorted({m for r in results if r['cli'] == 'claude' for m in (r.get('modelUsage') or {})})
    routes = sorted({tuple(route) for r in results if r['cli'] == 'dsco' for route in r.get('routes', [])})
    lines += [f"- Claude Code: {manifest['binaries']['claude']['version']}; response-reported models: {', '.join('`'+m+'`' for m in models)}.",
              f"- DSCO: {manifest['binaries']['dsco']['version']}; runtime-traced provider/model routes: " + ', '.join(f'`{p} / {m}`' for p, m in routes) + '.',
              '- No model or provider flags were supplied to scored runs. DSCO retains configured native selection and fallback behavior.',
              '- DSCO is a multi-provider runtime. This pilot measures the routes above, not its entire provider catalog.',
              '- External MCP connectors were disabled for local task work. Codex used workspace-write; Claude had local coding tools preapproved; DSCO used its normal capability gate.',
              '- Claude initially reported not logged in. Its scored child processes received the existing unexpired same-user Claude OAuth token cached by DSCO; no saved credential configuration was changed.', '',
              '## Interpretation and limitations', '',
              '- This is an exploratory three-task sample, with no confidence interval or general product-ranking claim. There are too few independent tasks for useful statistical inference.',
              '- The intended task order is counterbalanced; in this run valid Codex attempts were repeated after the initial launcher errors, so service load and cache effects may vary by run order. Models, reasoning settings, prompts, permissions, and tool implementations differ; this is a configured-product comparison, not a model-controlled harness experiment.',
              '- Total latency includes reasoning, tools, tests, retries, and startup. `--help` timings are a warm CLI entry-path microbenchmark, not agent TTFT or cold machine startup.',
              '- Cost and token usage are retained in `results.json`. Claude reports API-equivalent dollars, DSCO logs estimated dollars even for subscription calls, and Codex does not provide comparable dollars. No spend ranking is inferred.',
              '- An initial Codex launcher quoting error happened before inference. Original attempts are retained in `*-harness-error` directories; corrected attempts are scored. Prompts and graders were unchanged.',
              '- Startup measurements using timeout polling were superseded by pipe-completion measurements to avoid polling delay; both files are retained.', '',
              '## Reproduce', '', '```sh',
              'python3 bench/cli_compare.py --output reports/cli-comparison-new --timeout 180', '```', '',
              'The output directory must be new. The harness uses existing credentials and configured models, '
              'executes actual coding agents, and consumes subscription quota or configured provider credits. '
              'Prompts, source/grader hashes, binary hashes, invocations, raw outputs, grades, usage, and timing '
              'are retained. Agent workspaces are listed in each invocation and result.', '']
    lines += ['## DSCO lifecycle evidence', '',
              '| Task | Session-end event | Process elapsed | Time after session-end event |',
              '|---|---:|---:|---:|']
    for r in results:
        if 'session_end_elapsed_seconds' in r:
            qualifier = ' (cut off)' if r['timeout'] else ''
            lines.append(f"| {r['task']} | {r['session_end_elapsed_seconds']:.1f}s | "
                         f"{r['wall_seconds']:.1f}s{qualifier} | {r['post_session_end_seconds']:.1f}s{qualifier} |")
    lines += ['', 'The timestamps establish when DSCO recorded session completion. The specific cause of '
              'the remaining exit delay has not been profiled or patched in this benchmark.', '']
    lines += ['## Reported usage', '', '| CLI | Output tokens | Cached input reads | Dollar telemetry |',
              '|---|---:|---:|---|']
    for name in CLIS:
        rows = [r for r in results if r['cli'] == name]
        output = sum((r.get('usage') or {}).get('output_tokens', 0) for r in rows)
        cache_key = {'codex': 'cached_input_tokens', 'claude': 'cache_read_input_tokens',
                     'dsco': 'cache_read_tokens'}[name]
        cached = sum((r.get('usage') or {}).get(cache_key, 0) for r in rows)
        dollars = 'Not reported'
        if name == 'claude':
            dollars = f"${sum(r.get('total_cost_usd') or 0 for r in rows):.4f} API-equivalent"
        if name == 'dsco':
            dollars = f"${sum(r.get('estimated_cost_usd') or 0 for r in rows):.4f} estimated"
        lines.append(f'| {name} | {output:,} | {cached:,} | {dollars} |')
    lines += ['', 'These are observed telemetry totals for scored attempts, not invoices. Cached reads '
              'are counted repeatedly across requests; tokenization and reasoning accounting differ. '
              'Timed-out requests can have incomplete usage records. Preflight and launcher-error attempts are excluded.', '']
    save(out / 'summary.json', summary)
    (out / 'REPORT.md').write_text('\n'.join(lines))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--rounds', type=int, default=1)
    parser.add_argument('--clis', nargs='+', choices=list(CLIS), default=list(CLIS))
    parser.add_argument('--resume', action='store_true', help='Retry harness failures while retaining original evidence')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=args.resume)
    work_root = Path(tempfile.mkdtemp(prefix='cli-comparison-'))
    manifest = {'eval_id': out.name, 'claim': 'Exploratory local coding performance with current CLI model/provider configuration',
                'created_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                'platform': platform.platform(), 'workspace_root': str(work_root),
                'timeout_seconds': args.timeout, 'rounds': args.rounds,
                'environment_controls': environment_controls(os.environ),
                'clock_gap_tolerance_seconds': CLOCK_GAP_TOLERANCE_SECONDS,
                'budget_cap': '180s default per run; Claude/DSCO $2 admission cap per run; existing subscriptions; Codex has no equivalent dollar flag',
                'judge_version': 'deterministic-v1', 'rubric_version': 'all-checks-required-v1',
                'seed': 'fixed per grader', 'split': 'locked pilot; no tuning or reruns after grading',
                'limitations': ['Three authored tasks are not a public benchmark or representative sample.',
                                'One run per task by default; no inferential significance claims.',
                                'CLI model, prompts, permissions and tool implementation differ.',
                                'Wall time includes process startup, inference, tool execution and retries.',
                                'External MCP connectors disabled; configured model/provider selection retained.',
                                'Claude child process uses existing same-user DSCO Claude OAuth cache if available.',
                                'No provider matrix: only routes actually exercised are measured.',
                                'Token accounting differs by provider; no dollar ranking from incomparable estimates.'],
                'binaries': {}, 'tasks': {}}
    for name, path in CLIS.items():
        p = Path(path)
        version = subprocess.run([path, '--version'], capture_output=True, text=True, timeout=10)
        manifest['binaries'][name] = {'path': path, 'sha256': digest(p.read_bytes()),
                                     'version': version.stdout.strip(), 'bytes': p.stat().st_size}
    cfg = tomllib.loads((Path.home() / '.codex/config.toml').read_text())
    manifest['codex_configuration'] = {k: cfg.get(k) for k in ['model', 'model_provider', 'model_reasoning_effort', 'service_tier']}
    cfg = json.loads((Path.home() / '.claude/settings.json').read_text())
    manifest['claude_configuration'] = {k: cfg.get(k) for k in ['model', 'effortLevel']}
    for task, spec in TASKS.items():
        manifest['tasks'][task] = {'stratum': spec['stratum'], 'prompt': spec['prompt'],
                                  'prompt_sha256': digest(spec['prompt'].encode()),
                                  'source_digest': digest(json.dumps(spec['files'], sort_keys=True).encode()),
                                  'grader_sha256': digest(spec['grader'].encode())}
    if not args.resume:
        save(out / 'manifest.json', manifest)
    # Counterbalanced interleaved process-start measurements; warm OS caches.
    startup = {name: [] for name in CLIS}
    for i in range(0 if args.resume else 17):
        names = list(CLIS); names = names[i % 3:] + names[:i % 3]
        for name in names:
            t = time.perf_counter()
            # Pipe completion avoids subprocess.wait(timeout)'s polling delay
            # dominating sub-10ms executables.
            p = subprocess.run([CLIS[name], '--help'], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, timeout=10)
            if p.returncode: raise RuntimeError(f'{name} --help failed')
            if i >= 2: startup[name].append((time.perf_counter() - t) * 1000)
    if not args.resume:
        save(out / 'startup.json', {n: {'samples_ms': v, 'p50_ms': statistics.median(v),
                                      'p95_ms': sorted(v)[-1], 'n': len(v)} for n, v in startup.items()})
    results = json.loads((out / 'results.json').read_text()) if args.resume else []
    for repeat in range(args.rounds):
        for i, (task, spec) in enumerate(TASKS.items()):
            names = list(CLIS); rotation = (i + repeat) % 3
            names = names[rotation:] + names[:rotation]
            for name in names:
                if name not in args.clis:
                    continue
                run_id = f'{repeat + 1:02}-{task}-{name}'
                previous = [r for r in results if r['run_id'] == run_id]
                if previous:
                    if not args.resume:
                        raise ValueError('Duplicate run')
                    previous_stderr = (out / run_id / 'stderr.txt').read_text()
                    if 'Error loading config.toml' not in previous_stderr:
                        raise ValueError('Resume is allowed only for a pre-inference harness config failure')
                    (out / run_id).rename(out / (run_id + '-harness-error'))
                    results = [r for r in results if r['run_id'] != run_id]
                evidence = out / run_id; evidence.mkdir()
                workspace = work_root / run_id; workspace.mkdir()
                for filename, content in spec['files'].items():
                    (workspace / filename).write_text(content)
                prompt = (spec['prompt'] + '\nWork only in the current directory. Use local tools only; '
                          'do not use network, external services, subagents, or inspect files outside this directory. '
                          'Make the changes on disk and run your own tests. No clarification is needed.')
                argv = command(name, prompt)
                env = cli_env(name, evidence)
                save(evidence / 'invocation.json', {'argv': argv, 'cwd': str(workspace),
                                                   'environment_controls': environment_controls(env)})
                print(json.dumps({'event': 'start', 'run_id': run_id}), flush=True)
                result = {'run_id': run_id, 'cli': name, 'task': task, 'workspace': str(workspace)}
                result.update(execute(argv, workspace, env, args.timeout,
                                      evidence / 'stdout.txt', evidence / 'stderr.txt'))
                stdout = (evidence / 'stdout.txt').read_text(errors='replace')
                stderr = (evidence / 'stderr.txt').read_text(errors='replace')
                result.update(usage(name, stdout, stderr))
                judge = execute([sys.executable, '-'], workspace, os.environ.copy(), 30,
                                evidence / 'grader.stdout.txt', evidence / 'grader.stderr.txt', spec['grader'])
                try: grade = json.loads((evidence / 'grader.stdout.txt').read_text())
                except ValueError: grade = {'passed': 0, 'total': 1, 'errors': ['grader execution failed; inspect grader.stderr.txt']}
                if task == 'ring' and (workspace / 'ring.h').read_text() != spec['files']['ring.h']:
                    grade['total'] += 1; grade['errors'].append('protected header changed')
                result['grade'] = grade
                result['grader_process'] = judge
                result['passed'] = (result['exit_code'] == 0 and not result['timeout'] and not result.get('is_error')
                                    and judge['exit_code'] == 0 and grade['passed'] == grade['total'])
                save(evidence / 'result.json', result)
                results.append(result); save(out / 'results.json', results)
                print(json.dumps({'event': 'finish', 'run_id': run_id, 'passed': result['passed'],
                                  'wall_seconds': result['wall_seconds'], 'grade': grade,
                                  'timeout': result['timeout']}), flush=True)
    for name in CLIS:
        rows = [r for r in results if r['cli'] == name]
        if not rows:
            continue
        valid_times = [r['wall_seconds'] for r in rows if timing_is_valid(r)]
        print(json.dumps({'cli': name, 'passed': sum(r['passed'] for r in rows), 'n': len(rows),
                          'timing_valid_n': len(valid_times),
                          'median_seconds': statistics.median(valid_times) if valid_times else None}), flush=True)
    write_report(out)

if __name__ == '__main__':
    main()
