#!/usr/bin/env python3
"""Local MCP proof: raw Unicode JSON escapes survive dispatch to Bash/write_file."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--expect-bug', action='store_true')
    args = ap.parse_args()
    binary = args.binary.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    with tempfile.TemporaryDirectory(prefix='dsco-unicode-mcp-') as home:
        env = {'HOME': home, 'PATH': '/usr/bin:/bin', 'DSCO_ENV_FILE': '/dev/null',
               'DSCO_PRICING_OFFLINE': '1', 'DSCO_ALLOW_RUN': '1', 'DSCO_ALLOW_WRITE': '1',
               'DSCO_MCP_HEADLESS': '0', 'TERM': 'dumb'}
        with (output / 'stderr.txt').open('w') as err:
            # Both ends of this pipe and all files are owned local fixtures.
            # Capability dispatch is retained; trusted tier permits host Bash.
            proc = subprocess.Popen([str(binary), 'mcp', 'serve', '--tier', 'trusted'],
                                    cwd=home, env=env, stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, stderr=err, text=True, bufsize=1)

            def call(wire, request_id):
                proc.stdin.write(wire + '\n')
                proc.stdin.flush()
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    assert select.select([proc.stdout], [], [], max(.01, deadline-time.monotonic()))[0]
                    line = proc.stdout.readline()
                    assert line, 'MCP exited before response'
                    result = json.loads(line)
                    if result.get('id') == request_id:
                        return result
                raise AssertionError('MCP response deadline')

            try:
                call(json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
                                 'params': {'protocolVersion': '2024-11-05', 'capabilities': {},
                                            'clientInfo': {'name': 'unicode-fixture', 'version': '1'}}}), 1)
                cases = [
                    ('shell-characters', 'bash', r'''{"command":"printf '%s' '\u003e\u0026'","timeout":5}''', '>&'),
                    ('literal-backslash-u', 'bash', r'''{"command":"printf '%s' '\\u003e\\u0026'","timeout":5}''', r'\u003e\u0026'),
                    ('escaped-heredoc', 'bash', r'''{"command":"cat \u003c\u003c'EOF'\nFIXTURE\nEOF\n","timeout":5}''', 'FIXTURE\n'),
                    ('literal-heredoc', 'bash', r'''{"command":"cat <<'EOF'\nFIXTURE\nEOF\n","timeout":5}''', 'FIXTURE\n'),
                    ('source-Unicode', 'write_file', r'''{"path":"fixture.py","content":"value = \"\u05d0\"\n"}''', None),
                    ('source-literal-escape', 'write_file', r'''{"path":"fixture.py","content":"value = \"\\u05d0\"\n"}''', None),
                    ('source-surrogate-pair', 'write_file', r'''{"path":"fixture.py","content":"value = \"\ud83d\ude00\"\n"}''', None),
                ]
                for request_id, (name, tool, raw, expected) in enumerate(cases, 2):
                    wire = ('{"jsonrpc":"2.0","id":' + str(request_id) +
                            ',"method":"tools/call","params":{"name":' + json.dumps(tool) +
                            ',"arguments":' + raw + '}}')
                    reply = call(wire, request_id)
                    content = ''.join(x.get('text', '') for x in reply.get('result', {}).get('content', []))
                    ok = not reply.get('error') and not reply.get('result', {}).get('isError')
                    row = {'case': name, 'raw_arguments_json': raw, 'reply': reply}
                    if tool == 'bash':
                        row['correct'] = ok and content == expected
                    else:
                        written = (Path(home) / 'fixture.py').read_text()
                        row['written_source'] = written
                        row['correct'] = ok and written == json.loads(raw)['content']
                    rows.append(row)
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=3)
    result = {'binary': str(binary), 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'cases': rows, 'correct': sum(row['correct'] for row in rows), 'total': len(rows),
              'expected_bug': args.expect_bug}
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'cases'}, indent=2))
    assert result['correct'] < result['total'] if args.expect_bug else result['correct'] == result['total'], result


if __name__ == '__main__':
    main()
