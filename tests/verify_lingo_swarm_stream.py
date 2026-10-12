#!/usr/bin/env python3
"""Check a completed real three-agent Lingo swarm using independent retained evidence."""
import base64
import collections
import hashlib
import json
from pathlib import Path
import subprocess
import sys

directory = Path(sys.argv[1]).resolve()
result = json.loads((directory/'stdout.json').read_text())
launch = json.loads((directory/'launch.json').read_text())
metrics = json.loads((directory/'verification.json').read_text())
events = [json.loads(line) for line in (directory/'events.ndjson').read_text().splitlines()]
records = [e['record'] for e in events]
checks = {}
def check(name, condition):
    assert condition, name
    checks[name] = True

run = result['value']['run']
check('native_program_completed', result['value']['succeeded'] and metrics['exit_code']==0)
check('exact_three_process_operation', run['map_successes']==2 and run['process_attempts']==2 and run['coordinator_attempts']==1)
check('complete_without_degradation', run['complete'] and not run['degraded'] and run['outcome']=='complete')
check('consumer_matches_committed_outbox', metrics['exact_outbox_match'])
check('ordered_complete_stream', [e['seq'] for e in events]==list(range(1,len(events)+1)) and result['stream']['last_seq']==len(events) and result['stream']['pending']==0)
check('clean_capture_and_transport', not result['stream']['capture_error'] and not result['stream']['transport_error'])
starts = [e['payload'] for e in records if e['event']=='worker.start']
exits = [e['payload'] for e in records if e['event']=='worker.exit']
check('three_owned_worker_lifecycles', len(starts)==3 and len(exits)==3 and {e['pid'] for e in starts}=={e['pid'] for e in exits})
check('all_workers_exited_successfully', all(e['exit_code']==0 and e['status']=='done' for e in exits))
check('no_unobserved_retirement', not any(e['event']=='worker.reap_pending' for e in records))
check('worker_route_is_subscription', all(e['provider']=='openai-codex' and e['model']=='gpt-5.6-luna' for e in starts))
living = subprocess.run(['ps','-p',','.join(str(e['pid']) for e in starts),'-o','pid='],capture_output=True,text=True)
check('workers_are_no_longer_running', not living.stdout.strip())
check('root_is_no_longer_running', not subprocess.run(['ps','-p',str(launch['pid']),'-o','pid='],capture_output=True,text=True).stdout.strip())
check('closing_event_is_last', records[-1]['event']=='stream.closed')
check('first_text_was_live', min(metrics['first_text_seconds_by_pid'].values()) < metrics['elapsed_seconds']-5)

attempts = {e['payload']['attempt_id']:e for e in records if e['event']=='provider.attempt.started'}
finishes = {e['payload']['attempt_id']:e for e in records if e['event']=='provider.attempt.finished'}
check('all_provider_attempts_have_terminal_records', len(attempts)==3 and attempts.keys()==finishes.keys())
check('provider_http_and_protocol_success', all(e['payload']['http_status']==200 and e['payload']['effective_curl_code']==0 and e['payload']['terminal_received'] and not e['payload']['protocol_error'] for e in finishes.values()))
bodies = collections.defaultdict(bytearray)
for e in records:
    if e['event']!='provider.response.body': continue
    payload=e['payload']; body=bodies[payload['attempt_id']]
    chunk=base64.b64decode(payload['data'],validate=True)
    check('body_offsets_are_contiguous', payload['offset']==len(body) and payload['byte_length']==len(chunk))
    body.extend(chunk)
check('every_received_provider_byte_retained', all(len(bodies[key])==finishes[key]['payload']['received_body_bytes'] for key in attempts))
roles = {e['pid']:role for e,role in zip(sorted(starts,key=lambda e:e['id']),('architect','skeptic','synthesizer'))}
body_hashes={}
for key,body in bodies.items():
    pid=attempts[key]['pid']; role=roles[pid]
    body_hashes[key]=hashlib.sha256(body).hexdigest()
    (directory/f'{role}.provider-body.bin').write_bytes(body)
    decoded=[]
    for line in bytes(body).decode('utf-8').splitlines():
        if not line.startswith('data:'): continue
        payload=line[5:].strip()
        if payload and payload!='[DONE]': decoded.append(json.loads(payload))
    projected=[e['payload'] for e in records if e['pid']==pid and e['event']=='provider.responses.event' and isinstance(e['payload'],dict) and 'type' in e['payload']]
    check(f'{role}_parsed_events_equal_raw_provider_events', decoded==projected)
    text=''.join(e.get('delta','') for e in decoded if e.get('type')=='response.output_text.delta')
    check(f'{role}_has_real_provider_text', bool(text.strip()))
    (directory/f'{role}.md').write_text(text+'\n')
models={e['payload']['governance_model'] for e in records if e['event']=='tool.started'}
summary={'passed':True,'checks':checks,'binary_sha256':launch['binary_sha256'],
    'source_sha256':hashlib.sha256((directory/'source.lingo').read_bytes()).hexdigest(),
    'events':len(events),'workers':starts,'governance_models':sorted(models),
    'provider_body_sha256':body_hashes,'metrics':metrics}
(directory/'swarm-verification.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({'passed':True,'checks':len(checks),'events':len(events),'governance_models':sorted(models),'report':str(directory)},indent=2))
