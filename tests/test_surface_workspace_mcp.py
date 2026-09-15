#!/usr/bin/env python3
"""Live owned Kitty lifecycle through the production MCP capability gate.

Only synthetic /bin/cat panes are read, using ordinary trusted-tier grants.
No operator terminal or credential/profile store is touched.
"""
import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time


class Client:
    def __init__(self, binary, state):
        env = os.environ.copy()
        for key in list(env):
            if key.startswith('DSCO_ALLOW_') or key in ('DSCO_GOV_BYPASS', 'DSCO_MCP_GOV_PINNED'):
                env.pop(key)
        env.update(DSCO_SURFACE_DIR=state, DSCO_GOV_MODEL='standard')
        self.err = tempfile.TemporaryFile()
        self.proc = subprocess.Popen([binary, 'mcp', 'serve', '--toolsets', 'terminal,desktop', '--tier', 'trusted'],
                                     env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.err)
        self.seq = 0
        self.rpc('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
                               'clientInfo': {'name': 'owned-kitty-test', 'version': '1'}})

    def rpc(self, method, params):
        self.seq += 1
        self.proc.stdin.write((json.dumps(dict(jsonrpc='2.0', id=self.seq, method=method, params=params))+'\n').encode())
        self.proc.stdin.flush()
        assert select.select([self.proc.stdout], [], [], 35)[0], f'MCP deadline: {method}'
        line = self.proc.stdout.readline()
        reply = json.loads(line)
        assert reply.get('id') == self.seq and 'error' not in reply, reply
        return reply['result']

    def tool(self, tool, args, ok=True):
        result = self.rpc('tools/call', dict(name=tool, arguments=args))
        assert bool(result.get('isError')) != ok, (args, result)
        body = result['content'][0]['text']
        try:
            return json.loads(body)
        except ValueError:
            return {'text': body}

    def surface(self, action, ok=True, **args):
        return self.tool('surface', dict(action=action, workspace='fixture', **args), ok)

    def stop(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.err.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', default='./dsco')
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    # Keep the Unix domain socket below macOS's 104-byte limit.
    state = tempfile.mkdtemp(prefix='dsco-view-', dir='/tmp')
    client = Client(binary, state)
    owned = []
    checks = []
    try:
        # Observations and invalid actions must not create workspace state.
        client.surface('status')
        assert not Path(state, 'fixture').exists()
        client.surface('not_an_action', ok=False)
        assert not Path(state, 'fixture').exists()
        checks.append('read-only and invalid requests do not create workspace state')

        started = client.surface('start', command='/bin/cat', visible=False)
        assert started['verified'] and len(started['surfaces']) == 1, started
        first = started['surfaces'][0]['surface_id']
        owned.append(first)
        assert started['surfaces'][0]['exists']
        registry = Path(state, 'fixture', 'views.json')
        assert registry.stat().st_mode & 0o077 == 0
        checks.append('private Kitty startup with durably registered initial pane')

        launched = client.surface('create', source_surface_id=first, request_id='split-proof',
                                  run_id='fixture-run', command='/bin/cat', type='window', location='vsplit')
        second = launched['surfaces'][0]['surface_id']
        owned.append(second)
        assert launched['verified'] and second != first
        repeated = client.surface('create', request_id='split-proof')
        assert repeated['surfaces'][0]['surface_id'] == second
        assert len(client.surface('list')['surfaces']) == 2
        checks.append('split creation and request-id retry without duplicate launch')

        # A workspace tag alone is not membership in the durable registry.
        unregistered = client.tool('kitty_remote', {'command': 'launch', 'to': started['to'],
            'args': ['--type=window', '--keep-focus', '--match=id:' + str(launched['surfaces'][0]['window_id']),
                     '--var', 'dsco_workspace=fixture', '/bin/cat']})
        foreign_id = int(unregistered['output'].strip())
        try:
            refused = client.surface('layout', surface_id=second, layout='grid', ok=False)
            assert refused['ok'] is False, refused
            client.surface('resize', surface_id=second, increment=2, ok=False)
            unchanged = client.surface('inspect', surface_id=second)['surfaces'][0]
            assert unchanged['layout'] == launched['surfaces'][0]['layout']
        finally:
            client.tool('kitty_remote', {'command': 'close-window', 'to': started['to'],
                                        'args': ['--match=id:' + str(foreign_id)]})
        checks.append('workspace tag alone cannot authorize layout/resize of unregistered panes')

        intact = registry.read_bytes()
        malformed = json.loads(intact)
        malformed['version'] = 999
        registry.write_text(json.dumps(malformed))
        try:
            client.surface('start', ok=False)
        finally:
            registry.write_bytes(intact)
        assert len(client.surface('list')['surfaces']) == 2
        checks.append('unsupported registry version fails closed without another launch')

        client.surface('send_text', surface_id=second, text='OWNED_SURFACE_中文\\literal\n')
        screen = client.surface('read', surface_id=second)
        assert 'OWNED_SURFACE_中文\\literal' in screen['output'], screen
        checks.append('Unicode and literal backslash input observed in exact owned pane')
        resized = client.surface('resize', surface_id=second, axis='horizontal', increment=5)
        assert resized['verified'], resized
        layout = client.surface('layout', surface_id=second, layout='tall')
        assert layout['verified'], layout
        detached = client.surface('detach', surface_id=second)
        assert detached['verified'], detached
        focused = client.surface('focus', surface_id=second)
        assert focused['verified'], focused
        checks.append('resize, layout, detach and focus observed after commands')

        before = registry.read_bytes()
        client.surface('list')
        assert registry.read_bytes() == before
        client.stop()
        client = Client(binary, state)
        recovered = client.surface('inspect', surface_id=second)
        assert recovered['surfaces'][0]['exists'] and recovered['surfaces'][0]['run_id'] == 'fixture-run'
        checks.append('registry survives MCP restart; observations do not rewrite it')

        closed = client.surface('close', surface_id=second)
        assert closed['verified'] and closed['surfaces'][0]['closed'], closed
        owned.remove(second)
        replay = client.surface('create', request_id='split-proof')
        assert replay['surfaces'][0]['closed'] and not replay['surfaces'][0]['exists']
        checks.append('closed tombstone persists and retry never resurrects it')
        print(json.dumps({'ok': True, 'checks': checks, 'state': state}, indent=2))
    finally:
        # Close only IDs returned by this fixture's private workspace.
        for surface in reversed(owned):
            try:
                client.surface('close', surface_id=surface)
            except Exception as exc:
                print(f'fixture cleanup needs inspection: {surface}: {exc}', file=__import__('sys').stderr)
        client.stop()
        # Keep the tiny registry as a receipt; no recursive filesystem cleanup.


if __name__ == '__main__':
    main()
