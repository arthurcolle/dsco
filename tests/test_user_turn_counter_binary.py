#!/usr/bin/env python3
"""Installed CLI, owned PTY and local SSE: prompts are distinct from attempts."""
import argparse
import fcntl
import http.server
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, default=Path('./dsco'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--native', action='store_true')
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    requests, errors = [], []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                assert self.path == '/v1/chat/completions'
                req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                requests.append(req)
                stage = len(requests)
                if stage == 1:
                    delta = {'tool_calls': [{'index': 0, 'id': 'counter-recall',
                        'type': 'function', 'function': {'name': 'context_recall',
                        'arguments': '{"key":"ck:msg:fixture-missing"}'}}]}
                    reason = 'tool_calls'
                else:
                    if stage == 2:
                        assert any(m.get('tool_call_id') == 'counter-recall'
                                   for m in req['messages'])
                    delta = {'content': f'COUNTER_FIXTURE_DONE_{stage}'}
                    reason = 'stop'
                chunks = [dict(id='counter-fixture', choices=[dict(index=0,
                    delta=delta, finish_reason=None)]), dict(id='counter-fixture',
                    choices=[dict(index=0, delta={}, finish_reason=reason)],
                    usage={'prompt_tokens': 20, 'completion_tokens': 5, 'cost': 0})]
                payload = (''.join('data: ' + json.dumps(c) + '\n\n' for c in chunks)
                           + 'data: [DONE]\n\n').encode()
                self.send_response(200)
                self.send_header('Content-Type', 'text/event-stream')
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            except Exception as exc:
                errors.append(repr(exc))
                self.close_connection = True

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='dsco-turn-counter-') as tmp:
            root = Path(tmp)
            env = dict(HOME=tmp, TMPDIR=tmp, PATH='/usr/bin:/bin:/usr/sbin:/sbin',
                TERM='xterm-256color', LANG='en_US.UTF-8', OPENAI_API_KEY='local-fixture',
                OPENAI_API_BASE=f'http://127.0.0.1:{server.server_port}/v1',
                DSCO_ENV_FILE='/dev/null', DSCO_PRICING_OFFLINE='1',
                DSCO_SECURE_STORE_NO_PROMPT='1', DSCO_DISABLE_DEFAULT_FALLBACKS='1',
                DSCO_AUTO_FALLBACK='0', DSCO_DYNAMIC_FAILOVER='0',
                DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1', DSCO_NO_AUTO_SUPERVISE='1',
                DSCO_NO_SUPERVISE='1', DSCO_KITTY_AGENT_WINDOWS='0', DSCO_PIXEL_TUI='0',
                DSCO_KITTY_GRAPHICS='0', DSCO_BANNER='0', DSCO_AUTO_GOAL='0',
                DSCO_TUI_ANIMATIONS='0', DSCO_AUTO_COMPACT='0', DSCO_TOOL_PROXY='1',
                DSCO_TOOL_ALLOWLIST='context_recall,context_compact,context_evict',
                DSCO_SYSTEM_PROMPT='Return only the local fixture response.',
                DSCO_CONTEXT_DB=str(root/'context.db'),
                DSCO_CHRONICLE_DIR=str(root/'chronicle'), DSCO_CHRONICLE_MODE='full-local')
            snapshot = root/'native.ppm'
            if args.native:
                env.update(TERM='xterm-kitty', TERM_PROGRAM='kitty', KITTY_WINDOW_ID='1',
                    COLORTERM='truecolor', DSCO_PIXEL_TUI='1', DSCO_KITTY_GRAPHICS='1',
                    DSCO_PIXEL_TUI_SESSION_SNAPSHOT=str(snapshot))
            command = [str(binary), '--profile', 'worker', '--native' if args.native else '--tui', '-i',
                '--provider', 'openai', '-m', 'fixture-model', '--trust-tier', 'trusted',
                '--approval-mode', 'never']
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 1200, 800))
            proc = subprocess.Popen(command, cwd=tmp, env=env, stdin=slave,
                stdout=slave, stderr=slave, start_new_session=True)
            os.close(slave)
            transcript = bytearray()
            autosave = root/'.dsco/sessions/_autosave.json'
            observations = []

            def pump(seconds=.1):
                until = time.monotonic() + seconds
                while time.monotonic() < until:
                    if select.select([master], [], [], .03)[0]:
                        try:
                            data = os.read(master, 262144)
                        except OSError:
                            break
                        if not data:
                            break
                        transcript.extend(data)

            def wait_for(predicate, allow_exit=False):
                until = time.monotonic() + 20
                while time.monotonic() < until:
                    pump()
                    if predicate():
                        pump(.25)
                        return
                    assert allow_exit or proc.poll() is None, (proc.returncode, bytes(transcript[-2000:]))
                raise AssertionError(bytes(transcript[-3000:]))

            def send(text):
                os.write(master, (text+'\r').encode())

            def saved():
                try:
                    return json.loads(autosave.read_text())
                except (OSError, ValueError):
                    return {}

            def answer(stage, turns, attempts):
                marker = f'COUNTER_FIXTURE_DONE_{stage}'
                wait_for(lambda: marker in json.dumps(saved().get('messages', [])))
                state = saved()['session']
                assert (state['user_turn_count'], state['turn_count']) == (turns, attempts), state
                observations.append(dict(stage=stage, user_turns=turns, attempts=attempts))

            def status(turns, attempts):
                offset = len(transcript)
                send('/status')
                if args.native:
                    pump(.6)
                    assert snapshot.exists() and len(requests) == attempts
                    return
                def matches():
                    text = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '',
                                  transcript[offset:].decode(errors='replace'))
                    return (re.search(r'Turns:\s+'+str(turns)+r'\b', text) and
                            re.search(r'Attempts:\s+'+str(attempts)+r'\b', text))
                wait_for(matches)

            try:
                wait_for(lambda: b'\x1b[?2004h' in transcript)
                send('Inspect the local context archive with context_recall and repair the fixture.')
                answer(2, 1, 2)
                status(1, 2)
                send('/compact'); pump(.6)
                status(1, 2)
                send('/retry'); answer(3, 1, 3)
                status(1, 3)
                send('/undo')
                wait_for(lambda: saved().get('session', {}).get('user_turn_count') == 0)
                status(0, 3)
                send('Inspect the local fixture again.'); answer(4, 1, 4)
                send('/save counter-slot'); pump(.5)
                send('/clear'); pump(.5)
                if args.native:
                    assert saved()['session']['user_turn_count'] == 0
                else:
                    status(0, 0)
                send('/retry'); pump(.5)
                assert len(requests) == 4, 'clear retained a stale retry input'
                send('/load counter-slot'); pump(.5)
                status(1, 4)
                send('Inspect the resumed local fixture.'); answer(5, 2, 5)
                status(2, 5)
                if args.native and args.output:
                    import shutil
                    shutil.copy2(snapshot, args.output.with_suffix('.ppm'))
                send('/quit')
                # Drain terminal pixels while shutdown restores the terminal.
                wait_for(lambda: proc.poll() is not None, allow_exit=True)
                proc.wait(timeout=10)
                assert proc.returncode == 0 and not errors, (proc.returncode, errors)
                result = dict(binary=str(binary), requests=len(requests),
                    native=args.native, observations=observations, commands='status,compact,retry,undo,save,clear,load',
                    paid_requests=0)
                if args.output:
                    args.output.write_text(json.dumps(result, indent=2)+'\n')
                print('PASS: '+('native compositor' if args.native else 'text terminal')+' real PTY prompts, tool rounds, commands, retry, undo and save/resume; 5 local requests')
            except Exception:
                if args.output:
                    args.output.with_suffix('.failure.txt').write_text(
                        'saved='+json.dumps(saved().get('session', {}))+'\n'+
                        transcript[-6000:].decode(errors='replace'))
                raise
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    try:
                        proc.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait(timeout=5)
                os.close(master)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
