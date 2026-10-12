#!/usr/bin/env python3
"""Replay the real composer in Kitty's headless VT/reflow engine.

Run with: kitty +runpy 'exec(compile(open("tests/test_tui_kitty_reflow.py").read(),"test","exec"), globals())'
Raw PTY assertions cannot detect panels left on Kitty's reflowed screen/scrollback.
"""
import fcntl
import os
import pty
import select
import signal
import struct
import tempfile
import termios
import time

from kitty.fast_data_types import Screen

FIXTURE = os.path.abspath('build/composer_input_fixture')


def check_sequence(steps, draft=b'zoomcheck'):
    rows, cols = steps[0]
    screen = Screen(None, rows, cols)
    view = screen.test_create_write_buffer()
    with tempfile.TemporaryDirectory() as tmp:
        pid, fd = pty.fork()
        if pid == 0:
            os.environ.update(TERM='xterm-kitty', COLORTERM='truecolor',
                              DSCO_NO_SUPERVISE='1', DSCO_KITTY_AGENT_WINDOWS='0')
            os.execl(FIXTURE, FIXTURE, 'tui', tmp + '/lines')
        try:
            def feed(data):
                if len(data) > len(view):
                    for start in range(0, len(data), len(view)):
                        feed(data[start:start + len(view)])
                    return
                if data:
                    view[:len(data)] = data
                    screen.test_commit_write_buffer(view, data)
                    screen.test_parse_written_data()

            def drain(seconds):
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    if select.select([fd], [], [], .015)[0]:
                        try:
                            feed(os.read(fd, 65536))
                        except OSError:
                            break

            def screen_and_history():
                visible = [str(screen.line(i)) for i in range(screen.lines)]
                history = [str(screen.historybuf.line(i))
                           for i in range(screen.historybuf.count)]
                return visible, history

            def frames():
                visible, history = screen_and_history()
                return ([i + 1 for i, line in enumerate(visible)
                         if line.startswith(('┌', '╭')) and '─' in line],
                        [i + 1 for i, line in enumerate(history)
                         if line.startswith(('┌', '╭')) and '─' in line])

            def resize(r, c, delay=.24):
                fcntl.ioctl(fd, termios.TIOCSWINSZ,
                            struct.pack('HHHH', r, c, c * 10, r * 20))
                screen.resize(r, c)  # Kitty reflows BEFORE consuming SIGWINCH redraw.
                drain(delay)

            resize(rows, cols, .65)
            os.write(fd, draft)
            drain(.35)
            assert len(frames()[0]) == 1, ('initial', frames())
            header = frames()[0][0]
            assert header > 1
            # A transcript row immediately above the input must survive.
            sentinel_row = header - (4 if draft == b'/' else 1)
            feed(b'\x1b[' + str(sentinel_row).encode() + b';1HTRANSCRIPT_SENTINEL')
            for r, c in steps[1:]:
                resize(r, c)
                visible, history = screen_and_history()
                visible_frames, history_frames = frames()
                assert len(visible_frames) == 1 and not history_frames, ((r, c), frames())
                assert sum('TRANSCRIPT_SENTINEL' in line for line in
                           visible + history) == 1, ((r, c), 'transcript erased')
            os.write(fd, b'\r')
            drain(.15)
            os.write(fd, b'second\r')
            drain(.25)
        finally:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass
            os.close(fd)


check_sequence([(30, 100), (30, 80), (30, 60), (30, 30),
                (30, 60), (30, 100)])
check_sequence([(30, 100), (22, 80), (17, 60), (13, 48), (9, 36),
                (13, 48), (17, 60), (22, 80), (30, 100)])
check_sequence([(30, 100), (30, 80), (22, 68), (30, 100)], draft=b'/')
print('Kitty headless reflow: one panel, transcript retained through zoom: PASS')
