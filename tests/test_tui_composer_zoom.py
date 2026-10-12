#!/usr/bin/env python3
"""Exercise an active ANSI composer through terminal zoom and resize, without an LLM."""
import argparse
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import tempfile
import termios
import time


def collect(fd, duration=0.3):
    output = bytearray()
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        if select.select([fd], [], [], min(0.01, max(0, deadline - time.monotonic())))[0]:
            try:
                output.extend(os.read(fd, 65536))
            except OSError:
                break
    return bytes(output)


def run(fixture):
    with tempfile.TemporaryDirectory() as tmp:
        result = Path(tmp) / "lines"
        pid, fd = pty.fork()
        if pid == 0:
            os.environ.update(TERM="xterm-kitty", COLORTERM="truecolor",
                              DSCO_NO_SUPERVISE="1", DSCO_KITTY_AGENT_WINDOWS="0")
            os.execl(fixture, fixture, "tui", str(result))
        reaped = False
        try:
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 1000, 600))
            assert b"\x1b[?2004h" in collect(fd, 0.65), "composer did not start"
            os.write(fd, b"/")
            assert b"/help" in collect(fd), "slash menu did not open"
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 6, 8, 290, 200))
            menu = collect(fd, 0.45)
            assert b"/hel" in menu, "slash menu not repainted in narrow grid"
            menu_positions = [(int(row), int(col)) for row, col in
                              re.findall(rb"\x1b\[(\d+);(\d+)H", menu)]
            assert menu_positions and all(1 <= row <= 6 and 1 <= col <= 8
                                          for row, col in menu_positions), menu_positions
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 4, 8, 290, 150))
            minimum = collect(fd, 0.4)
            positions = [(int(row), int(col)) for row, col in
                         re.findall(rb"\x1b\[(\d+);(\d+)H", minimum)]
            assert positions and all(1 <= row <= 4 and 1 <= col <= 8
                                     for row, col in positions), positions
            os.write(fd, b"\x15")  # Ctrl+U: clear '/' without submitting it.
            collect(fd, 0.2)
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 1000, 600))
            collect(fd, 0.3)
            os.write(fd, b"draft")
            assert b"draft" in collect(fd), "draft not rendered"
            # Font/pixel change with the same cell grid must still paint, with no typing.
            os.kill(pid, signal.SIGWINCH)
            same_size = collect(fd, 0.45)
            assert b"draft" in same_size and b"\x1b[" in same_size, "SIGWINCH did not repaint"
            # Zoom to a narrow grid: the long hint must not wrap into another panel.
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 17, 30, 680, 440))
            narrow = collect(fd, 0.5)
            assert b"draft" in narrow and b"send" in narrow, "narrow resize not redrawn"
            assert b"Ctrl+G swarm" not in narrow, "narrow footer exceeds terminal width"
            # At small heights, a long multiline draft must stay on-screen.
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 6, 10, 310, 200))
            small_view = collect(fd)
            assert b"aft" in small_view, "narrow viewport hid the entire draft"
            for i in range(12):
                os.write(fd, b"\x1b\r" + str(i).encode())
            small = collect(fd, 0.5)
            assert b"11" in small, "cursor line disappeared in small grid"
            painted_rows = [int(n) for n in re.findall(rb"\x1b\[(\d+);\d+H", small)]
            assert painted_rows and max(painted_rows) <= 6, painted_rows
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 6, 8, 290, 200))
            tiny = collect(fd, 0.45)
            assert b"11" in tiny, "8-column zoom lost the draft"
            painted = [(int(row), int(col)) for row, col in
                       re.findall(rb"\x1b\[(\d+);(\d+)H", tiny)]
            assert painted and all(1 <= row <= 6 and 1 <= col <= 8 for row, col in painted), painted
            # Zoom out again without losing the in-flight draft.
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 1000, 600))
            wide = collect(fd, 1.3)
            assert b"11" in wide and b"Ctrl+G swarm" in wide, "wide resize not redrawn"
            # A later resize must not sweep transcript rows just because the
            # composer once occupied row 1 in the tiny window.
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 28, 90, 900, 560))
            later = collect(fd, 0.5)
            assert b"11" in later, "later resize lost the draft"
            assert not re.search(rb"\x1b\[(?:[1-9]|10);1H\x1b\[2K", later), \
                "resizing after zoom erased transcript rows"
            os.write(fd, b"\r")
            collect(fd, 0.15)
            os.write(fd, b"second\r")
            collect(fd, 0.35)
            done, status = os.waitpid(pid, os.WNOHANG)
            if not done:
                collect(fd, 0.6)
                done, status = os.waitpid(pid, os.WNOHANG)
            assert done and os.waitstatus_to_exitcode(status) == 0, "fixture did not finish"
            reaped = True
            assert result.read_text().splitlines() == ["draft"] + [str(i) for i in range(12)] + ["second"], result.read_text()
        finally:
            if not reaped:
                try:
                    os.kill(pid, signal.SIGKILL)
                    os.waitpid(pid, 0)
                except (ProcessLookupError, ChildProcessError):
                    pass
            os.close(fd)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", default="build/composer_input_fixture")
    args = parser.parse_args()
    run(str(Path(args.fixture).resolve(strict=True)))
    print("composer zoom/redraw: PASS")
