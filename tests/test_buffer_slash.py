#!/usr/bin/env python3
"""Prove persistent buffer slash commands using the real CLI in an owned PTY.

Only local slash commands are submitted; no model prompt is sent. The native
variant verifies a raster frame and durable contents, not text extracted from
graphics. HOME, credentials, keychains and user terminal windows are untouched.
"""
import argparse
import base64
import errno
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


class TerminalText:
    """Discard terminal control sequences incrementally, including Kitty pixels."""

    def __init__(self):
        self.state = 'text'
        self.graphics = False

    def feed(self, data):
        text = bytearray()
        for byte in data:
            if self.state == 'text':
                if byte == 27:
                    self.state = 'escape'
                elif byte in (9, 10, 13) or byte >= 32:
                    text.append(byte)
            elif self.state == 'escape':
                if byte == ord('['):
                    self.state = 'csi'
                elif byte in (ord(']'), ord('_'), ord('P'), ord('^')):
                    self.state = 'string_start' if byte == ord('_') else 'string'
                else:
                    self.state = 'text'
            elif self.state == 'csi':
                if 0x40 <= byte <= 0x7e:
                    self.state = 'text'
            elif self.state == 'string_start':
                self.graphics |= byte == ord('G')
                self.state = 'string_escape' if byte == 27 else 'string'
            elif self.state == 'string':
                if byte == 27:
                    self.state = 'string_escape'
                elif byte == 7:
                    self.state = 'text'
            elif self.state == 'string_escape':
                self.state = 'text' if byte == ord('\\') else 'string'
        return text


def cli(binary, env, *command):
    proc = subprocess.run([binary, 'buffer', *command], env=env,
                          stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=20, check=False)
    assert len(proc.stdout) < 1024 * 1024, 'buffer CLI output exceeded bound'
    assert proc.returncode == 0, f'buffer CLI {command[0]} exited {proc.returncode}: stdout={proc.stdout[-1200:]!r}; stderr={proc.stderr[-1200:]!r}'
    result = json.loads(proc.stdout)
    assert result.get('ok') is True, result
    return result


def run(binary, native=False):
    name = 'pty-buffer-proof'
    content = 'PTYPERSIST snow 雪 rocket 🚀'
    commands = [f'/buffer new {name}', f'/buffer append {name} "{content}"',
                '/buffers', f'/buffer read {name}', '/quit']
    with tempfile.TemporaryDirectory(prefix='dsco-buffer-slash-') as directory:
        snapshot = Path(directory, 'native.ppm')
        store = Path(directory, 'buffers')
        env = os.environ.copy()
        for key in ('DSCO_PIXEL_TUI', 'DSCO_KITTY_GRAPHICS', 'DSCO_SUPERVISED',
                    'DSCO_MCP_SERVER', 'DSCO_SPLASH', 'DSCO_BANNER_CELLS'):
            env.pop(key, None)
        env.update(TERM='xterm-kitty', TERM_PROGRAM='kitty', KITTY_WINDOW_ID='1',
                   COLORTERM='truecolor', DSCO_NO_AUTO_SUPERVISE='1',
                   DSCO_KITTY_AGENT_WINDOWS='0', DSCO_PRICING_OFFLINE='1',
                   DSCO_TUI_COMPOSER='1', DSCO_BUFFER_DIR=str(store),
                   DSCO_BANNER='0', DSCO_KITTY_BANNER='0',
                   DSCO_PIXEL_TUI_SESSION_SNAPSHOT=str(snapshot))
        if not native:
            env['DSCO_KITTY_GRAPHICS'] = '0'
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 1440, 960))
        try:
            proc = subprocess.Popen([binary, '--native' if native else '--tui'],
                                    stdin=slave, stdout=slave, stderr=slave,
                                    env=env, start_new_session=True)
        finally:
            os.close(slave)
        os.set_blocking(master, False)
        text_filter = TerminalText()
        transcript, phase_text = bytearray(), bytearray()
        output_bytes, sent = 0, 0
        started = time.monotonic()
        sent_at = started
        ready_at = None
        status = None
        read_visible = False
        metadata_responses = 0
        list_visible = False
        try:
            while time.monotonic() - started < 20:
                if select.select([master], [], [], .04)[0]:
                    try:
                        data = os.read(master, 262144)
                    except OSError as exc:
                        if exc.errno not in (errno.EIO, errno.EAGAIN):
                            raise
                        data = b''
                    output_bytes += len(data)
                    assert output_bytes < 64 * 1024 * 1024, 'PTY output exceeded bound'
                    plain = text_filter.feed(data)
                    transcript.extend(plain)
                    phase_text.extend(plain)
                    assert len(transcript) < 2 * 1024 * 1024, 'text transcript exceeded bound'
                    if len(phase_text) > 262144:
                        del phase_text[:-262144]
                now = time.monotonic()
                if ready_at is None and (b'Ctrl+G swarm' in phase_text or
                                         (native and snapshot.exists())):
                    ready_at = now
                advance = False
                if sent == 0:
                    # Native snapshots can begin during startup, before its
                    # input loop owns the PTY. Allow the same six-second startup
                    # window used by the existing splash proof.
                    advance = ready_at is not None and now - ready_at >= .25 and (not native or now - started >= 6)
                elif sent <= 2:
                    # Echoing a command is insufficient: wait for backend result
                    # metadata, or the actual owned content file in pixel mode.
                    metadata = b'Buffer ID:' in phase_text or b'"buffer_id"' in phase_text
                    persisted = False
                    if native:
                        paths = list(Path(store, 'main').glob('*.txt'))
                        if len(paths) == 1:
                            try:
                                persisted = paths[0].read_bytes() == (b'' if sent == 1 else content.encode())
                            except FileNotFoundError:
                                pass  # A concurrent atomic replacement is retried.
                    advance = now - sent_at >= .2 and (persisted if native else metadata)
                    if advance and metadata:
                        metadata_responses += 1
                elif sent == 3:
                    listed = b'Buffers (1)' in phase_text or b'"buffers":[' in phase_text
                    advance = now - sent_at >= (.65 if native else .2) and (native or listed)
                    list_visible |= listed
                elif sent == 4:
                    read_visible |= content.encode() in phase_text
                    advance = now - sent_at >= (.65 if native else .2) and (native or read_visible)
                if advance and sent < len(commands):
                    encoded = (commands[sent] + '\r').encode()
                    assert os.write(master, encoded) == len(encoded), 'short PTY command write'
                    sent += 1
                    sent_at = now
                    phase_text.clear()
                status = proc.poll()
                if status is not None:
                    break
            assert status is not None, f'CLI deadline after {sent} commands; tail={bytes(transcript[-1800:])!r}'
            assert sent == len(commands), f'CLI exited before all slash commands: {sent}; tail={bytes(transcript[-1800:])!r}'
            assert status == 0, f'CLI exit {status}; tail={bytes(transcript[-1800:])!r}'
            assert b'Buffer command failed:' not in transcript, bytes(transcript[-1800:])
            saved = cli(binary, env, 'read', name)
            listed = cli(binary, env, 'list')
            assert saved['text'] == content, saved
            assert base64.b64decode(saved['base64']) == content.encode(), saved
            assert len(listed['buffers']) == 1 and listed['buffers'][0]['buffer_id'] == saved['buffer']['buffer_id'], listed
            content_path = Path(saved['buffer']['content_path'])
            assert content_path.is_relative_to(store.resolve()), content_path
            assert content_path.read_bytes() == content.encode(), 'owned file contents differ'
            assert saved['buffer']['revision'] == hashlib.sha256(content.encode()).hexdigest(), saved
            dimensions = None
            if native:
                frame = snapshot.read_bytes()
                header = frame.split(b'\n', 3)
                assert len(header) == 4 and header[0] == b'P6' and header[2] == b'255', 'invalid native PPM frame'
                dimensions = [int(v) for v in header[1].split()]
                assert len(dimensions) == 2 and min(dimensions) > 0 and len(header[3]) == dimensions[0] * dimensions[1] * 3, 'incomplete native frame'
                assert text_filter.graphics, 'native PTY emitted no graphics'
            else:
                assert metadata_responses == 2 and list_visible and read_visible, 'slash results missing from text TUI'
                assert not snapshot.exists(), 'text TUI unexpectedly started native compositor'
            return {'ok': True, 'mode': 'native' if native else 'tui',
                    'commands': len(commands), 'clean_quit': True,
                    'persisted_bytes': len(content.encode()),
                    'read_text_in_transcript': read_visible,
                    'native_frame': dimensions, 'pty_bytes': output_bytes,
                    'elapsed_seconds': round(time.monotonic() - started, 3)}
        finally:
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait(timeout=3)
            os.close(master)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', default='./dsco')
    parser.add_argument('--native', action='store_true', help='exercise the native pixel compositor')
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve(strict=True))
    binary_hash = hashlib.sha256(Path(binary).read_bytes()).hexdigest()
    result = run(binary, args.native)
    assert hashlib.sha256(Path(binary).read_bytes()).hexdigest() == binary_hash, 'binary changed during the proof run; rerun after build completes'
    result.update(binary=binary, sha256=binary_hash)
    print(json.dumps(result))


if __name__ == '__main__':
    main()
