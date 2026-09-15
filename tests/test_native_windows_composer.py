#!/usr/bin/env python3
"""Real composer/native renderer in an owned PTY; never touches another terminal."""
import argparse
import fcntl
import json
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
parser.add_argument("--fixture", default="build/native_windows_composer_fixture")
args = parser.parse_args()
fixture = str(Path(args.fixture).resolve(strict=True))
with tempfile.TemporaryDirectory(prefix="dsco-native-composer-") as directory:
    status_path = Path(directory) / "status.json"
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.update(TERM="xterm-kitty", COLORTERM="truecolor", LC_ALL="C",
                          DSCO_NO_SUPERVISE="1", DSCO_KITTY_AGENT_WINDOWS="0")
        os.execl(fixture, fixture, str(status_path))
    output = bytearray()
    reaped = False

    def pump(seconds=0.08):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if select.select([fd], [], [], max(0, min(0.03, end-time.monotonic())))[0]:
                try:
                    data = os.read(fd, 262144)
                except OSError:
                    return
                if not data:
                    return
                output.extend(data)
                if len(output) > 8 * 1024 * 1024:
                    del output[:4 * 1024 * 1024]

    def status():
        try:
            return json.loads(status_path.read_text())
        except (FileNotFoundError, ValueError):
            return {}

    def wait(predicate, seconds=6):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            current = status()
            if predicate(current):
                return current
            pump(0.05)
        raise AssertionError(status())

    def send(data, pause=0.10):
        os.write(fd, data)
        pump(pause)

    def resize(cols, rows):
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, cols*10, rows*20))
        os.kill(pid, signal.SIGWINCH)
        pump(0.35)

    def window(identifier=1):
        return next(w for w in status()["windows"] if w["id"] == identifier)

    def mouse(button, x, y, release=False):
        col, row = max(1, int(x // 10) + 1), max(1, int(y // 20) + 1)
        return f"\x1b[<{button};{col};{row}{'m' if release else 'M'}".encode()

    def unfocus():
        if status().get("focused"):
            send(b"\x1b")
        assert not status()["focused"]

    def select_workflow():
        if not status()["focused"]:
            send(b"\x07")
        for _ in range(3):
            if status()["focused_id"] == 1:
                return
            send(b"\t")
        raise AssertionError(status())

    def action(button):
        w = window()
        x, y = w["x"] + w["width"] * (button + 0.5) / 4, w["y"] + w["height"] - 17
        send(mouse(0, x, y) + mouse(0, x, y, True), 0.03)

    try:
        resize(110, 32)
        wait(lambda s: len(s.get("windows", [])) == 2)
        pump(0.6)  # first composer read intentionally drains terminal replies
        draft = "draft π-leftpreserved\nsecond".encode()
        send(b"\x1b[200~" + draft + b"\x1b[201~")
        send(b"\x07")
        assert status()["focused"], status()
        selected = status()["focused_id"]
        before = window(selected)
        send(b"\x1b[D")
        assert window(selected)["x"] != before["x"]
        before = window(selected)
        send(b"\x1b[1;2D")
        assert window(selected)["width"] < before["width"]
        send(b"\x1b[1;5C")
        assert window(selected)["width"] == before["width"]
        select_workflow()
        send(b"z")
        assert window()["zoomed"]
        send(b"z")
        assert not window()["zoomed"]
        w = window()
        send(mouse(65, w["x"]+80, w["y"]+60))
        assert window()["scroll"] > 0
        w = window()
        x, y = w["x"]+80, w["y"]+10
        send(mouse(0, x, y) + mouse(32, x+40, y+20) + mouse(0, x+40, y+20, True))
        assert window()["x"] > w["x"] and status()["captured_id"] == 0
        w = window()
        x, y = w["x"]+w["width"]-8, w["y"]+w["height"]-8
        send(mouse(0, x, y) + mouse(32, x+40, y+20) + mouse(0, 5, 5, True))
        assert window()["width"] > w["width"] and status()["captured_id"] == 0
        send(b"t")
        send(b"r")
        select_workflow()
        send(b"\t")
        assert status()["focused_id"] == 2
        send(b"x")
        assert len(status()["windows"]) == 1 and status()["focused_id"] == 1
        assert status()["returned"] == 0 and status()["actions"] == 0
        # Enter under window focus never submits the pre-existing draft.
        send(b"\r")
        assert status()["returned"] == 0
        if not status()["focused"]:
            send(b"\x07")
        send(b"?")
        assert not status()["focused"]
        send(b"\x7f")
        # Delayed malformed packet tails, including newline, stay out of input.
        send(b"\x1b[<0;1;bad\n", 0.12)
        send(b"9M")
        send(b"\x1b[<" + b"9"*100 + b"\r;1;1m")
        assert status()["returned"] == 0
        # A valid packet may span pauses much longer than the old 30ms reader.
        w = window()
        packet = mouse(0, w["x"]+70, w["y"]+10)
        cut = packet.rfind(b";") + 1
        send(packet[:cut], 0.15)
        send(packet[cut:])
        assert status()["captured_id"] == 1, (packet, status())
        send(mouse(0, w["x"]+70, w["y"]+10, True))
        assert status()["focused"] and status()["focused_id"] == 1 and status()["captured_id"] == 0, status()
        # Paste returns focus to the composer; no pasted newline is a submit.
        send(b"\x1b[200~\x1b[201~")
        assert not status()["focused"]
        resize(98, 30)
        resize(110, 32)
        select_workflow()
        unfocus()
        # Put the byte cursor before 'preserved', then exercise both handoffs.
        send(b"\x01\x1b[A" + b"\x1b[C"*len("draft π-left"))
        action(0)
        wait(lambda s: s.get("phase") == 1 and s.get("actions") == 1)
        assert status()["returned"] == 1
        action(2)
        wait(lambda s: s.get("boundary_waiting"), seconds=2)
        assert status()["returned"] == 1, "streaming click interrupted composer before boundary"
        # Preserve both pending paste bytes and a partial end marker across
        # the reader handoff, with no newline accidentally submitting input.
        send(b"\x1b[200~!\x1b[20", 0.03)
        wait(lambda s: s.get("phase") == 2 and s.get("returned") == 2)
        assert 0 < status()["first_event"] < status()["second_event"]
        assert status()["actions"] == 2
        send(b"1~")
        send(b"\r", 0.3)
        final = wait(lambda s: s.get("phase") == 3)
        assert final["submitted"] == "draft π-left!preserved\nsecond", final
        assert final["failures"] == 0, final
        end = time.monotonic()+8
        while time.monotonic()<end:
            child, code = os.waitpid(pid, os.WNOHANG)
            if child:
                reaped = True
                assert os.waitstatus_to_exitcode(code) == 0
                break
            pump(0.05)
        assert reaped, "fixture did not exit"
        assert b"\x1b[?1002h" in output and b"\x1b[?1002l" in output, "mouse mode restoration"
        print("PASS: native real-composer keys, drag, resize, wheel, malformed/fragmented SGR, retained UTF-8 draft/cursor and partial paste marker, idle wake-up, streaming boundary, actions delivered once")
    finally:
        if not reaped:
            try:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
            except ProcessLookupError:
                pass
        os.close(fd)
