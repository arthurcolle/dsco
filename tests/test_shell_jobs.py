#!/usr/bin/env python3
"""Production dispatch, separate clients, no provider and no UI."""
import json, os, subprocess, sys, time
from pathlib import Path
binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else './dsco').resolve())
env = dict(os.environ, DSCO_NO_AUTO_SUPERVISE='1', DSCO_PRICING_OFFLINE='1')
def call(args, tool='bash', extra=None):
    p = subprocess.run([binary, '--tool-exec-raw', tool, json.dumps(args)], env=dict(env, **(extra or {})), capture_output=True, text=True, timeout=90)
    assert p.returncode == 0, (p.stdout, p.stderr)
    return json.loads(p.stdout)
def start(command, **kw):
    return call(dict(command=command, run_in_background=True, **kw))['job_id']
def wait(job):
    for _ in range(10):
        r = call(dict(action='wait', job_id=job, wait_seconds=2))
        if r['status'] not in ('starting', 'running'): return r
    raise AssertionError('job did not finish')
j = start('sleep 1; printf durable')
r = wait(j); assert r['status'] == 'completed' and r['exit_code'] == 0 and r['output'] == 'durable', r
r = call(dict(action='output', job_id=j, offset=3, max_bytes=2)); assert r['output'] == 'ab' and r['next_offset'] == 5, r
j = start('exit 7'); r = wait(j); assert r['status'] == 'failed' and r['exit_code'] == 7, r
j = start('sleep 10', timeout=1); r = wait(j); assert r['status'] == 'timed_out', r
j = start('sleep 30'); r = call(dict(action='cancel', job_id=j)); assert r['status'] == 'cancelled', r
r = call(dict(action='list')); assert any(x.get('job_id') == j for x in r['jobs']), r
p = subprocess.run([binary, '--tool-exec-raw', 'bash', json.dumps(dict(command='touch /tmp/DSCO_SHOULD_NOT_EXIST', run_in_background=True))], env=dict(env, DSCO_ALLOW_RUN='0'), capture_output=True, text=True, timeout=10)
assert p.returncode != 0 and 'disabled' in p.stdout.lower(), (p.stdout, p.stderr)
# The regression was a quiet script being killed at 60 seconds.
p = subprocess.run([binary, '--tool-exec-raw', 'bash', json.dumps(dict(command='sleep 65; printf quiet-ok'))], env=env, capture_output=True, text=True, timeout=90)
assert p.returncode == 0 and 'quiet-ok' in p.stdout and '[killed:' not in p.stdout, (p.stdout, p.stderr)
print(json.dumps(dict(ok=True, binary=binary, checks=['restart survival', 'output offsets', 'exit code', 'timeout', 'cancel', 'list', 'exec denial', '65-second quiet foreground default'])))
