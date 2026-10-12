#!/usr/bin/env python3
"""Exercise actual /status + /windows session via an owned headless PTY.
No desktop surface, model request, real HOME, or credentials are used.
"""
import argparse
import fcntl
import hashlib
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
import time

parser = argparse.ArgumentParser()
parser.add_argument('--binary', default='./dsco')
parser.add_argument('--output', required=True)
args = parser.parse_args()
binary = Path(args.binary).resolve(strict=True)
output = Path(args.output).resolve()
output.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='dsco-session-details-') as directory:
    env = dict(HOME=directory, TMPDIR=directory, PATH='/usr/bin:/bin:/usr/sbin:/sbin',
        LANG='en_US.UTF-8', TERM='xterm-kitty', TERM_PROGRAM='kitty',
        OPENAI_API_KEY='fixture-only', OPENAI_API_BASE='http://127.0.0.1:1/v1',
        DSCO_ENV_FILE='/dev/null', DSCO_PRICING_OFFLINE='1', DSCO_SECURE_STORE_NO_PROMPT='1',
        DSCO_NO_AUTO_SUPERVISE='1', DSCO_NO_SUPERVISE='1', DSCO_KITTY_AGENT_WINDOWS='0',
        DSCO_PIXEL_TUI='1', DSCO_KITTY_GRAPHICS='1', DSCO_PIXEL_TUI_DPR='1',
        DSCO_PIXEL_TUI_ANIMATIONS='0', DSCO_TUI_ANIMATIONS='0',
        DSCO_BANNER='0', DSCO_KITTY_BANNER='0', DSCO_ALLOW_NET='0',
        DSCO_AUTO_GOAL='0', DSCO_GOAL_NO_AUTORUN='1', DSCO_MCP_HEADLESS='0',
        DSCO_DISABLE_PROVIDER_FABRIC_AUTO='1', DSCO_AUTO_FALLBACK='0',
        DSCO_DISABLE_DEFAULT_FALLBACKS='1', DSCO_DYNAMIC_FAILOVER='0',
        DSCO_SYSTEM_PROMPT='Owned local regression. No inference is requested.')
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 35, 112, 1120, 700))
    proc = subprocess.Popen([str(binary), '--profile', 'worker', '--native', '-i',
        '--provider', 'openai', '-m', 'gpt54'], cwd=directory, env=env,
        stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    os.close(slave)
    raw = bytearray()
    def pump(seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if select.select([master], [], [], .02)[0]:
                try: raw.extend(os.read(master, 262144))
                except OSError: return
                assert len(raw) < 48 * 1024 * 1024, 'bounded terminal output'
    def wait(predicate, seconds=8):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if predicate(): return
            assert proc.poll() is None, ('exited', proc.returncode, bytes(raw[-3000:]))
            pump(.05)
        raise AssertionError(('timeout', bytes(raw[-2000:])))
    def send(text):
        data = (text + '\r').encode()
        assert os.write(master, data) == len(data)
        pump(.35)
    def trace(label, duration=700):
        previous = set(Path(directory).glob('dsco-ui-*.jsonl'))
        send(f'/ui trace {duration}ms')
        def completed():
            files = set(Path(directory).glob('dsco-ui-*.jsonl')) - previous
            return len(files) == 1 and next(iter(files)).stat().st_size > 0
        wait(completed, duration / 1000 + 8)
        path = next(iter(set(Path(directory).glob('dsco-ui-*.jsonl')) - previous))
        text = path.read_text()
        dest = output.with_suffix('.' + label + '.jsonl')
        dest.write_text(text)
        records = [json.loads(line) for line in text.splitlines()]
        assert records[-1]['event'] == 'end'
        return [r for r in records if r.get('id', '').startswith('window/')]
    try:
        wait(lambda: b'\x1b_G' in raw)
        pump(.7)
        assert not trace('startup'), 'no unsolicited panels at startup'
        send('/status')
        opened = trace('opened', 2500)
        ids = {r['id'] for r in opened}
        assert len(ids) == 1, opened
        initial = opened[0]['changes']
        assert initial['status'][1] == 4 and initial['visible'][1] == 1, initial
        assert initial['bytes'][1] > 400, initial
        assert any('revision' in r['changes'] for r in opened[1:]), 'elapsed updates with reduced motion'
        ident = next(iter(ids)).split('/')[1]
        send('/windows session')
        alias = trace('alias')
        assert {r['id'] for r in alias} == ids, 'alias reuses singleton'
        send('/windows hide')
        hidden = trace('hidden')
        assert hidden[0]['changes']['visible'][1] == 0, hidden
        send('/windows session')
        send('/windows close ' + ident)
        assert not trace('closed'), 'closed details never resurrect'
        send('/quit')
        wait(lambda: proc.poll() is not None)
        assert proc.returncode == 0, proc.returncode
        # Existing text status remains available outside native mode.
        ansi_env = dict(env, DSCO_PIXEL_TUI='0', TERM='xterm-256color')
        ansi = subprocess.run([str(binary), '--profile', 'worker', '--tui', '-i',
            '--provider', 'openai', '-m', 'gpt54'], input='/status\n/quit\n',
            text=True, capture_output=True, cwd=directory, env=ansi_env, timeout=15)
        assert ansi.returncode == 0 and 'Session Status' in ansi.stdout + ansi.stderr
        result = dict(ok=True, binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
            startup_opt_in=True, actual_native_session_panel=True, singleton=True,
            reduced_motion_live_updates=True, hide_respected=True, close_respected=True,
            ansi_fallback=True, provider_endpoint='loopback closed port; no inference requested')
        output.write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result))
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL); proc.wait(timeout=3)
        os.close(master)
