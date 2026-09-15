#!/usr/bin/env python3
"""Run a real Lingo program; retain every IPC frame and verify it against its outbox."""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import socket
import sqlite3
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

# Match C clock_gettime(CLOCK_MONOTONIC). On macOS Python monotonic_ns()
# uses a different suspend-time convention and cannot be subtracted from it.
def clock_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)

def process_identity(pid):
    result = subprocess.run(['ps', '-p', str(pid), '-o', 'lstart=', '-o', 'comm='],
                            capture_output=True, text=True, timeout=2)
    return result.stdout.strip() if result.returncode == 0 else None


def capture(binary, command, directory, env=None, timeout=240, quiet=False, expect_capture_error=False):
    """command is a Lingo subcommand tail. No shell, credentials or provider defaults."""
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    child.setblocking(False)
    child.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 256 * 1024)
    database = directory / 'events.sqlite'
    argv = [str(binary), 'lingo', *command, '--events-fd', str(child.fileno())]
    if command[0] != 'replay': argv += ['--events-db', str(database)]
    started = clock_ns()
    records, latencies, first_output, workers = [], [], {}, {}
    with (directory / 'stdout.json').open('wb') as stdout, (directory / 'stderr.log').open('wb') as stderr:
        proc = subprocess.Popen(argv, cwd=directory, env=env, stdout=stdout, stderr=stderr,
                                pass_fds=(child.fileno(),), start_new_session=True)
        child.close()
        (directory / 'launch.json').write_text(json.dumps({
            'argv': argv, 'pid': proc.pid, 'binary': str(binary),
            'binary_sha256': hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
            'started_monotonic_ns': started,
        }, indent=2) + '\n')
        pending = b''
        try:
            with (directory / 'events.ndjson').open('wb') as journal:
                while True:
                    if clock_ns() - started > timeout * 1e9:
                        raise TimeoutError(f'Lingo exceeded {timeout}s; report: {directory}')
                    readable, _, _ = select.select([parent], [], [], 0.1)
                    if not readable: continue
                    data = parent.recv(256 * 1024)
                    if not data: break
                    pending += data
                    while b'\n' in pending:
                        line, pending = pending.split(b'\n', 1)
                        received = clock_ns()
                        event = json.loads(line)
                        record = event['record']
                        if records:
                            assert event['seq'] == records[-1]['seq'] + 1, 'IPC sequence discontinuity'
                        records.append(event)
                        journal.write(line + b'\n')
                        lag = (received - int(record['monotonic_ns'])) / 1e6
                        assert lag >= 0, 'producer and consumer clock domains differ'
                        latencies.append(lag)
                        name, payload = record['event'], record['payload']
                        if name == 'worker.start': workers[payload['pid']] = process_identity(payload['pid'])
                        if name == 'provider.responses.event' and payload.get('type') == 'response.output_text.delta':
                            pid = record['pid']
                            if pid not in first_output:
                                first_output[pid] = (received - started) / 1e9
                                if not quiet:
                                    print(f"{first_output[pid]:7.3f}s pid={pid} first text over IPC ({lag:.3f}ms): {payload.get('delta', '')!r}", flush=True)
                        if not quiet and name in ('worker.start', 'worker.exit', 'stream.closed'):
                            print(f"{(received-started)/1e9:7.3f}s #{event['seq']} {name}: {json.dumps(payload)}", flush=True)
                assert not pending, 'truncated IPC frame (replay is required)'
                journal.flush()
                os.fsync(journal.fileno())
            code = proc.wait(timeout=10)
        except BaseException:
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try: proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait()
            # Root termination does not prove a separately grouped child exited.
            # Check its observed identity before retiring any surviving process.
            for pid, identity in workers.items():
                if identity and process_identity(pid) == identity:
                    try:
                        if os.getpgid(pid) == pid: os.killpg(pid, signal.SIGKILL)
                    except ProcessLookupError: pass
            raise
        finally:
            parent.close()
    result = json.loads((directory / 'stdout.json').read_text())
    receipt = result if command[0] == 'replay' else result['stream']
    retained_db = Path(receipt['db_path'])
    with sqlite3.connect(f'file:{retained_db}?mode=ro', uri=True) as db:
        after = int(command[command.index('--after') + 1]) if '--after' in command else 0
        retained = [(seq, json.loads(record)) for seq, record in db.execute(
            'SELECT seq,record FROM events WHERE seq>? AND seq<=? ORDER BY seq', (after, receipt['last_seq']))]
    assert [(e['seq'], e['record']) for e in records] == retained, 'IPC content differs from committed outbox'
    assert receipt['pending'] == 0 and receipt['sent_seq'] == receipt['last_seq'], receipt
    assert receipt['capture_error'] == expect_capture_error and not receipt['transport_error'], receipt
    elapsed = (clock_ns() - started) / 1e9
    ordered = sorted(latencies)
    def percentile(fraction): return ordered[min(int((len(ordered)-1)*fraction), len(ordered)-1)] if ordered else None
    counts = collections.Counter((e['record']['source'], e['record']['event']) for e in records)
    metrics = {'exit_code': code, 'events': len(records), 'elapsed_seconds': elapsed,
        'consumer_committed_seq': records[-1]['seq'] if records else after,
        'exact_outbox_match': True, 'first_text_seconds_by_pid': first_output,
        'emission_to_consumer_ms': {'p50': percentile(.5), 'p95': percentile(.95), 'max': max(latencies, default=0)},
        'event_counts': {f'{source}:{name}': count for (source, name), count in sorted(counts.items())}}
    (directory / 'verification.json').write_text(json.dumps(metrics, indent=2) + '\n')
    return result, records, metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('program', type=Path)
    parser.add_argument('--binary', type=Path, default=ROOT / 'dsco')
    parser.add_argument('--args', default='{}')
    parser.add_argument('--report', type=Path)
    opts = parser.parse_args()
    json.loads(opts.args)
    report = opts.report or Path(tempfile.mkdtemp(prefix='lingo-ipc-', dir=ROOT / 'reports'))
    result, _, metrics = capture(opts.binary.resolve(), ['run', str(opts.program.resolve()), opts.args], report)
    print(json.dumps({'report': str(report.resolve()), **metrics}, indent=2))
    return metrics['exit_code']


if __name__ == '__main__':
    raise SystemExit(main())
