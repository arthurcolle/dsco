#!/usr/bin/env python3
"""Offline binary regression: rolling pressure archives old assistant prose, not user turns."""
import fcntl
import http.server
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

from test_input_budget_binary import budget_events

binary = Path(sys.argv[1] if len(sys.argv) > 1 else './dsco').resolve(strict=True)
FIRST = 'OWNED_PROSE_FIRST'
SECOND = 'OWNED_PROSE_FOLLOWUP'
THIRD = 'OWNED_PROSE_THIRD'
LONG = 'OWNED_PROSE_START_' + 'z' * 120000 + '_OWNED_PROSE_END'
requests, errors = [], []
with tempfile.TemporaryDirectory(prefix='dsco-prose-rolling-') as tmp:
    root = Path(tmp)
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_POST(self):
            try:
                req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                requests.append(req)
                assert self.path == '/v1/chat/completions'
                assert len(requests) <= 3
                messages = req['messages']
                assert FIRST in json.dumps(messages)
                if len(requests) == 3:
                    wire = json.dumps(messages)
                    assert SECOND in wire and THIRD in wire
                    assert 'ck:msg:' in wire, 'old assistant text did not archive'
                    assert '_OWNED_PROSE_END' not in wire, 'large response still in provider request'
                text = LONG if len(requests) == 1 else 'OWNED_PROSE_FINISHED' if len(requests) == 3 else 'OWNED_PROSE_SHORT'
                chunks = [dict(id='owned-prose', choices=[dict(index=0, delta={'content': text}, finish_reason=None)]),
                          dict(id='owned-prose', choices=[dict(index=0, delta={}, finish_reason='stop')],
                               usage={'prompt_tokens': 20, 'completion_tokens': 10, 'cost': 0})]
                data = (''.join('data: ' + json.dumps(chunk) + '\n\n' for chunk in chunks)
                        + 'data: [DONE]\n\n').encode()
                self.send_response(200); self.send_header('Content-Type', 'text/event-stream')
                self.send_header('Content-Length', str(len(data))); self.end_headers()
                self.wfile.write(data)
            except Exception as e:
                errors.append(repr(e)); self.close_connection = True
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    env = dict(HOME=tmp, TMPDIR=tmp, PATH='/usr/bin:/bin:/usr/sbin:/sbin',
        TERM='xterm-256color', LANG='en_US.UTF-8', OPENAI_API_KEY='local-fixture',
        OPENAI_API_BASE=f'http://127.0.0.1:{server.server_port}/v1',
        DSCO_ENV_FILE='/dev/null', DSCO_PRICING_OFFLINE='1', DSCO_SECURE_STORE_NO_PROMPT='1',
        DSCO_DISABLE_DEFAULT_FALLBACKS='1', DSCO_AUTO_FALLBACK='0', DSCO_DYNAMIC_FAILOVER='0',
        DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1', DSCO_NO_AUTO_SUPERVISE='1', DSCO_NO_SUPERVISE='1',
        DSCO_KITTY_AGENT_WINDOWS='0', DSCO_KITTY_GRAPHICS='0', DSCO_BANNER='0', DSCO_AUTO_GOAL='0',
        DSCO_TOOL_PROXY='1', DSCO_TOOL_ALLOWLIST='context_evict,context_recall',
        DSCO_AUTO_COMPACT='1', DSCO_REQUEST_TARGET_BYTES='65536', DSCO_TUI_ANIMATIONS='0',
        DSCO_CHRONICLE_DIR=str(root / 'chronicle'), DSCO_CHRONICLE_MODE='full-local',
        DSCO_CONTEXT_DB=str(root / 'context.db'), DSCO_SYSTEM_PROMPT='OWNED_PROSE_SYSTEM',
        DSCO_HARD_TURN_CEILING='3')
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 1200, 800))
    proc = subprocess.Popen([str(binary), '--profile', 'worker', '--tui', '-i',
                             '--provider', 'openai', '-m', 'fixture-model',
                             '--trust-tier', 'trusted', '--approval-mode', 'never'],
                            cwd=tmp, env=env, stdin=slave, stdout=slave, stderr=slave,
                            start_new_session=True)
    os.close(slave)
    raw_tail = bytearray()
    sent_first = sent_second = sent_third = quit_sent = False
    second_at = None
    autosave = root / '.dsco/sessions/_autosave.json'
    try:
        until = time.monotonic() + 45
        while time.monotonic() < until and proc.poll() is None:
            if select.select([master], [], [], .05)[0]:
                try: data = os.read(master, 262144)
                except OSError: data = b''
                raw_tail.extend(data); del raw_tail[:-4096]
                if b'\x1b[?2004h' in raw_tail and not sent_first:
                    os.write(master, (FIRST + '\r').encode()); sent_first = True
            saved = None
            if autosave.exists():
                try: saved = json.loads(autosave.read_text())
                except ValueError: pass
            stored_text = json.dumps(saved) if saved else ''
            if sent_first and not sent_second and LONG[-120:] in stored_text:
                os.write(master, (SECOND + '\r').encode()); sent_second = True
            if sent_second and not sent_third and 'OWNED_PROSE_SHORT' in stored_text:
                os.write(master, (THIRD + '\r').encode()); sent_third = True
            if sent_third and len(requests) == 3 and 'OWNED_PROSE_FINISHED' in stored_text:
                second_at = second_at or time.monotonic()
                if not quit_sent and time.monotonic() - second_at > .3:
                    os.write(master, b'/quit\r'); quit_sent = True
        assert proc.poll() == 0 and quit_sent, (proc.poll(), sent_first, sent_second, sent_third, len(requests), errors, bytes(raw_tail[-500:]))
        assert not errors, errors
        assert len(requests) == 3
        rows = budget_events(root)
        assert any(r['archive_passes'] > 0 and r['wire_target_satisfied'] for r in rows), rows
        print(json.dumps({'passed': True, 'requests': len(requests),
                          'assistant_archive_passes': max(r['archive_passes'] for r in rows),
                          'user_and_recent_preserved': True, 'real_inference': False}))
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL); proc.wait(timeout=3)
        os.close(master); server.shutdown(); server.server_close(); thread.join(timeout=3)
