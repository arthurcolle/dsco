#!/usr/bin/env python3
"""Real buffer storage and owned Kitty editors through the production MCP gate."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time

class Client:
    def __init__(self, binary, state, **grants):
        env = os.environ.copy()
        for key in list(env):
            if key.startswith('DSCO_ALLOW_') or key in ('DSCO_GOV_BYPASS', 'DSCO_MCP_GOV_PINNED'):
                env.pop(key)
        env.update(DSCO_BUFFER_DIR=str(state / 'buffers'), DSCO_SURFACE_DIR=str(state / 'surfaces'),
                   DSCO_GOV_MODEL='standard', DSCO_PRICING_OFFLINE='1', **grants)
        self.err = tempfile.TemporaryFile()
        self.proc = subprocess.Popen([binary, 'mcp', 'serve', '--toolsets', 'terminal', '--tier', 'trusted'],
            env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.err)
        self.seq = 0
        self.rpc('initialize', dict(protocolVersion='2024-11-05', capabilities={}, clientInfo=dict(name='buffer-proof', version='1')))
    def rpc(self, method, params):
        self.seq += 1
        self.proc.stdin.write((json.dumps(dict(jsonrpc='2.0', id=self.seq, method=method, params=params))+'\n').encode())
        self.proc.stdin.flush()
        assert select.select([self.proc.stdout], [], [], 35)[0], f'MCP timeout {method}'
        response = json.loads(self.proc.stdout.readline())
        assert 'error' not in response and response.get('id') == self.seq, response
        return response['result']
    def tool(self, tool, action, ok=True, **args):
        response = self.rpc('tools/call', dict(name=tool, arguments=dict(action=action, **args)))
        assert bool(response.get('isError')) != ok, (tool, action, args, response)
        text = response['content'][0]['text']
        try: return json.loads(text)
        except ValueError: return {'detail': text}
    def buffer(self, action, **args): return self.tool('buffer', action, **args)
    def view(self, action, **args): return self.tool('buffer_view', action, **args)
    def surface(self, action, **args): return self.tool('surface', action, **args)
    def close(self):
        self.proc.stdin.close()
        try: self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill(); self.proc.wait(timeout=3)
        self.err.close()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', default='./dsco')
    parser.add_argument('--storage-only', action='store_true')
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    state = Path(tempfile.mkdtemp(prefix='dsco-buf-', dir='/tmp'))
    client = Client(binary, state)
    owned = []
    checks = []
    try:
        names = {tool['name'] for tool in client.rpc('tools/list', {})['tools']}
        assert {'buffer', 'buffer_view', 'surface'} <= names
        assert client.buffer('list')['buffers'] == []
        assert not (state / 'buffers').exists()
        created = client.buffer('create', name='notes', content='initial π 🦉\n', request_id='create-notes')
        bid, rev = created['buffer']['buffer_id'], created['buffer']['revision']
        assert client.buffer('create', name='notes', content='initial π 🦉\n', request_id='create-notes')['replayed']
        changed = client.buffer('write', buffer_id=bid, content='updated\n', expected_revision=rev, request_id='write-one')
        assert client.buffer('write', buffer_id=bid, content='updated\n', expected_revision=rev, request_id='write-one')['replayed']
        stale = client.buffer('write', buffer_id=bid, content='wrong', expected_revision=rev, ok=False)
        assert stale['error'] == 'revision_conflict', stale
        appended = client.buffer('append', buffer_id=bid, content='once\n', request_id='append-once')
        assert client.buffer('append', buffer_id=bid, content='once\n', request_id='append-once')['replayed']
        assert client.buffer('read', name='notes')['text'] == 'updated\nonce\n'
        fork = client.buffer('fork', buffer_id=bid, new_name='branch', request_id='fork-branch')['buffer']
        client.buffer('append', buffer_id=fork['buffer_id'], content='branch-only\n')
        assert 'branch-only' not in client.buffer('read', name='notes')['text']
        client.buffer('rename', name='branch', new_name='renamed')
        client.buffer('close', name='renamed')
        assert client.buffer('read', name='renamed')['buffer']['closed']
        client.buffer('append', name='renamed', content='closed', ok=False)
        client.buffer('reopen', name='renamed')
        checks.append('buffer creation, Unicode, CAS conflicts, exact retries, fork, rename and archive lifecycle')
        client.close(); client = Client(binary, state)
        assert client.buffer('read', name='notes')['text'] == 'updated\nonce\n'
        checks.append('saved buffers survive harness restart')
        source = state / 'source.txt'; source.write_text('source one\n')
        imported = client.buffer('create', name='file', kind='file', source_path=str(source))['buffer']
        client.buffer('write', name='file', content='owned edit\n', expected_revision=imported['revision'])
        assert source.read_text() == 'source one\n'
        source.write_text('external edit\n')
        failed = client.buffer('save', name='file', ok=False)
        assert failed['error'] == 'source_conflict', failed
        assert source.read_text() == 'external edit\n'
        exported = state / 'export.txt'
        client.buffer('save', name='file', path=str(exported))
        assert exported.read_text() == 'owned edit\n'
        checks.append('file import isolates edits and explicit save detects external changes')
        if not args.storage_only:
            edited = client.view('open', name='notes', visible=False)
            assert edited['verified'] and len(edited['views']) == 1, edited
            editor = edited['views'][0]['surface_id']; owned.append(editor)
            repeated = client.view('open', name='notes', visible=False)
            assert repeated['reused'] and repeated['views'][0]['surface_id'] == editor, repeated
            time.sleep(.4)
            client.surface('send_text', surface_id=editor, text='\x1bGoKITTY_EDITOR_PROOF π 🦉\x1b:w\n')
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline:
                observed = client.buffer('read', name='notes')
                if 'KITTY_EDITOR_PROOF π 🦉' in observed['text']: break
                time.sleep(.1)
            assert 'KITTY_EDITOR_PROOF π 🦉' in observed['text'], observed
            assert observed['buffer']['revision'] != appended['buffer']['revision']
            client.buffer('write', name='notes', content='stale overwrite', expected_revision=appended['buffer']['revision'], ok=False)
            follower = client.view('open', name='notes', mode='follow', location='hsplit', request_id='follow-notes', visible=False)
            assert follower['verified'], follower
            follow_id = follower['views'][0]['surface_id']; owned.append(follow_id)
            client.buffer('append', name='notes', content='LIVE_APPEND_PROOF\n')
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                screen = client.surface('read', surface_id=follow_id)['output']
                if 'LIVE_APPEND_PROOF' in screen: break
                time.sleep(.2)
            assert 'LIVE_APPEND_PROOF' in screen, screen
            assert client.view('focus', name='notes', ok=False)['error'] == 'ambiguous_view'
            assert client.view('layout', surface_id=follow_id, layout='tall')['verified']
            assert client.view('resize', surface_id=follow_id, axis='horizontal', increment=3)['verified']
            assert client.view('detach', surface_id=follow_id)['verified']
            assert client.view('focus', surface_id=follow_id)['verified']
            client.close(); client = Client(binary, state)
            assert len([v for v in client.view('list', name='notes')['views'] if v['exists']]) == 2
            client.view('close', surface_id=follow_id); owned.remove(follow_id)
            assert 'LIVE_APPEND_PROOF' in client.buffer('read', name='notes')['text']
            closed_retry = client.view('open', name='notes', mode='follow', request_id='follow-notes')
            assert closed_retry['views'][0]['closed'] and not closed_retry['verified'], closed_retry
            assert client.view('open', name='notes', mode='view', request_id='follow-notes', ok=False)['error'] == 'request_conflict'
            checks.append('real vi save, independent live follow view, view reuse, resize/layout/detach/focus, restart and retained close')
        denied = Client(binary, state, DSCO_ALLOW_WRITE='0', DSCO_ALLOW_RUN='0')
        try:
            assert denied.buffer('read', name='notes')['buffer']['buffer_id'] == bid
            assert 'capability' in str(denied.buffer('append', name='notes', content='blocked', ok=False)).lower()
            assert 'capability' in str(denied.view('open', name='notes', ok=False)).lower()
        finally: denied.close()
        secret = client.buffer('create', name='sensitive-fixture', content='synthetic only', sensitive=True)
        restricted = Client(binary, state, DSCO_ALLOW_SECRETS='0')
        try:
            refused = restricted.buffer('read', buffer_id=secret['buffer']['buffer_id'], ok=False)
            assert 'secrets' in str(refused).lower(), refused
        finally: restricted.close()
        checks.append('read-only operation without run/write and persisted sensitivity across restart')
    finally:
        # A separate ordinary harness owns cleanup even after taint tests.
        client.close()
        cleanup = Client(binary, state)
        for surface in reversed(owned):
            cleanup.surface('close', surface_id=surface)
        cleanup.close()
    print(json.dumps(dict(ok=True, binary=binary, sha256=hashlib.sha256(Path(binary).read_bytes()).hexdigest(), state=str(state), checks=checks), ensure_ascii=False))

if __name__ == '__main__': main()
