#!/usr/bin/env python3
"""Pin, detect changed bytes, explicitly reselect, and persist through real UI modes."""
import argparse
import fcntl
import hashlib
import http.server
import json
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import tempfile
import termios
import threading
import time


def run(binary, native):
    with tempfile.TemporaryDirectory(prefix='dsco-skill-pin-') as work:
        root = Path(work)
        skill = root / '.dsco/workspace/skills/fixture/SKILL.md'
        skill.parent.mkdir(parents=True)
        v1 = '---\nname: fixture\ndescription: Test observed versions.\n---\nSKILL_BODY_VERSION_ONE\n'
        v2 = v1.replace('VERSION_ONE', 'VERSION_TWO')
        skill.write_text(v1)
        requests = []
        class Provider(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                requests.append(request)
                events = [dict(choices=[dict(index=0, delta=dict(content='PIN_FIXTURE_FINISHED'), finish_reason=None)]),
                          dict(choices=[dict(index=0, delta={}, finish_reason='stop')],
                               usage=dict(prompt_tokens=10, completion_tokens=5))]
                body = (''.join('data: ' + json.dumps(e) + '\n\n' for e in events) + 'data: [DONE]\n\n').encode()
                self.send_response(200)
                self.send_header('Content-Type', 'text/event-stream')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Provider)
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        env = dict(HOME=work, TMPDIR=work, PATH='/usr/bin:/bin:/usr/sbin:/sbin',
                   TERM='xterm-kitty' if native else 'xterm-256color', LANG='en_US.UTF-8',
                   OPENAI_API_KEY='fixture-only', OPENAI_API_BASE=f'http://127.0.0.1:{server.server_port}/v1',
                   DSCO_ENV_FILE='/dev/null', DSCO_PRICING_OFFLINE='1', DSCO_SECURE_STORE_NO_PROMPT='1',
                   DSCO_DISABLE_DEFAULT_FALLBACKS='1', DSCO_AUTO_FALLBACK='0', DSCO_DYNAMIC_FAILOVER='0',
                   DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1', DSCO_NO_AUTO_SUPERVISE='1', DSCO_NO_SUPERVISE='1',
                   DSCO_KITTY_AGENT_WINDOWS='0', DSCO_MCP_HEADLESS='0', DSCO_BANNER='0', DSCO_KITTY_BANNER='0',
                   DSCO_TOOL_PROXY='1', DSCO_AUTO_GOAL='0', DSCO_PIXEL_TUI_ANIMATIONS='0',
                   DSCO_KITTY_GRAPHICS='force' if native else '0', DSCO_HARD_TURN_CEILING='2')
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 1200, 800))
        proc = subprocess.Popen([str(binary), '--profile', 'worker', '--native' if native else '--tui',
                                 '-i', '--provider', 'openai', '-m', 'fixture-model',
                                 '--trust-tier', 'trusted', '--approval-mode', 'never'],
                                cwd=work, env=env, stdin=slave, stdout=slave, stderr=slave,
                                start_new_session=True)
        os.close(slave)
        raw = bytearray()
        def pump(seconds=.1):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if select.select([master], [], [], .02)[0]:
                    try:
                        chunk = os.read(master, 262144)
                    except OSError:
                        return
                    raw.extend(chunk)
                    del raw[:-1000000]
        def wait(predicate, label):
            end = time.monotonic() + 10
            while time.monotonic() < end:
                pump()
                if predicate():
                    return
                assert proc.poll() is None, f'{label}: exited {proc.returncode}'
            raise AssertionError(f'{label}: timeout; tail={bytes(raw[-2000:])!r}')
        def send(text):
            os.write(master, text.encode() + b'\r')
            pump(.4)
        try:
            wait(lambda: b'\x1b[?2004h' in raw, 'composer ready')
            send('/skills use fixture')
            send('First skill turn')
            wait(lambda: len(requests) == 1, 'first request')
            first = json.dumps(requests[0])
            first_hash = hashlib.sha256(v1.encode()).hexdigest()
            assert 'SKILL_BODY_VERSION_ONE' in first and first_hash in first, first[-3000:]
            pump(.6)
            skill.write_text(v2)
            send('Second skill turn')
            wait(lambda: len(requests) == 2, 'second request')
            second = json.dumps(requests[1]['messages'][-1])
            assert 'Instructions withheld' in second and 'SKILL_BODY_VERSION_TWO' not in second
            pump(.6)
            send('/skills use fixture')
            send('Third skill turn')
            wait(lambda: len(requests) == 3, 'third request')
            third = json.dumps(requests[2])
            second_hash = hashlib.sha256(v2.encode()).hexdigest()
            assert 'SKILL_BODY_VERSION_TWO' in third and second_hash in third
            pump(.6)
            send('/quit')
            wait(lambda: proc.poll() is not None, 'clean quit')
            assert proc.returncode == 0
            saved = json.loads((root / '.dsco/sessions/_autosave.json').read_text())
            assert saved['session']['active_skill'] == 'fixture'
            assert saved['session']['active_skill_sha256'] == second_hash
            assert saved['session']['active_skill_provider'] == 'openai'
        finally:
            if proc.poll() is None:
                proc.terminate()
                end = time.monotonic() + 3
                while proc.poll() is None and time.monotonic() < end:
                    pump()
                if proc.poll() is None:
                    proc.kill()
                proc.wait(timeout=3)
            os.close(master)
            server.shutdown()
        return dict(mode='native' if native else 'tui', requests=len(requests), exit_code=proc.returncode,
                    pinned_sha256=second_hash, changed_instructions_withheld=True, explicit_reselection=True)


def run_local_selection(binary, native):
    """Selection/save/load/clear uses no inference request at all."""
    with tempfile.TemporaryDirectory(prefix='dsco-skill-selection-') as work:
        root = Path(work)
        skill = root / '.dsco/workspace/skills/fixture/SKILL.md'
        skill.parent.mkdir(parents=True)
        v1 = '---\nname: fixture\ndescription: Test explicit selection.\n---\nVERSION_ONE\n'
        v2 = v1.replace('VERSION_ONE', 'VERSION_TWO')
        skill.write_text(v1)
        env = dict(HOME=work, TMPDIR=work, PATH='/usr/bin:/bin:/usr/sbin:/sbin',
                   TERM='xterm-kitty' if native else 'xterm-256color', LANG='en_US.UTF-8',
                   OPENAI_API_KEY='fixture-only', OPENAI_API_BASE='http://127.0.0.1:1/v1',
                   DSCO_ENV_FILE='/dev/null', DSCO_PRICING_OFFLINE='1', DSCO_SECURE_STORE_NO_PROMPT='1',
                   DSCO_DISABLE_DEFAULT_FALLBACKS='1', DSCO_AUTO_FALLBACK='0', DSCO_DYNAMIC_FAILOVER='0',
                   DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1', DSCO_NO_AUTO_SUPERVISE='1', DSCO_NO_SUPERVISE='1',
                   DSCO_KITTY_AGENT_WINDOWS='0', DSCO_MCP_HEADLESS='0', DSCO_BANNER='0', DSCO_KITTY_BANNER='0',
                   DSCO_TOOL_PROXY='1', DSCO_AUTO_GOAL='0', DSCO_ALLOW_NET='0',
                   DSCO_PIXEL_TUI_ANIMATIONS='0', DSCO_KITTY_GRAPHICS='force' if native else '0')
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 1200, 800))
        proc = subprocess.Popen([str(binary), '--profile', 'worker', '--native' if native else '--tui',
                                 '-i', '--provider', 'openai', '-m', 'fixture-model'], cwd=work, env=env,
                                stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
        os.close(slave)
        raw = bytearray()
        def pump(seconds=.15):
            stop = time.monotonic() + seconds
            while time.monotonic() < stop:
                if select.select([master], [], [], .02)[0]:
                    try:
                        raw.extend(os.read(master, 262144))
                    except OSError:
                        return
                    del raw[:-1000000]
        def wait(predicate):
            stop = time.monotonic() + 8
            while not predicate() and time.monotonic() < stop:
                pump()
            assert predicate(), bytes(raw[-1500:])
        def command(text):
            os.write(master, text.encode() + b'\r')
            pump(.3)
        def save(name):
            command('/save ' + name)
            path = root / '.dsco/sessions' / (name + '.json')
            wait(path.exists)
            return json.loads(path.read_text())['session']
        try:
            wait(lambda: b'\x1b[?2004h' in raw)
            command('/skills use fixture')
            first = save('pin-first')
            first_hash = hashlib.sha256(v1.encode()).hexdigest()
            assert first['active_skill_sha256'] == first_hash
            assert first['active_skill_provider'] == 'openai'
            skill.write_text(v2)
            stale = save('pin-stale')
            assert stale['active_skill_sha256'] == first_hash, 'changed file must not silently change selection'
            command('/skills use fixture')
            second = save('pin-second')
            assert second['active_skill_sha256'] == hashlib.sha256(v2.encode()).hexdigest()
            command('/skills clear')
            cleared = save('pin-clear')
            assert not cleared.get('active_skill') and not cleared.get('active_skill_sha256')
            command('/load pin-first')
            restored = save('pin-restored')
            assert restored['active_skill_sha256'] == first_hash and restored['active_skill_provider'] == 'openai'
            command('/quit')
            wait(lambda: proc.poll() is not None)
            assert proc.returncode == 0
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=3)
            os.close(master)
        return dict(mode='native' if native else 'tui', exit_code=proc.returncode,
                    select_save_load_clear=True, observed_version_retained=True, requests=0)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--binary', default='./dsco')
    p.add_argument('--output', required=True)
    p.add_argument('--transmission', action='store_true', help='Requires approved provider skill integration; uses only loopback mocks')
    a = p.parse_args()
    binary = Path(a.binary).resolve(strict=True)
    rows = [(run if a.transmission else run_local_selection)(binary, native) for native in [False, True]]
    receipt = dict(passed=True, cases=rows, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                   provider_inference=False, gui_opened=False, local_mock_requests=6 if a.transmission else 0)
    Path(a.output).write_text(json.dumps(receipt, indent=2) + '\n')
    print('PASS: sticky skill selection/version/save/load/clear in actual ANSI and native PTYs; %d loopback requests' % (6 if a.transmission else 0))


if __name__ == '__main__':
    main()
