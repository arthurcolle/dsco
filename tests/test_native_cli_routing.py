#!/usr/bin/env python3
"""Prove saved CLI selectors use DSCO's native HTTP path, without provider CLIs."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

binary = Path(sys.argv[1] if len(sys.argv) > 1 else './dsco').resolve()
with tempfile.TemporaryDirectory(prefix='dsco-native-routing-') as tmp:
    root = Path(tmp)
    trap = root / 'external-cli-called'
    for name in ('codex', 'claude'):
        executable = root / name
        executable.write_text('#!/bin/sh\nprintf called > "$DSCO_TEST_EXEC_TRAP"\nexit 93\n')
        executable.chmod(0o700)
    envfile = root / 'provider.env'
    env = dict(PATH=f'{tmp}:/usr/bin:/bin', TMPDIR=tmp, LANG='en_US.UTF-8',
               DSCO_ENV_FILE=str(envfile), DSCO_TEST_EXEC_TRAP=str(trap),
               DSCO_CHATGPT_OAUTH_TOKEN='fixture-only', DSCO_CHATGPT_ACCOUNT_ID='fixture-account',
               ANTHROPIC_API_KEY='sk-ant-fixture-only', DSCO_CHATGPT_GATE='0',
               DSCO_SECURE_STORE_NO_PROMPT='1', DSCO_CREDENTIAL_DISCOVERY_NO_PROMPT='1',
               DSCO_DISABLE_SHARED_HOME_OAUTH='1', DSCO_PRICING_OFFLINE='1',
               DSCO_DISABLE_DEFAULT_FALLBACKS='1', DSCO_AUTO_FALLBACK='0', DSCO_DYNAMIC_FAILOVER='0',
               DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1', DSCO_NO_AUTO_SUPERVISE='1', DSCO_NO_SUPERVISE='1',
               DSCO_MCP_HEADLESS='0', DSCO_TOOLMGMT='0', DSCO_AUTO_GOAL='0',
               DSCO_PIXEL_TUI='0', DSCO_BANNER='0', DSCO_HARD_TURN_CEILING='1',
               DSCO_SYSTEM_PROMPT='Owned local regression fixture. Answer directly.')
    for selector, model, provider in (('codex', 'gpt-6-astra', 'openai-codex'),
                                       ('claude', 'claude-sonnet-5', 'anthropic')):
        envfile.write_text(f'DSCO_EXEC={selector}\nDSCO_MODEL={model}\n')
        cp = subprocess.run([str(binary), '--route-explain', model], env=env, cwd=tmp,
                            capture_output=True, text=True, timeout=20)
        assert cp.returncode == 0, cp.stderr
        assert f'route_provider: {provider}\n' in cp.stdout, cp.stdout
        assert 'route_executor: (none)\n' in cp.stdout, cp.stdout
        assert not trap.exists(), selector
    requests = []
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            requests.append((self.path, payload, self.headers.get('Authorization')))
            events = [
                {'type': 'response.created', 'response': {'id': 'fixture'}},
                {'type': 'response.output_text.delta', 'delta': 'NATIVE-CLI-OK'},
                {'type': 'response.completed', 'response': {'status': 'completed',
                    'usage': {'input_tokens': 7, 'output_tokens': 5}}}]
            body = ''.join('data: ' + json.dumps(item) + '\n\n' for item in events).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        envfile.write_text('DSCO_EXEC=codex\nDSCO_MODEL=gpt-6-astra\n')
        env['DSCO_CHATGPT_BASE_URL'] = f'http://127.0.0.1:{server.server_port}/responses'
        cp = subprocess.run([str(binary), '--profile', 'worker', 'Reply with NATIVE-CLI-OK'],
                            env=env, cwd=tmp, capture_output=True, text=True, timeout=30)
        assert cp.returncode == 0, cp.stderr
        assert 'NATIVE-CLI-OK' in cp.stdout + cp.stderr, (cp.stdout, cp.stderr)
        assert len(requests) == 1, len(requests)
        assert requests[0][0] == '/responses', requests[0][0]
        assert requests[0][2] == 'Bearer fixture-only', 'wrong OAuth header'
        assert not trap.exists(), 'provider CLI was executed'
        # A canceled menu performs no authentication or preference mutation.
        before = envfile.read_text()
        cp = subprocess.run([str(binary), 'login'], input='q\n', env=env, cwd=tmp,
                            capture_output=True, text=True, timeout=20)
        assert cp.returncode == 0, cp.stderr
        assert 'connect a native provider' in cp.stderr, cp.stderr
        assert envfile.read_text() == before
        assert not trap.exists(), 'login executed provider CLI'
    finally:
        server.shutdown()
        server.server_close()
print('PASS: saved codex/claude selectors route natively; real CLI streamed HTTP; no provider process; login cancel preserves preferences')
