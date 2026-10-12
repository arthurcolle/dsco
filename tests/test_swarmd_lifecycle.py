#!/usr/bin/env python3
"""Offline detached swarm lifecycle: the supervisor owns and reaps its workers."""
import json
import os
import pathlib
import subprocess
import tempfile
import time

repo = pathlib.Path(__file__).resolve().parents[1]
binary = repo / 'build' / 'swarmd_fixture'
subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-D_GNU_SOURCE',
                '-DDSCO_SWARMD_TEST', '-Iinclude', '-o', str(binary),
                'tests/swarmd_fixture.c', 'src/swarm_daemon.c', 'src/swarm_reactor.c', 'src/json_util.c',
                '-lm'], cwd=repo, check=True, capture_output=True, text=True)

def state(run):
    return json.loads((run / 'state.json').read_text())

def until(predicate, deadline=10):
    end = time.monotonic() + deadline
    while time.monotonic() < end:
        result = predicate()
        if result: return result
        time.sleep(.05)
    raise AssertionError('timed out waiting for supervised workers')

def begin(root, name, *tasks):
    started = time.monotonic()
    p = subprocess.run([str(binary), 'swarmd', 'start', '--name', name,
                        '--workdir', str(root), '--', *tasks],
                       capture_output=True, text=True, timeout=3, check=True)
    assert time.monotonic() - started < 2, 'start kept a daemon stdio pipe open'
    reply = json.loads(p.stdout)
    run = pathlib.Path(reply['run_dir'])
    assert run.is_dir() and len(state(run)['workers']) == len(tasks)
    return reply, run

with tempfile.TemporaryDirectory(prefix='dsco-swarmd-test-') as tmp:
    root = pathlib.Path(tmp)
    runs = []
    try:
        reply, run = begin(root, 'own-workers', 'echo WORK_OK; sleep 2; exit 0',
                           'echo WORK_FAIL; sleep 2; exit 7')
        runs.append((reply, run))
        running = until(lambda: (s if s['supervisor_pid'] == reply['supervisor_pid']
                                 and all(w['pid'] > 1 and w['status'] == 'running'
                                         for w in s['workers']) else None) if
                        (s := state(run)) else None)
        for worker in running['workers']:
            parent = int(subprocess.check_output(['ps', '-o', 'ppid=', '-p', str(worker['pid'])], text=True))
            assert parent == reply['supervisor_pid'], (parent, reply, worker)
        terminal = until(lambda: (s if [w['status'] for w in s['workers']] == ['done', 'failed']
                                  else None) if (s := state(run)) else None)
        assert [w['exit_code'] for w in terminal['workers']] == [0, 7], terminal
        for worker, marker in zip(terminal['workers'], ('WORK_OK', 'WORK_FAIL')):
            assert marker in pathlib.Path(worker['log_path']).read_text()
            assert json.loads(pathlib.Path(worker['result_path']).read_text())['status'] == worker['status']
        events = [json.loads(x) for x in (run / 'events.jsonl').read_text().splitlines()]
        assert len([e for e in events if e['event'] == 'worker.terminal']) == 2
        until(lambda: 'supervisor.exited' in (run / 'events.jsonl').read_text())
        print('PASS: detached supervisor parents both workers, reaps real exit codes, persists logs/results')

        reply, run = begin(root, 'abort-workers', 'echo WAITING; sleep 30')
        runs.append((reply, run))
        worker = until(lambda: (w if w['pid'] > 1 and w['status'] == 'running' else None)
                       if (w := state(run)['workers'][0]) else None)
        until(lambda: pathlib.Path(worker['log_path']).exists())
        subprocess.run([str(binary), 'swarmd', 'abort', '--run', str(run)],
                       capture_output=True, text=True, check=True, timeout=3)
        terminal = until(lambda: (s if s['workers'][0]['status'] == 'aborted' else None)
                         if (s := state(run)) else None, deadline=9)
        assert terminal['abort_requested'] is True
        assert json.loads(pathlib.Path(worker['result_path']).read_text())['status'] == 'aborted'
        until(lambda: 'supervisor.exited' in (run / 'events.jsonl').read_text())
        print('PASS: abort terminates owned worker and persists an aborted result')
    finally:
        for reply, run in runs:
            s = state(run)
            if any(w['status'] in ('queued', 'running') for w in s['workers']):
                subprocess.run([str(binary), 'swarmd', 'abort', '--run', str(run)],
                               capture_output=True, timeout=3)
                for _ in range(90):
                    if all(w['status'] not in ('queued', 'running') for w in state(run)['workers']): break
                    time.sleep(.1)
                else:
                    for w in state(run)['workers']:
                        if w['status'] == 'running' and w['pgid'] > 1: os.killpg(w['pgid'], 9)
                    os.kill(reply['supervisor_pid'], 9)
