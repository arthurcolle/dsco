#!/usr/bin/env python3
"""Compare actual CLI splash pixel dimensions in identical Kitty-shaped PTYs.

No prompt is submitted to a model. /quit closes each owned test process.
"""
import argparse
import errno
import fcntl
import hashlib
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
import time


def capture(binary, flags, graphics_off=False, inherit_native=False):
    with tempfile.TemporaryDirectory(prefix='dsco-splash-') as directory:
        snapshot = Path(directory, 'native.ppm')
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 1440, 960))
        env = os.environ.copy()
        for key in ('DSCO_PIXEL_TUI', 'DSCO_KITTY_GRAPHICS', 'DSCO_SUPERVISED',
                    'DSCO_SPLASH', 'DSCO_BANNER_CELLS', 'DSCO_MCP_SERVER'):
            env.pop(key, None)
        env.update(TERM='xterm-kitty', TERM_PROGRAM='kitty', KITTY_WINDOW_ID='1',
                   COLORTERM='truecolor', DSCO_NO_AUTO_SUPERVISE='1',
                   DSCO_KITTY_AGENT_WINDOWS='0', DSCO_PRICING_OFFLINE='1',
                   DSCO_TUI_COMPOSER='1', DSCO_BANNER='1', DSCO_KITTY_BANNER='1',
                   DSCO_PIXEL_TUI_SESSION_SNAPSHOT=str(snapshot))
        if inherit_native:
            env['DSCO_PIXEL_TUI'] = '1'
        if graphics_off:
            env['DSCO_KITTY_GRAPHICS'] = '0'
        proc = subprocess.Popen([binary, *flags], stdin=slave, stdout=slave,
                                stderr=slave, env=env, start_new_session=True)
        os.set_blocking(master, False)
        os.close(slave)
        output = bytearray()
        status = None
        started = time.monotonic()
        quit_at = None
        ready_at = None
        try:
            while time.monotonic() - started < 20:
                if select.select([master], [], [], .05)[0]:
                    try:
                        data = os.read(master, 262144)
                    except OSError as exc:
                        if exc.errno not in (errno.EIO, errno.EAGAIN):
                            raise
                        data = b''
                    output.extend(data)
                assert len(output) < 64 * 1024 * 1024, 'splash output exceeded bound'
                elapsed = time.monotonic() - started
                # Inspect only the latest chunk and overlap: repeatedly scanning
                # all prior image payloads can stall the PTY consumer itself.
                if ready_at is None and b'Ctrl+G swarm' in output[-262208:]:
                    ready_at = elapsed
                if ((ready_at is not None and elapsed - ready_at > .2) or elapsed > 6) and (quit_at is None or elapsed - quit_at > 3):
                    os.write(master, b'/quit\r')
                    quit_at = elapsed
                status = proc.poll()
                if status is not None:
                    break
            if status is None:
                text = re.sub(rb'\x1b_G.*?\x1b\\', b'', output, flags=re.S)
                text = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', text)
                raise AssertionError(f'{flags}: CLI did not exit after /quit; tail={text[-1200:]!r}')
            assert quit_at is not None, f'{flags}: CLI exited before /quit'
            assert status == 0, f'{flags}: CLI exit {status}'
            assert not snapshot.exists(), f'{flags}: native compositor activated in text TUI'
            dimensions = set()
            for match in re.finditer(rb'\x1b_G([^;\x1b]+);', output):
                fields = dict(part.split(b'=', 1) for part in match[1].split(b',') if b'=' in part)
                if fields.get(b'a') in (b't', b'T') and fields.get(b'f') in (b'24', b'32'):
                    if b's' in fields and b'v' in fields:
                        dimensions.add((int(fields[b's']), int(fields[b'v'])))
            return sorted(dimensions)
        finally:
            if status is None:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.kill()
                proc.wait(timeout=3)
            os.close(master)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', default='./dsco')
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve(strict=True))
    plain = capture(binary, [])
    tui = capture(binary, ['--tui'], inherit_native=True)
    disabled = capture(binary, ['--tui'], graphics_off=True)
    assert plain and max(width for width, _ in plain) > 240, f'plain splash has no pixel canvas: {plain}'
    assert tui == plain, f'--tui changed splash resolution: {plain} vs {tui}'
    assert not disabled, f'explicit graphics opt-out ignored: {disabled}'
    print(json.dumps({'ok': True, 'binary': binary,
                      'sha256': hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
                      'plain_pixels': plain, 'tui_pixels': tui, 'graphics_off_pixels': disabled,
                      'native_compositor_inactive': True, 'clean_quit': True}))


if __name__ == '__main__':
    main()
