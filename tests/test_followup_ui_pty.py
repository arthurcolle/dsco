#!/usr/bin/env python3
"""Owned PTY proof of queue-only wake and exact Unicode draft/caret retention.
Run with --fixture /tmp/test_followup_ui_integration. No model or desktop UI.
"""
import argparse
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import tempfile
import termios
import time

parser = argparse.ArgumentParser()
parser.add_argument('--fixture', required=True)
args = parser.parse_args()
fixture = str(Path(args.fixture).resolve(strict=True))
with tempfile.TemporaryDirectory(prefix='dsco-followup-ui-') as directory:
    result = Path(directory) / 'result.txt'
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.update(TERM='xterm-kitty', COLORTERM='truecolor', DSCO_NO_SUPERVISE='1',
                          DSCO_KITTY_AGENT_WINDOWS='0', DSCO_KITTY_GRAPHICS='1')
        os.execl(fixture, fixture, str(result))
    reaped = False
    received = bytearray()

    def pump(seconds=.15):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            if select.select([fd], [], [], .02)[0]:
                try:
                    data = os.read(fd, 262144)
                except OSError:
                    return
                if not data:
                    return
                received.extend(data)
                if len(received) > 8 * 1024 * 1024:
                    del received[:4 * 1024 * 1024]

    def text():
        return result.read_text() if result.exists() else ''

    def wait(fragment):
        until = time.monotonic() + 8
        while time.monotonic() < until:
            if fragment in text():
                return
            pump(.05)
        raise AssertionError((fragment, text()))

    def send(data):
        os.write(fd, data)
        pump()

    try:
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 32, 100, 1000, 640))
        wait('ready\n')
        pump(.4)
        # Place cursor before the last character; keep it through two action wakes.
        send('draft πZ'.encode() + b'\x1b[D')
        send(b'\x11\r\x01')  # Ctrl-Q, edit selected, select all in queue editor
        # Fragmented paste with Unicode, newline and embedded terminal control.
        send(b'\x1b[200~edited ' + '🦉'.encode())
        send(b'\nsecond\x1b[201~')
        send(b'\x1b[D!')  # decoded arrow edits queue, not composer
        send(b'\x13')
        wait('action:1:42:3:edited 🦉\nsecon!d\n')
        assert text().count('return:1:\n') == 1, text()
        # Remove editor's stable entry, regardless of reader action-wakeup flag.
        send(b'\x04')
        wait('action:2:42:3:\n')
        assert text().count('return:1:\n') == 2, text()
        # Escape editor -> list -> composer, each as a standalone key.
        send(b'\x1b')
        send(b'\x1b')
        # Resize then finish draft. Neither action should submit/store queue edit as chat.
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 25, 60, 600, 500))
        os.kill(pid, signal.SIGWINCH)
        pump(.3)
        send(b'X\r')
        wait('return:1:draft πXZ\n')
        until = time.monotonic() + 5
        while time.monotonic() < until:
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                reaped = True
                assert os.waitstatus_to_exitcode(status) == 0, status
                break
            pump(.05)
        assert reaped, 'fixture did not exit'
        assert b'\x1b_G' in received, 'native raster transport was not exercised'
        print('PASS: owned PTY; global wake=false; replace/remove CAS; fragmented Unicode paste; arrows; caller-owned empty return; exact draft/caret after resize')
    finally:
        if not reaped:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        os.close(fd)
