#!/usr/bin/env python3
"""Offline MCP regression: advertised IPC payload alias and topic delivery."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

binary = str(Path(__file__).resolve().parents[1] / 'dsco')
with tempfile.TemporaryDirectory(prefix='dsco-ipc-alias-') as tmp:
    env = os.environ.copy()
    env['DSCO_IPC_DB'] = str(Path(tmp) / 'ipc.db')
    env['DSCO_MCP_SYNC'] = '1'
    requests = []
    def request(ident, method, params):
        requests.append(dict(jsonrpc='2.0', id=ident, method=method, params=params))
    def call(ident, args):
        request(ident, 'tools/call', dict(name='ipc', arguments=args))
    request(1, 'initialize', dict(protocolVersion='2024-11-05', capabilities={}, clientInfo=dict(name='ipc-regression', version='1')))
    requests.append(dict(jsonrpc='2.0', method='notifications/initialized'))
    request(2, 'tools/list', {})
    call(3, dict(action='send', topic='alias-test', message='alias-nonce'))
    call(4, dict(action='recv', topic='alias-test'))
    call(5, dict(action='send', topic='body-test', body='canonical', message='ignored'))
    call(6, dict(action='recv', topic='body-test'))
    call(7, dict(action='recv', topic='alias-test'))
    call(8, dict(action='send', topic='missing-test'))
    call(9, dict(action='send', topic='unicode-test', message='quote" newline\n雪'))
    call(10, dict(action='recv', topic='unicode-test'))
    call(11, dict(action='send', topic='empty-test', body='', message='ignored'))
    call(12, dict(action='recv', topic='empty-test'))
    proc = subprocess.run([binary, 'mcp', 'serve', '--toolsets', 'all', '--tier', 'trusted'], input=''.join(json.dumps(r, ensure_ascii=False)+'\n' for r in requests), text=True, capture_output=True, env=env, timeout=30)
    assert proc.returncode == 0, proc.stderr[-2000:]
    replies = {r['id']: r for line in proc.stdout.splitlines() if line.startswith('{') for r in [json.loads(line)] if 'id' in r}
    schema = next(t['inputSchema'] for t in replies[2]['result']['tools'] if t['name'] == 'ipc')
    assert {'body', 'message', 'topic'} <= schema['properties'].keys()
    def payload(ident):
        result = replies[ident]['result']
        assert not result.get('isError'), result
        return json.loads(''.join(c.get('text', '') for c in result['content']))
    assert payload(3)['sent'] is True
    assert payload(4)['messages'][0]['body'] == 'alias-nonce'
    assert payload(5)['sent'] is True
    assert payload(6)['messages'][0]['body'] == 'canonical'
    assert payload(7)['count'] == 0
    assert payload(4)['count'] == 1
    assert payload(4)['messages'][0]['topic'] == 'alias-test'
    assert replies[8]['result']['isError'] is True
    assert 'body or message required' in json.dumps(replies[8])
    assert payload(9)['sent'] is True
    assert payload(10)['messages'][0]['body'] == 'quote" newline\n雪'
    assert payload(11)['sent'] is True
    assert payload(12)['messages'][0]['body'] == ''
    print('PASS: schema disclosure, message delivery, body precedence, topic receive, consumed-message behavior, missing-payload rejection')
