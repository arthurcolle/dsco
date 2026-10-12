#!/usr/bin/env python3
"""Check native default selection and ANSI fallback in owned PTYs.

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


def capture(binary, flags, graphics_off=False, pixel_mode=None, config_mode=None,
            term='xterm-kitty', program='kitty', kitty_id=True,
            piped_input=False, expect_native=False, motion=None, reduced=False,
            metrics=None):
    with tempfile.TemporaryDirectory(prefix='dsco-splash-') as directory:
        snapshot = Path(directory, 'native.ppm')
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 1440, 960))
        env = os.environ.copy()
        for key in ('DSCO_PIXEL_TUI', 'DSCO_KITTY_GRAPHICS', 'DSCO_SUPERVISED',
                    'DSCO_SPLASH', 'DSCO_BANNER_CELLS', 'DSCO_MCP_SERVER',
                    'DSCO_TUI_ANIM', 'NO_MOTION', 'ACCESSIBILITY_REDUCE_MOTION'):
            env.pop(key, None)
        env_file = Path(directory, 'profile.env')
        if config_mode is not None:
            env_file.write_text(f'DSCO_PIXEL_TUI={config_mode}\n')
        env.update(HOME=directory, TMPDIR=directory, TERM=term, TERM_PROGRAM=program, DSCO_PROFILE='worker',
                   OPENAI_API_KEY='fixture-only', DSCO_ALLOW_NET='0',
                   OPENAI_API_BASE='http://127.0.0.1:1/v1',
                   COLORTERM='truecolor', DSCO_NO_AUTO_SUPERVISE='1',
                   DSCO_NO_SUPERVISE='1', DSCO_ENV_FILE=str(env_file),
                   DSCO_SECURE_STORE_NO_PROMPT='1',
                   DSCO_KITTY_AGENT_WINDOWS='0', DSCO_PRICING_OFFLINE='1',
                   DSCO_TUI_COMPOSER='1', DSCO_BANNER='1', DSCO_KITTY_BANNER='1',
                   DSCO_PIXEL_TUI_SESSION_SNAPSHOT=str(snapshot))
        if motion is not None:
            env['DSCO_TUI_ANIM'] = '1' if motion else '0'
        env['DSCO_REDUCED_MOTION'] = '1' if reduced else '0'
        if kitty_id:
            env['KITTY_WINDOW_ID'] = '1'
        else:
            env.pop('KITTY_WINDOW_ID', None)
        if pixel_mode is not None:
            env['DSCO_PIXEL_TUI'] = pixel_mode
        if graphics_off:
            env['DSCO_KITTY_GRAPHICS'] = '0'
        proc = subprocess.Popen([binary, '--provider', 'openai', '-m', 'gpt54', *flags],
                                stdin=subprocess.PIPE if piped_input else slave, stdout=slave,
                                stderr=slave, env=env, start_new_session=True)
        if piped_input:
            proc.stdin.write(b'/quit\n')
            proc.stdin.close()
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
                if not piped_input and ((ready_at is not None and elapsed - ready_at > .2) or elapsed > 6) and (quit_at is None or elapsed - quit_at > 3):
                    os.write(master, b'/quit\r')
                    quit_at = elapsed
                status = proc.poll()
                if status is not None:
                    break
            if status is None:
                text = re.sub(rb'\x1b_G.*?\x1b\\', b'', output, flags=re.S)
                text = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', text)
                raise AssertionError(f'{flags}: CLI did not exit after /quit; tail={text[-1200:]!r}')
            if not piped_input and quit_at is None:
                text = re.sub(rb'\x1b_G.*?\x1b\\', b'', output, flags=re.S)
                text = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', text)
                raise AssertionError(f'{flags}: CLI exited before /quit; status={status}; tail={text[-1200:]!r}')
            assert status == 0, f'{flags}: CLI exit {status}'
            assert snapshot.exists() == expect_native, f'{flags}: unexpected native compositor state'
            # An animation frame may span many protocol chunks. Each chunk
            # must retain a=f or Kitty replaces the root image instead.
            continuing_frame = False
            frame_chunks = 0
            for packet in re.finditer(rb'\x1b_G([^;\x1b]+);', output):
                fields = dict(part.split(b'=', 1) for part in packet[1].split(b',') if b'=' in part)
                if continuing_frame:
                    assert fields.get(b'a') == b'f', 'animation continuation lost a=f'
                    frame_chunks += 1
                continuing_frame = fields.get(b'a') == b'f' and fields.get(b'm') == b'1'
            splash_motion = motion is not False and not reduced and not piped_input
            if not graphics_off and not expect_native and kitty_id and splash_motion:
                assert frame_chunks > 0, 'no chunked animation frames exercised'
                assert re.search(rb'\x1b_Ga=a,[^;]*s=3', output), 'animation loop never started'
            elif not expect_native:
                assert frame_chunks == 0, 'static/reduced-motion startup uploaded an animation'
            cell_frames = output.count(b'\x1b[?2026h')
            if not expect_native and (graphics_off or not kitty_id):
                assert (cell_frames > 1) == splash_motion, f'cell splash motion mismatch: {cell_frames}'
            dimensions = set()
            for match in re.finditer(rb'\x1b_G([^;\x1b]+);', output):
                fields = dict(part.split(b'=', 1) for part in match[1].split(b',') if b'=' in part)
                if fields.get(b'a') in (b't', b'T') and fields.get(b'f') in (b'24', b'32'):
                    if b's' in fields and b'v' in fields:
                        dimensions.add((int(fields[b's']), int(fields[b'v'])))
            if metrics is not None:
                metrics.append(dict(flags=flags, motion=motion, reduced=reduced,
                                    ready_ms=None if ready_at is None else round(ready_at * 1000, 3),
                                    startup_animation_chunks=frame_chunks, cell_frames=cell_frames,
                                    piped_input=piped_input, bytes=len(output)))
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


def capture_without_tty(binary):
    with tempfile.TemporaryDirectory(prefix='dsco-no-tty-') as directory:
        snapshot = Path(directory, 'native.ppm')
        env = os.environ.copy()
        env.update(HOME=directory, TMPDIR=directory, TERM='xterm-kitty', TERM_PROGRAM='kitty', KITTY_WINDOW_ID='1',
                   DSCO_PROFILE='worker', DSCO_SECURE_STORE_NO_PROMPT='1',
                   OPENAI_API_KEY='fixture-only', DSCO_ALLOW_NET='0',
                   OPENAI_API_BASE='http://127.0.0.1:1/v1',
                   DSCO_PIXEL_TUI='1', DSCO_NO_SUPERVISE='1',
                   DSCO_NO_AUTO_SUPERVISE='1', DSCO_ENV_FILE='/dev/null',
                   DSCO_KITTY_AGENT_WINDOWS='0', DSCO_PRICING_OFFLINE='1',
                   DSCO_PIXEL_TUI_SESSION_SNAPSHOT=str(snapshot))
        result = subprocess.run([binary, '--provider', 'openai', '-m', 'gpt54', '--native', '-i'], input='/quit\n',
                                capture_output=True, text=True, env=env, timeout=20)
        assert result.returncode == 0, result.stderr[-1200:]
        assert not snapshot.exists(), 'native activated without terminal output'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', default='./dsco')
    parser.add_argument('--output')
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve(strict=True))
    native = capture(binary, [], expect_native=True)
    requested = capture(binary, ['--native'], pixel_mode='0', expect_native=True)
    metrics = []
    tui = capture(binary, ['--tui'], pixel_mode='1', metrics=metrics)
    animated = capture(binary, ['--tui'], motion=True, metrics=metrics)
    static = capture(binary, ['--tui'], motion=False, metrics=metrics)
    default_reduced = capture(binary, ['--tui'], reduced=True, metrics=metrics)
    cells = capture(binary, ['--tui'], graphics_off=True, metrics=metrics)
    reduced = capture(binary, ['--tui'], motion=True, reduced=True, metrics=metrics)
    disabled = capture(binary, ['--native'], graphics_off=True)
    env_off = capture(binary, [], pixel_mode='0')
    config_off = capture(binary, [], config_mode='0')
    config_override = capture(binary, ['--native'], config_mode='0', expect_native=True)
    unsupported = capture(binary, ['--native'], term='xterm-256color',
                          program='Apple_Terminal', kitty_id=False)
    piped = capture(binary, ['--native', '-i'], piped_input=True, motion=True, metrics=metrics)
    capture_without_tty(binary)
    assert native and requested, 'Kitty default and explicit native lack a pixel canvas'
    assert tui and max(width for width, _ in tui) > 240, f'ANSI splash has no pixel canvas: {tui}'
    assert env_off == tui, 'environment opt-out changed ANSI splash resolution'
    assert animated == tui and reduced == tui and static == tui and default_reduced == tui, 'motion policy changed splash resolution'
    assert not cells, 'graphics opt-out ignored by animated cell fallback'
    assert config_off == tui and config_override, 'saved config opt-out or CLI override failed'
    assert not disabled, f'explicit graphics opt-out ignored: {disabled}'
    assert not unsupported, f'unsupported terminal emitted Kitty pixels: {unsupported}'
    assert piped, 'piped input failed to use ANSI splash'
    receipt = {'ok': True, 'binary': binary,
                      'sha256': hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
                      'default_native_pixels': native, 'requested_native_pixels': requested,
                      'tui_pixels': tui, 'graphics_off_pixels': disabled,
                      'env_off_pixels': env_off, 'config_off_pixels': config_off,
                      'config_override_pixels': config_override,
                      'unsupported_pixels': unsupported,
                      'piped_pixels': piped, 'no_tty_fallback': True, 'clean_quit': True,
                      'startup_motion': metrics}
    if args.output:
        Path(args.output).write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
