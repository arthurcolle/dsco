#!/usr/bin/env python3
"""Actual selected-server CLI calls must pass the public capability gate."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def server(log):
    for line in sys.stdin:
        request = json.loads(line)
        if 'id' not in request:
            continue
        method = request.get('method')
        if method == 'initialize':
            result = dict(protocolVersion='2024-11-05', capabilities={'tools': {}},
                          serverInfo={'name': 'owned-gate-fixture', 'version': '1'})
        elif method == 'tools/list':
            result = {'tools': [dict(name='fixture_read', description='Read the owned fixture value.',
                                    inputSchema={'type': 'object', 'properties': {}})]}
        elif method == 'tools/call':
            with open(log, 'a') as stream:
                stream.write(json.dumps(request['params'])+'\n')
            result = {'content': [{'type': 'text', 'text': 'OWNED_SELECTED_MCP_PROOF'}], 'isError': False}
        else:
            result = {}
        print(json.dumps(dict(jsonrpc='2.0', id=request['id'], result=result)), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', default='./dsco')
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix='dsco-selected-gate-') as directory:
        root = Path(directory)
        log = root/'calls.jsonl'
        config = {'mcpServers': {'owned-selected-gate': {
            'command': sys.executable, 'args': [str(Path(__file__).resolve()), '--server', str(log)]}}}
        (root/'.mcp.json').write_text(json.dumps(config))
        env = os.environ.copy()
        for key in list(env):
            if key.startswith('DSCO_ALLOW_') or key in ('DSCO_GOV_BYPASS', 'DSCO_MCP_GOV_PINNED'):
                env.pop(key)
        env.update(DSCO_MCP_SERVER='owned-selected-gate', DSCO_TRUST_TIER='trusted',
                   DSCO_GOV_MODEL='standard', DSCO_PRICING_OFFLINE='1',
                   DSCO_NO_AUTO_SUPERVISE='1')
        name = 'mcp__owned-selected-gate__fixture_read'
        for raw in (False, True):
            command = [binary, '--tool-exec-raw' if raw else '--tool-exec', name, '{}']
            env['DSCO_ALLOW_NET'] = '0'
            denied = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True, timeout=20)
            assert denied.returncode != 0, denied.stdout
            assert 'net' in denied.stdout.lower() and 'disabled' in denied.stdout.lower(), denied.stdout
            assert not log.exists(), 'denied call reached the MCP server'
        env['DSCO_ALLOW_NET'] = '1'
        allowed = subprocess.run([binary, '--tool-exec', name, '{}'], cwd=root, env=env,
                                 capture_output=True, text=True, timeout=20)
        assert allowed.returncode == 0, (allowed.stdout, allowed.stderr)
        response = json.loads(allowed.stdout)
        assert response['ok'] and 'OWNED_SELECTED_MCP_PROOF' in str(response['result']), response
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        assert len(calls) == 1 and calls[0]['name'] == 'fixture_read', calls
        print(json.dumps(dict(ok=True, binary=binary,
                              sha256=hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
                              checks=['JSON and raw selected MCP calls denied before transport',
                                      'permitted selected MCP call executes exactly once'],
                              home_preserved=True)))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--server':
        server(sys.argv[2])
    else:
        main()
