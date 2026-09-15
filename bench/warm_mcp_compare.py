#!/usr/bin/env python3
"""Compare warm local MCP tool hosts; no model requests or remote tools."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import signal
import statistics
import subprocess
import tempfile
import threading
import time


class Host:
    def __init__(self, name, argv, cwd, env, output, session, protocol='mcp'):
        self.name, self.argv, self.seq = name, argv, 0
        self.replies = queue.Queue()
        self.log = (output / f'{session}-{name}-wire.jsonl').open('w')
        self.err = (output / f'{session}-{name}-stderr.txt').open('w')
        started = time.perf_counter_ns()
        self.proc = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.err,
                                     text=True, bufsize=1, start_new_session=True)
        def read():
            for line in self.proc.stdout:
                try:
                    self.replies.put(json.loads(line))
                except ValueError:
                    self.replies.put({'unparsed_stdout': line})
            self.replies.put({'eof': self.proc.poll()})
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        init = {'capabilities': {}, 'clientInfo': {'name': 'warm-local-benchmark', 'version': '1'}}
        if protocol == 'mcp':
            init['protocolVersion'] = '2024-11-05'
        else:
            init['capabilities']['experimentalApi'] = True
        self.initialized, _ = self.call('initialize', init)
        self.startup_ms = (time.perf_counter_ns() - started) / 1e6
        self.proc.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized' if protocol == 'mcp' else 'initialized'}) + '\n')
        self.proc.stdin.flush()
        if protocol != 'mcp':
            return
        catalog, self.catalog_ms = self.call('tools/list')
        self.tools = {t['name']: t for t in catalog['result']['tools']}

    def call(self, method, params=None):
        self.seq += 1
        req = {'jsonrpc': '2.0', 'id': self.seq, 'method': method}
        if params is not None:
            req['params'] = params
        wire = json.dumps(req, separators=(',', ':')) + '\n'
        started = time.perf_counter_ns()
        self.proc.stdin.write(wire)
        self.proc.stdin.flush()
        deadline = time.monotonic() + 20
        while True:
            reply = self.replies.get(timeout=max(.001, deadline - time.monotonic()))
            if 'eof' in reply or 'unparsed_stdout' in reply:
                raise RuntimeError(reply)
            if reply.get('id') == self.seq:
                break
        elapsed = (time.perf_counter_ns() - started) / 1e6
        self.log.write(json.dumps({'request': req, 'response': reply, 'elapsed_ms': elapsed}) + '\n')
        self.log.flush()
        if 'error' in reply or reply.get('result', {}).get('isError'):
            raise RuntimeError(reply)
        return reply, elapsed

    def close(self):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=3)
        self.reader.join(timeout=1)
        self.log.close()
        self.err.close()
        return self.proc.returncode


def percentile(values, fraction):
    values = sorted(values)
    index = (len(values) - 1) * fraction
    lo = int(index)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (index - lo)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dsco', default=str(Path(__file__).resolve().parents[1] / 'dsco'))
    ap.add_argument('--claude', default=shutil.which('claude'))
    ap.add_argument('--output', required=True)
    ap.add_argument('--sessions', type=int, default=2)
    ap.add_argument('--requests', type=int, default=50)
    ap.add_argument('--gov-model', choices=['standard','none'], default='standard')
    args = ap.parse_args()
    if args.sessions < 1 or args.requests < 1 or not args.claude:
        ap.error('positive counts and both binaries required')
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    paths = {'dsco': str(Path(args.dsco).resolve()), 'claude': str(Path(args.claude).resolve())}
    env = os.environ.copy()
    # Explicit local operation grants still pass the normal DSCO capability gate.
    for key in ['DSCO_GOV_BYPASS', 'DSCO_ALLOW_EXFIL']:
        env.pop(key, None)
    env.update(DSCO_GOV_MODEL=args.gov_model, DSCO_ALLOW_READ='1', DSCO_ALLOW_RUN='1',
               DSCO_ALLOW_NET='0', DSCO_ALLOW_CONTROL='0', DSCO_ALLOW_SECRETS='0',
               DSCO_MCP_AUTOCONNECT='0', DSCO_MCP_HEADLESS='0',
               DSCO_ENV_FILE='/dev/null', DSCO_NO_KEYCHAIN='1',
               DSCO_NO_WORKSPACE_BOOTSTRAP='1')
    records, starts, bindings = [], [], {}
    try:
        with tempfile.TemporaryDirectory(prefix='dsco-warm-mcp-') as temp:
            cwd = Path(temp).resolve()
            fixture = cwd / 'fixture.txt'
            tokens = [f'warm-mcp-row-{i:03d}-a7c49b' for i in range(32)]
            fixture.write_text('\n'.join(tokens) + '\n')
            # Claude Read caches already-read files and returns file_unchanged.
            # Precreate identical separate files so every scored request reads
            # full content. Both hosts read the same file in each paired sample.
            fixtures = {}
            for session in range(args.sessions):
                for index in range(args.requests + 3):
                    path = cwd / f'fixture-{session}-{index}.txt'
                    path.write_text('\n'.join(tokens) + '\n')
                    fixtures[session, index] = path
            for session in range(args.sessions):
                hosts = []
                try:
                    for name in (['dsco', 'claude'] if session % 2 == 0 else ['claude', 'dsco']):
                        argv = [paths[name], 'mcp', 'serve']
                        if name == 'dsco':
                            argv[1:1] = ['--gov-model', args.gov_model]
                            argv += ['--toolsets', 'all', '--tier', 'trusted']
                        host = Host(name, argv, cwd, env, out, session)
                        hosts.append(host)
                        starts.append({'session': session, 'host': name, 'argv': argv,
                                       'cwd': str(cwd), 'initialize_ms': host.startup_ms,
                                       'tools_list_ms': host.catalog_ms,
                                       'server': host.initialized['result']})
                        ops = {'read': ('read_file', {'path': str(fixture)}),
                               'pwd': ('bash', {'command': 'pwd'})} if name == 'dsco' else {
                                   'read': ('Read', {'file_path': str(fixture)}),
                                   'pwd': ('Bash', {'command': 'pwd'})}
                        host.ops = ops
                        bindings[name] = {op: {'tool': tool, 'arguments': params,
                                              'advertised_definition': host.tools[tool]}
                                          for op, (tool, params) in ops.items()}
                    for index in range(args.requests + 3):
                        for operation in (['read', 'pwd'] if index % 2 == 0 else ['pwd', 'read']):
                            for host in (hosts if index % 2 == 0 else hosts[::-1]):
                                tool, params = host.ops[operation]
                                if operation == 'read':
                                    params = {'path' if host.name == 'dsco' else 'file_path':
                                              str(fixtures[session, index])}
                                reply, elapsed = host.call('tools/call', {'name': tool, 'arguments': params})
                                result = reply['result']
                                text = '\n'.join(c.get('text', '') for c in result.get('content', []))
                                correct = all(text.count(t) == 1 for t in tokens) if operation == 'read' else str(cwd) in text
                                if not correct:
                                    raise AssertionError({'host': host.name, 'operation': operation, 'response': reply})
                                records.append({'session': session, 'host': host.name, 'operation': operation,
                                                'index': index - 3, 'warmup': index < 3,
                                                'elapsed_ms': elapsed, 'correct': correct})
                finally:
                    for host in hosts:
                        host.close()
        summary = {}
        for operation in ['read', 'pwd']:
            summary[operation] = {}
            for name in paths:
                samples = [r['elapsed_ms'] for r in records if r['host'] == name and r['operation'] == operation and not r['warmup']]
                summary[operation][name] = {'n': len(samples), 'p50_ms': statistics.median(samples),
                                            'p95_ms': percentile(samples, .95), 'max_ms': max(samples),
                                            'all_correct': True}
            summary[operation]['dsco_fraction_of_claude_p50'] = summary[operation]['dsco']['p50_ms'] / summary[operation]['claude']['p50_ms']
        (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(summary, indent=2))
    finally:
        (out / 'samples.json').write_text(json.dumps(records, indent=2) + '\n')
        (out / 'manifest.json').write_text(json.dumps({'starts': starts, 'bindings': bindings,
            'binary_sha256': {k: hashlib.sha256(Path(v).read_bytes()).hexdigest() for k,v in paths.items()},
            'requests_per_operation_per_session': args.requests, 'sessions': args.sessions,
            'warmups_per_operation_per_session': 3,
            'dsco_governance_model': args.gov_model,
            'read_fixture_policy': 'Distinct precreated identical files per pair; each host reads each file once to avoid Claude file_unchanged cache responses.',
            'scope': 'Sequential warm local tool-host RPC; no inference. Distinct tool implementations and output wrappers.',
            'settings': 'Saved settings/auth unchanged; inherited HOME; temporary current directory; DSCO local capability grants with network/control/secrets disabled.'}, indent=2) + '\n')


if __name__ == '__main__':
    main()
