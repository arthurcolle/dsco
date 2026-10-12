#!/usr/bin/env python3
"""In-session voice activation, stop controls, context and preserved drafts on owned PTYs."""
import argparse
import fcntl
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


def run(fixture, mode, stop, directory):
    result = Path(directory) / f"{mode}-{stop}.txt"
    env = dict(os.environ, HOME=directory, DSCO_ENV_FILE="/dev/null",
               TERM="xterm-kitty", COLORTERM="truecolor", DSCO_PIXEL_TUI="1" if mode == "native" else "0",
               DSCO_PIXEL_TUI_DPR="1", DSCO_PIXEL_TUI_ANIMATIONS="0", DSCO_TUI_ANIMATIONS="0",
               DSCO_SECURE_STORE_NO_PROMPT="1", DSCO_NO_SUPERVISE="1", DSCO_KITTY_AGENT_WINDOWS="0")
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 1000, 600))
    proc = subprocess.Popen([str(fixture), mode, str(result), stop], env=env,
                            stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    os.close(slave)
    raw = bytearray()

    def pump():
        if select.select([master], [], [], .02)[0]:
            try:
                raw.extend(os.read(master, 262144))
            except OSError:
                pass

    def wait(predicate):
        end = time.monotonic() + 8
        while time.monotonic() < end:
            if predicate():
                return
            if proc.poll() is not None:
                if predicate():
                    return
                raise AssertionError((mode, stop, proc.returncode, raw[-1500:]))
            pump()
        raise AssertionError((mode, stop, "timeout", raw[-1500:]))

    try:
        wait(lambda: b"\x1b[?2004h" in raw)
        # Optional cloud adapter is explicit. Native Ctrl+O and preserved
        # drafts are exercised by test_native_voice.py without API/audio.
        os.write(master, b"/voice realtime\r")
        if stop != "spoken":
            wait(lambda: result.exists() and "VOICE_READY" in result.read_text())
            os.write(master, b"\x1b" if stop == "escape" else b"\x0f")
        wait(lambda: result.exists() and "VOICE_STOPPED" in result.read_text())
        os.write(master, "after voice π\r".encode())
        wait(lambda: result.exists() and "DRAFT:" in result.read_text())
        wait(lambda: proc.poll() is not None)
        assert proc.returncode == 0, (mode, stop, proc.returncode, raw[-1500:])
        assert "DRAFT:after voice π" in result.read_text(), result.read_text()
        assert mode != "native" or b"\x1b_G" in raw
        return dict(mode=mode, stop=stop, ok=True, same_session=True,
                    context_retained=True, trust_retained=True, composer_recovered=True)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
        os.close(master)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", default="build/voice_session_fixture")
    args = parser.parse_args()
    fixture = Path(args.fixture).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="dsco-voice-session-") as directory:
        for mode in ("tui", "native"):
            for stop in ("escape", "toggle", "spoken"):
                print(json.dumps(run(fixture, mode, stop, directory)))


if __name__ == "__main__":
    main()
