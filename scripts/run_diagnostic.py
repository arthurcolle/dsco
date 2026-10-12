#!/usr/bin/env python3
"""Offline DSCO run diagnostic. Reads Chronicle JSONL; never executes a workload."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import tempfile
import time

VERSION = 'dsco.run_diagnostic.v1'

def number(value):
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value >= 0 else None

def union_seconds(intervals):
    end = None
    total = 0.0
    for a, b in sorted(intervals):
        if b < a: continue
        total += max(0, b - max(a, end if end is not None else a))
        end = max(b, end if end is not None else b)
    return total

def analyze(events, trace_id):
    selected = [e for e in events if e.get('trace_id') == trace_id]
    requests = [e for e in selected if e.get('event_type') == 'llm.request.created']
    responses = [e for e in selected if e.get('event_type') == 'llm.response.completed']
    if not requests: raise ValueError('selected trace has no model requests')
    warnings = []
    def unique(rows):
        result = {}
        for e in rows:
            key = e.get('span_id')
            if not isinstance(key, str) or not key: raise ValueError('request/response missing span_id')
            if key in result: raise ValueError('duplicate request/response span_id')
            result[key] = e
        return result
    starts, finishes = unique(requests), unique(responses)
    if set(starts) != set(finishes): warnings.append('Incomplete request/response pairing; completion is not established.')
    sessions = {e.get('session_id') for e in selected}
    if len(sessions) != 1: raise ValueError('trace spans multiple sessions')
    session = next(iter(sessions))
    turns = []
    for key, r in starts.items():
        if key not in finishes: continue
        e = finishes[key]
        a, b = number(r.get('wall_time')), number(e.get('wall_time'))
        p, inp = e.get('payload', {}), r.get('payload', {})
        latency = number(p.get('latency_ms'))
        if a is None or b is None or b < a or latency is None: raise ValueError('invalid request timing')
        if latency / 1000 > b - a + 2: warnings.append('Transport duration exceeds coarse request span; timing attribution uncertain.')
        usage = p.get('usage', {})
        if not isinstance(usage, dict): raise ValueError('invalid usage shape')
        fields = ('input_tokens', 'cache_read_tokens', 'cache_write_tokens', 'output_tokens', 'reasoning_tokens')
        u = {k: number(usage.get(k)) for k in fields}
        turns.append({'start_unix': a, 'end_unix': b, 'span_seconds': b-a, 'transport_seconds': latency/1000,
                      'request_bytes': number(inp.get('byte_len')), 'usage': u})
    if not turns: raise ValueError('no complete model request spans')
    turns.sort(key=lambda x: x['start_unix'])
    start = min(number(e.get('wall_time')) for e in requests if number(e.get('wall_time')) is not None)
    end = max(x['end_unix'] for x in turns)
    windows = [(x['start_unix'], x['end_unix']) for x in turns]
    span_sum = sum(x['span_seconds'] for x in turns)
    span_union = union_seconds(windows)
    concurrent = span_sum > span_union + 1
    if concurrent: warnings.append('Overlapping requests: summed durations are not wall-time partitions.')
    nearby = [e for e in events if e.get('session_id') == session and number(e.get('wall_time')) is not None and start <= e['wall_time'] <= end]
    costs = [e.get('payload', {}) for e in nearby if e.get('event_type') == 'inference.cost']
    queue = [e.get('payload', {}) for e in nearby if str(e.get('event_type', '')).startswith('provider.queue.')]
    tools = [e.get('payload', {}) for e in selected if e.get('event_type') == 'tool.call.completed']
    def aggregate(values):
        valid = [x for x in values if number(x) is not None]
        return {'total': sum(valid) if values and len(valid) == len(values) else None,
                'known_subtotal': sum(valid), 'known_count': len(valid), 'missing_count': len(values)-len(valid)}
    usage = {k: aggregate([x['usage'][k] for x in turns]) for k in turns[0]['usage']}
    transport = sum(x['transport_seconds'] for x in turns)
    estimates = aggregate([number(x.get('estimated_inference_usd')) for x in costs])
    invoices = aggregate([number(x.get('invoiced_usd')) for x in costs])
    for label, rows in [('cost', costs), ('queue', queue)]:
        if len(rows) != len(turns): warnings.append(f'{label} records do not match completed-request count; inspect coverage/retries.')
    signals = []
    residual = span_sum-transport
    if not concurrent and residual > span_sum*.3:
        signals.append('Substantial time lies outside transport within model requests. Inspect admission/cooldown timing; do not assume CPU or provider latency.')
    def input_tokens(x):
        values = [x['usage'][k] for k in ('input_tokens','cache_read_tokens','cache_write_tokens')]
        return sum(values) if all(v is not None for v in values) else None
    first, last = input_tokens(turns[0]), input_tokens(turns[-1])
    if first and last and last > 2*first: signals.append('Request context more than doubled; reduce retained tool output and repeated serial turns.')
    if len(turns) > 20: signals.append('More than twenty model round trips; look for bounded deterministic batches.')
    fingerprint = hashlib.sha256(json.dumps(selected, sort_keys=True, separators=(',',':')).encode()).hexdigest()
    return {'schema': VERSION, 'trace_id': trace_id, 'selected_events_sha256': fingerprint,
        'coverage': 'partial' if set(starts)!=set(finishes) else 'paired', 'warnings': warnings,
        'time': {'start_unix': start, 'end_unix': end, 'wall_seconds': end-start, 'completed_request_count': len(turns),
                 'request_count':len(requests), 'transport_seconds_sum': transport, 'request_span_seconds_sum':span_sum,
                 'request_span_union_seconds': span_union, 'request_span_residual_seconds_sum': residual,
                 'outside_complete_request_spans_seconds':end-start-span_union, 'overlapping_requests':concurrent,
                 'queue_wait_ms': aggregate([number(p.get('queue_wait_ms')) for p in queue]),
                 'queue_attribution':'session_time_window_not_exact_request_correlation'},
        'tools':{'completed_calls':len(tools),'latency_ms':aggregate([number(p.get('latency_ms')) for p in tools]),
                 'result_bytes':aggregate([number(p.get('result_byte_len')) for p in tools])},
        'context':{'first_input_tokens':first,'last_input_tokens':last,'wire_bytes':aggregate([x['request_bytes'] for x in turns]),'usage':usage},
        'economics':{'currency':'USD','scope':'session_time_window','records':len(costs),'estimated_inference_usd':estimates,
                     'invoiced_usd':invoices,'task_completion_verified':False,
                     'cost_per_verified_outcome':None,'note':'Estimates are not invoices. No acceptance verdict is inferred from tool success.'},
        'signals':signals,
        'limits':['Second-resolution event timestamps; transport timings are milliseconds.',
                  'Request residual is not a directly measured queue or CPU time.',
                  'Tool sums may overlap and are not an additive wall-time category.',
                  'No prompts, tool arguments, result text, credentials, request blobs, or network access are used in the report.']}

def markdown(d):
    t=d['time']; c=d['context']; e=d['economics']; lines=['# DSCO Run Diagnostic','',f"Coverage: **{d['coverage']}**. Trace: `{d['trace_id']}`.",'',
        '| Measurement | Value |','|---|---:|',f"| Wall time | {t['wall_seconds']:.3f} s |",
        f"| Completed model requests | {t['completed_request_count']} |",f"| Transport, summed | {t['transport_seconds_sum']:.3f} s |",
        f"| Request-span residual, summed | {t['request_span_residual_seconds_sum']:.3f} s |",
        f"| Completed tool calls | {d['tools']['completed_calls']} |",f"| First → last request input tokens | {c['first_input_tokens']} → {c['last_input_tokens']} |",'',
        '## Findings',*['- '+s for s in d['signals']], '', '## Economics',
        f"- Estimated inference USD: {e['estimated_inference_usd']['total']} (known subtotal {e['estimated_inference_usd']['known_subtotal']:.6f}).",
        f"- Invoiced USD: {e['invoiced_usd']['total']}; unknown is not zero.",'- No verified task-completion verdict or cost-per-verified-outcome was inferred.','',
        '## Coverage and limitations',*['- '+s for s in d['warnings']+d['limits']],'',f"Evidence fingerprint: `{d['selected_events_sha256']}`",'']
    return '\n'.join(lines)

def atomic(path, text):
    path.parent.mkdir(parents=True,exist_ok=True)
    fd, temporary=tempfile.mkstemp(prefix='.'+path.name,dir=path.parent)
    try:
        with os.fdopen(fd,'w') as f: f.write(text);f.flush();os.fsync(f.fileno())
        os.replace(temporary,path)
        if path.read_text()!=text: raise OSError('artifact readback differs')
    finally:
        if os.path.exists(temporary):os.unlink(temporary)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--events',required=True,type=Path);parser.add_argument('--trace');parser.add_argument('--list',action='store_true')
    parser.add_argument('--output',type=Path);parser.add_argument('--strict',action='store_true');a=parser.parse_args()
    started=time.monotonic()
    try:
        events=[];partial_tail=False
        with a.events.open('rb') as f:
            while True:
                line = f.readline(4*1024*1024+1)
                if not line: break
                if len(line)>4*1024*1024:raise ValueError('event exceeds 4 MiB bound')
                if not line.endswith(b'\n'):partial_tail=True;break
                row=json.loads(line)
                if not isinstance(row,dict) or not isinstance(row.get('payload',{}),dict):raise ValueError('invalid event shape')
                events.append(row)
        traces=sorted({e['trace_id'] for e in events if e.get('event_type')=='llm.request.created' and isinstance(e.get('trace_id'),str)})
        if a.list:print(json.dumps({'traces':traces,'partial_tail':partial_tail}));return 0
        if not a.trace or not re.fullmatch(r'[A-Za-z0-9_-]{1,128}',a.trace):raise ValueError('select a trace using --trace; inspect --list first')
        if not a.output:raise ValueError('--output directory required')
        if a.output.resolve()==a.events.parent.resolve():raise ValueError('use a separate output directory, not the event-log directory')
        d=analyze(events,a.trace)
        if partial_tail:d['coverage']='partial';d['warnings'].append('Incomplete trailing event excluded; live log may still be appending.')
        d['analysis_seconds']=round(time.monotonic()-started,6)
        atomic(a.output/'diagnostic.json',json.dumps(d,indent=2,allow_nan=False)+'\n');atomic(a.output/'REPORT.md',markdown(d))
        print(json.dumps({'report':str(a.output/'REPORT.md'),'coverage':d['coverage'],'analysis_seconds':d['analysis_seconds'],'requests':d['time']['completed_request_count']}))
        return 3 if a.strict and d['coverage']!='paired' else 0
    except (OSError,ValueError,TypeError,KeyError) as error:parser.exit(2,f'error: {error}\n')
if __name__=='__main__':raise SystemExit(main())
