#!/usr/bin/env python3
"""Owned headless PTY + HTTP fixture: interactive prompts retain real tool access.
No external HTTP, real credentials, model inference, or visible terminal windows.
"""
import argparse
import fcntl
import hashlib
import http.server
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time
from test_buffer_slash import TerminalText


def run(binary, prompt, disabled=False, force_none=False, conceptual=False,
        refuse_first=False):
    requests = []
    errors = []
    token = 'OWNED_ROUTING_PROOF_74319'
    with tempfile.TemporaryDirectory(prefix='dsco-routing-pty-') as tmp:
        sample = Path(tmp, 'sample.txt')
        sample.write_text(token + '\n')

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                try:
                    assert self.path == '/v1/chat/completions', self.path
                    req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                    names = [v.get('function', v).get('name') for v in req.get('tools', [])]
                    result_seen = any(m.get('role') == 'tool' and token in json.dumps(m)
                                      for m in req.get('messages', []))
                    wire = json.dumps(req, ensure_ascii=False)
                    row = dict(tools=names, choice=req.get('tool_choice'),
                               grounding='LIVE TOOLS — CURRENT REQUEST' in wire,
                               direct_mode='[Direct Answer Mode]' in wire,
                               actual_file_result=result_seen)
                    requests.append(row)
                    assert len(requests) <= 3, 'unexpected extra provider call'
                    can_call = 'read_file' in names and req.get('tool_choice') != 'none'
                    if refuse_first and len(requests) == 1:
                        delta = {'content': "I don't have access to tools. "
                                            'OWNED_FALSE_NO_TOOLS_REFUSAL'}
                        finish = 'stop'
                    elif can_call and not result_seen and not conceptual:
                        delta = {'tool_calls': [{'index': 0, 'id': 'call_owned_read',
                                 'type': 'function', 'function': {'name': 'read_file',
                                 'arguments': json.dumps({'path': str(sample)})}}]}
                        finish = 'tool_calls'
                    else:
                        delta = {'content': 'ROUTING_FIXTURE_FINISHED'}
                        finish = 'stop'
                    events = [dict(id='owned-fixture', choices=[dict(index=0, delta=delta,
                                                                   finish_reason=None)]),
                              dict(id='owned-fixture', choices=[dict(index=0, delta={},
                                                                   finish_reason=finish)],
                                   usage=dict(prompt_tokens=10, completion_tokens=10, cost=0))]
                    payload = ''.join('data: ' + json.dumps(e) + '\n\n' for e in events)
                    payload += 'data: [DONE]\n\n'
                    data = payload.encode()
                    self.send_response(200)
                    self.send_header('Content-Type', 'text/event-stream')
                    self.send_header('Content-Length', str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                except Exception as exc:
                    errors.append(repr(exc))
                    self.close_connection = True

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = dict(HOME=tmp, TMPDIR=tmp, PATH='/usr/bin:/bin:/usr/sbin:/sbin',
                   TERM='xterm-256color', LANG='en_US.UTF-8',
                   OPENAI_API_KEY='fixture-only',
                   OPENAI_API_BASE=f'http://127.0.0.1:{server.server_port}/v1',
                   DSCO_ENV_FILE='/dev/null', DSCO_PRICING_OFFLINE='1',
                   DSCO_SECURE_STORE_NO_PROMPT='1',
                   DSCO_DISABLE_DEFAULT_FALLBACKS='1', DSCO_AUTO_FALLBACK='0',
                   DSCO_DYNAMIC_FAILOVER='0', DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1',
                   DSCO_NO_AUTO_SUPERVISE='1', DSCO_NO_SUPERVISE='1',
                   DSCO_KITTY_AGENT_WINDOWS='0', DSCO_PIXEL_TUI='0',
                   DSCO_KITTY_GRAPHICS='0', DSCO_MCP_HEADLESS='0',
                   DSCO_BANNER='0', DSCO_KITTY_BANNER='0', DSCO_TOOL_PROXY='1',
                   DSCO_ALLOW_NET='0',
                   DSCO_SYSTEM_PROMPT='Owned local fixture. Follow the user request.',
                   DSCO_HARD_TURN_CEILING='4')
        if disabled:
            env['DSCO_OR_DISABLE_TOOLS'] = '1'
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 0, 0))
        proc = None
        transcript = bytearray()
        text_filter = TerminalText()
        sent = quit_sent = False
        forced = not force_none
        try:
            proc = subprocess.Popen([str(binary), '--profile', 'worker', '--tui', '-i', '--provider', 'openai',
                                     '-m', 'gpt54'], cwd=tmp, env=env,
                                    stdin=slave, stdout=slave, stderr=slave,
                                    start_new_session=True)
            os.close(slave)
            slave = -1
            started = time.monotonic()
            finished_at = ready_at = None
            while time.monotonic() - started < 25:
                if select.select([master], [], [], .04)[0]:
                    try:
                        data = os.read(master, 262144)
                    except OSError:
                        data = b''
                    transcript.extend(text_filter.feed(data))
                    assert len(transcript) < 2 * 1024 * 1024, 'PTY output limit'
                if b'Ctrl+G swarm' in transcript:
                    ready_at = ready_at or time.monotonic()
                if not sent and ready_at and time.monotonic() - ready_at > .3:
                    if not forced:
                        os.write(master, b'/force none\r')
                        forced = True
                    elif not force_none or b'tool_choice set to none' in transcript:
                        os.write(master, (prompt + '\r').encode())
                        sent = True
                if b'ROUTING_FIXTURE_FINISHED' in transcript:
                    finished_at = finished_at or time.monotonic()
                    if not quit_sent and time.monotonic() - finished_at > .5:
                        os.write(master, b'/quit\r')
                        quit_sent = True
                if proc.poll() is not None:
                    break
            assert proc.poll() == 0 and quit_sent, (proc.poll(), bytes(transcript[-2500:]))
            assert not errors, errors
            assert requests, 'no fixture requests'
            return dict(prompt=prompt, disabled=disabled, force_none=force_none,
                        conceptual=conceptual, refuse_first=refuse_first,
                        refusal_visible=b'OWNED_FALSE_NO_TOOLS_REFUSAL' in transcript,
                        requests=requests,
                        elapsed_seconds=round(time.monotonic() - started, 3))
        finally:
            if proc is not None and proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=3)
            if slave >= 0:
                os.close(slave)
            os.close(master)
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', type=Path, default=Path('./dsco'))
    ap.add_argument('--expect-bug', action='store_true')
    args = ap.parse_args()
    binary = args.binary.resolve(strict=True)
    before = hashlib.sha256(binary.read_bytes()).hexdigest()
    cases = [('gimme weather in DC', dict(refuse_first=True)),
             ('Why did the last command fail?', {}),
             ('What is a mutex?', dict(conceptual=True)),
             ('What is my live balance?', dict(force_none=True)),
             ('What is the weather in DC?', dict(disabled=True))]
    rows = []
    for prompt, options in cases:
        row = run(binary, prompt, **options)
        rows.append(row)
        requests = row['requests']
        if options.get('force_none') or options.get('disabled'):
            assert len(requests) == 1 and not requests[0]['grounding'], row
            assert not requests[0]['actual_file_result'], row
            if options.get('force_none'):
                assert requests[0]['choice'] == 'none', row
            else:
                assert not requests[0]['tools'], row
        elif args.expect_bug:
            assert len(requests) == 1 and not requests[0]['tools'], row
        elif options.get('conceptual'):
            assert len(requests) == 1 and requests[0]['grounding'], row
            assert 'read_file' in requests[0]['tools'], row
            assert not requests[0]['actual_file_result'], row
        elif options.get('refuse_first'):
            assert len(requests) == 3 and requests[2]['actual_file_result'], row
            # A successful/attempted DSCO preflight lets the first model turn
            # summarize with auto; a false capability refusal must make the
            # recovery turn strict regardless.
            assert requests[0]['choice'] in ('auto', 'required'), row
            assert requests[1]['choice'] == 'required', row
            assert not row['refusal_visible'], row
            assert all(r['grounding'] and 'read_file' in r['tools'] and
                       'weather' in r['tools'] and not r['direct_mode']
                       for r in requests), row
        else:
            assert len(requests) == 2 and requests[1]['actual_file_result'], row
            assert all(r['grounding'] and 'read_file' in r['tools'] and not r['direct_mode']
                       for r in requests), row
    assert hashlib.sha256(binary.read_bytes()).hexdigest() == before, 'binary changed during test'
    print(json.dumps(dict(binary=str(binary), sha256=before, expect_bug=args.expect_bug,
                          passed=True, cases=rows), indent=2))


if __name__ == '__main__':
    main()
