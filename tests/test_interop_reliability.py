#!/usr/bin/env python3
"""Offline protocol and parent-only cancellation with owned real processes."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

CASES = 0
EVIDENCE = []


def gone(pid):
    deadline = time.monotonic() + 1.5
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        time.sleep(.01)
    return False


def main():
    global CASES
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--evidence')
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix='dsco-interop-reliability-') as td:
        root = Path(td)
        env = {'HOME': td, 'TMPDIR': td, 'PATH': f'{td}:/usr/bin:/bin',
               'DSCO_ENV_FILE': '/dev/null', 'DSCO_PRICING_OFFLINE': '1',
               'DSCO_NO_AUTO_SUPERVISE': '1', 'DSCO_DURABLE_AUTOWAKE': '0',
               'DSCO_DISABLE_SHARED_HOME_OAUTH': '1'}
        fixtures = [
            ('codex', [{'type': 'turn.failed', 'error': {'message': 'fixture quota exhausted'}}], 'provider_error'),
            ('codex', [{'type': 'error', 'message': 'fixture authentication failure'}], 'provider_error'),
            ('claude-code', [{'type': 'result', 'subtype': 'error_during_execution', 'is_error': True, 'errors': ['fixture tool denied']}], 'provider_error'),
            ('claude-code', [{'type': 'result', 'subtype': 'success', 'is_error': True}], 'provider_error'),
            ('codex', [{'type': 'turn.completed', 'usage': {'input_tokens': 2, 'cached_input_tokens': 0, 'output_tokens': 1}}], 'success'),
            ('claude-code', [{'type': 'result', 'subtype': 'success', 'is_error': False}], 'success'),
            ('codex', [{'type': 'item.completed', 'item': {'type': 'agent_message', 'text': 'success turn.completed'}}], 'incomplete'),
            ('claude-code', [{'type': 'result', 'subtype': 'success', 'is_error': 'false'}], 'incomplete'),
            ('opencode', [{'type': 'fixture', 'message': 'success'}], 'incomplete'),
            ('omp', [{'type': 'fixture', 'message': 'success'}], 'incomplete'),
            ('hermes', [{'type': 'fixture', 'message': 'success'}], 'incomplete'),
        ]
        for adapter, events, expected in fixtures:
            fake = root / ('claude' if adapter == 'claude-code' else adapter)
            stream = '\n'.join(json.dumps(row) for row in events) + '\n'
            fake.write_text('#!/usr/bin/python3\nimport sys\nsys.stdout.write(' + repr(stream) + ')\n')
            fake.chmod(0o755)
            proc = subprocess.run([binary, 'interop', 'run', adapter, '--prompt', 'fixture', '--json'],
                                  env=env, cwd=root, capture_output=True, text=True, timeout=4)
            receipt = json.loads(proc.stdout)
            assert receipt['exit_code'] == 0 and receipt['process_outcome'] == 'exit_zero', receipt
            assert receipt['outcome'] == expected, receipt
            assert receipt['ok'] == (expected == 'success'), receipt
            assert (proc.returncode == 0) == receipt['ok']
            assert receipt['semantic_acceptance'] == receipt['route_verification'] == 'unverified'
            assert receipt['output'] == stream
            if expected == 'provider_error':
                assert receipt['provider_failure_event'] == events[0]
                EVIDENCE.append({'fixture': adapter, 'receipt': receipt, 'returncode': proc.returncode})
            CASES += 1
        # Truncated/malformed/contradictory stream cannot prove completion;
        # explicit failure remains visible even when later evidence is missing.
        codex = root / 'codex'
        for text, bound, expected in [
            ('{"type":"turn.completed","usage":{}}\n' + 'x' * 512, '64', 'incomplete'),
            ('{"type":"turn.completed","usage":{}}\n{broken\n', '4096', 'incomplete'),
            ('{"type":"turn.failed","error":{"message":"fixture fail"}}\n' + 'x' * 512, '128', 'provider_error'),
        ]:
            codex.write_text('#!/usr/bin/python3\nimport sys\nsys.stdout.write(' + repr(text) + ')\n'); codex.chmod(0o755)
            proc = subprocess.run([binary, 'interop', 'run', 'codex', '--prompt', 'fixture', '--json', '--max-output', bound],
                                  env=env, cwd=root, capture_output=True, text=True, timeout=4)
            receipt = json.loads(proc.stdout)
            assert not receipt['ok'] and receipt['outcome'] == expected, receipt
            CASES += 1
        # Fixture identity file is published only after signal handlers/children
        # are ready, preventing startup races in parent-only interruption tests.
        helper = root / 'helper.py'
        helper.write_text('''import json,os,signal,sys,time,subprocess
mode,identity=sys.argv[1:3]
child=None
if mode in ('descendant','escaped','orphan'):
    ready=identity+'.ready'
    child=subprocess.Popen([sys.executable,'-c',
        "import os,signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);open("+repr(ready)+",'w').write('ready');time.sleep(10)"],
        start_new_session=mode in ('escaped','orphan'))
    while not os.path.exists(ready):time.sleep(.005)
if mode in ('wait','descendant','escaped'):
    signal.signal(signal.SIGTERM,signal.SIG_IGN)
if mode=='flush':
    def flush(sig,frame):
        print('FINAL-FIXTURE-DIAGNOSTIC',flush=True)
        sys.exit(0)
    signal.signal(signal.SIGTERM,flush)
open(identity,'w').write(json.dumps([os.getpid()]+([child.pid] if child else [])))
if mode in ('normal','orphan'):sys.exit(0)
while True:time.sleep(.01)
''')
        runs = [('normal', 0, 0), ('wait', signal.SIGINT, 1),
                ('wait', signal.SIGTERM, 1), ('wait', signal.SIGINT, 5),
                ('flush', signal.SIGINT, 1), ('wait', 0, 0),
                ('descendant', signal.SIGTERM, 1)]
        if os.sys.platform.startswith(('darwin', 'linux')):
            runs += [('escaped', signal.SIGINT, 1), ('orphan', 0, 0)]
        for index, (mode, sig, repeats) in enumerate(runs):
            identity = root / f'pids-{index}'
            pids = []
            parent = subprocess.Popen([binary, 'interop', 'run', 'generic', '--prompt', 'fixture',
                '--timeout-ms', '120' if not sig and mode != 'normal' else '4000', '--json',
                '--', '/usr/bin/python3', str(helper), mode, str(identity)],
                env=env, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, start_new_session=True)
            started = time.monotonic()
            try:
                deadline = started + 3
                while not identity.exists() and time.monotonic() < deadline:
                    assert parent.poll() is None
                    time.sleep(.005)
                assert identity.exists()
                pids = json.loads(identity.read_text())
                for _ in range(repeats):
                    os.kill(parent.pid, sig)
                    time.sleep(.01)
                stdout, stderr = parent.communicate(timeout=3)
                receipt = json.loads(stdout)
                assert time.monotonic() - started < 2.5, (mode, receipt)
                assert receipt['root_reaped'] and receipt['unreaped_pid'] == 0, receipt
                if mode == 'orphan':
                    # Detached child outlives/reparents before enumeration; no
                    # arbitrary process sweep. The held stream is incomplete.
                    assert not receipt['cleanup_complete'] and not receipt['ok'], receipt
                    assert receipt['outcome'] == 'incomplete', receipt
                    assert receipt['escaped_orphans'] == 'unverified', receipt
                    assert gone(pids[0])
                else:
                    assert receipt['cleanup_complete'], receipt
                    assert all(gone(pid) for pid in pids), (mode, pids, receipt)
                    if sig:
                        assert receipt['cancelled'] and receipt['cancel_signal'] == sig, receipt
                        assert receipt['outcome'] == 'cancelled' and not receipt['ok'], receipt
                        assert parent.returncode == 128 + sig
                    elif mode == 'normal':
                        assert receipt['ok'] and receipt['outcome'] == 'success', receipt
                    else:
                        assert receipt['timed_out'] and receipt['outcome'] == 'timeout', receipt
                    if mode == 'flush':
                        assert receipt['exit_code'] == 0
                        assert 'FINAL-FIXTURE-DIAGNOSTIC' in receipt['output']
                EVIDENCE.append({'fixture': mode, 'signal': sig, 'repeats': repeats,
                                 'receipt': receipt, 'returncode': parent.returncode})
                CASES += 1
            finally:
                if parent.poll() is None:
                    parent.kill()
                    parent.wait(timeout=2)
                if not pids and identity.exists():
                    pids = json.loads(identity.read_text())
                for pid in pids:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                parent.communicate(timeout=2)
    if args.evidence:
        Path(args.evidence).write_text(json.dumps({'cases_passed': CASES, 'fixtures': EVIDENCE}, indent=2))
    print(f'interop reliability: {CASES} offline protocol/lifecycle cases passed')


if __name__ == '__main__':
    main()
