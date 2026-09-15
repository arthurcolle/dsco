#!/usr/bin/env python3
"""Actual MCP invoke_tool calls retain caller tier over ambient trusted defaults.

Uses only owned temporary read fixtures and denied write targets. No model,
network, keychain, or user HOME changes; capability defaults apply in the child.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def successful_text(reply):
    assert 'error' not in reply, reply
    result = reply.get('result', {})
    assert not result.get('isError'), reply
    return '\n'.join(part.get('text', '') for part in result.get('content', [])
                     if part.get('type') == 'text')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', default='./dsco')
    args = parser.parse_args()
    binary = Path(args.binary).resolve(strict=True)
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix='dsco-invoke-tier-') as directory:
        root = Path(directory)
        direct_read, indirect_read = root / 'direct-read.txt', root / 'indirect-read.txt'
        direct_read.write_text('OWNED_DIRECT_READ_PROOF', encoding='utf-8')
        indirect_read.write_text('OWNED_INVOKE_READ_PROOF', encoding='utf-8')
        direct_read.chmod(0o600)
        indirect_read.chmod(0o600)
        direct_write, indirect_write = root / 'direct-denied.txt', root / 'indirect-denied.txt'

        env = os.environ.copy()
        # Reset only capability grants in the private child so this tests tier
        # defaults, not an operator's explicit grant or denial. Credentials and
        # HOME remain exactly as inherited.
        for key in ('DSCO_ALLOW_READ', 'DSCO_ALLOW_WRITE', 'DSCO_ALLOW_NET',
                    'DSCO_ALLOW_RUN', 'DSCO_ALLOW_SECRETS', 'DSCO_ALLOW_CONTROL',
                    'DSCO_ALLOW_EXFIL', 'DSCO_GOV_BYPASS', 'DSCO_MCP_GOV_PINNED'):
            env.pop(key, None)
        env.update(DSCO_TRUST_TIER='trusted', DSCO_GOV_MODEL='standard',
                   DSCO_NO_AUTO_SUPERVISE='1', DSCO_PRICING_OFFLINE='1',
                   DSCO_MCP_HEADLESS='0', DSCO_TOOLMGMT='0')
        requests = [dict(jsonrpc='2.0', id='initialize', method='initialize', params={
            'protocolVersion': '2024-11-05', 'capabilities': {},
            'clientInfo': {'name': 'owned-invoke-tier-proof', 'version': '1'}}),
            dict(jsonrpc='2.0', method='notifications/initialized')]
        calls = [
            ('direct_read', 'read_file', {'path': str(direct_read)}),
            ('direct_write', 'write_file', {'path': str(direct_write), 'content': 'must not write'}),
            ('indirect_read', 'invoke_tool', {'name': 'read_file', 'input': {'path': str(indirect_read)}}),
            ('indirect_write', 'invoke_tool', {'name': 'write_file', 'input': {
                'path': str(indirect_write), 'content': 'must not write'}}),
        ]
        for ident, name, arguments in calls:
            requests.append(dict(jsonrpc='2.0', id=ident, method='tools/call',
                                 params=dict(name=name, arguments=arguments)))
        proc = subprocess.run([str(binary), 'mcp', 'serve', '--toolsets', 'all', '--tier', 'untrusted'],
                              cwd=root, env=env, capture_output=True, text=True, timeout=20,
                              input=''.join(json.dumps(request) + '\n' for request in requests))
        assert proc.returncode == 0, (proc.returncode, proc.stderr[-1600:])
        assert len(proc.stdout) < 1024 * 1024, 'MCP response exceeded fixture bound'
        replies = {}
        for line in proc.stdout.splitlines():
            reply = json.loads(line)
            if 'id' in reply:
                assert reply['id'] not in replies, 'duplicate JSON-RPC response ID'
                replies[reply['id']] = reply
        assert set(replies) == {'initialize', *(ident for ident, _, _ in calls)}, replies
        assert replies['initialize'].get('result', {}).get('serverInfo'), replies['initialize']
        assert 'OWNED_DIRECT_READ_PROOF' in successful_text(replies['direct_read'])
        assert 'OWNED_INVOKE_READ_PROOF' in successful_text(replies['indirect_read'])

        # Direct MCP preflight may reject through a JSON-RPC error envelope;
        # an executed wrapper instead carries the inner tool result envelope.
        direct = replies['direct_write']
        if 'error' in direct:
            direct_reason = direct['error'].get('message', '')
            assert 'write_file' in direct_reason and 'untrusted' in direct_reason, direct
        else:
            assert direct.get('result', {}).get('isError'), direct
            direct_reason = json.dumps(direct['result'])
            assert 'write_file' in direct_reason and 'untrusted' in direct_reason, direct

        indirect = replies['indirect_write']
        assert 'error' not in indirect, ('outer invoke gate prevented inner-tier coverage', indirect)
        result = indirect.get('result', {})
        assert result.get('isError'), ('ambient trusted tier escaped through invoke_tool', indirect)
        texts = [part.get('text', '') for part in result.get('content', []) if part.get('type') == 'text']
        assert len(texts) == 1, indirect
        denial = json.loads(texts[0])
        assert denial.get('error') == 'governance_block', denial
        assert denial.get('tool') == 'write_file', ('denial did not reach the inner target', denial)
        assert 'untrusted' in denial.get('reason', ''), denial
        assert not direct_write.exists() and not indirect_write.exists(), 'untrusted write created a file'
        assert direct_read.read_text() == 'OWNED_DIRECT_READ_PROOF'
        assert indirect_read.read_text() == 'OWNED_INVOKE_READ_PROOF'
        assert hashlib.sha256(binary.read_bytes()).hexdigest() == binary_hash, 'binary changed during proof; rerun after build'
        print(json.dumps({'ok': True, 'binary': str(binary), 'sha256': binary_hash,
                          'ambient_tier': 'trusted', 'mcp_tier': 'untrusted',
                          'direct_read': True, 'indirect_read': True,
                          'inner_write_denied': True, 'denial_stage': denial.get('stage'),
                          'write_targets_absent': True, 'home_preserved': True}))


if __name__ == '__main__':
    main()
