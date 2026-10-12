#!/usr/bin/env python3
"""Exercise Ctrl+O native voice handoff in ANSI and pixel compositor PTYs."""

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


def run_case(fixture: Path, mode: str, scenario: str, directory: str) -> dict:
    receipt = Path(directory) / f"{mode}-{scenario}.txt"
    env = dict(
        os.environ,
        HOME=directory,
        DSCO_ENV_FILE="/dev/null",
        TERM="xterm-kitty",
        COLORTERM="truecolor",
        DSCO_PIXEL_TUI="1" if mode == "native" else "0",
        DSCO_PIXEL_TUI_DPR="1",
        DSCO_PIXEL_TUI_ANIMATIONS="0",
        DSCO_TUI_ANIMATIONS="0",
        DSCO_NO_SUPERVISE="1",
        DSCO_KITTY_AGENT_WINDOWS="0",
    )
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 1000, 600))
    process = subprocess.Popen(
        [str(fixture), mode, str(receipt), scenario],
        env=env,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        start_new_session=True,
    )
    os.close(slave)
    output = bytearray()

    def pump() -> None:
        if select.select([master], [], [], 0.02)[0]:
            try:
                output.extend(os.read(master, 262144))
            except OSError:
                pass

    def wait(predicate, label: str) -> None:
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if predicate():
                return
            if process.poll() is not None:
                if predicate():
                    return
                raise AssertionError((mode, scenario, label, process.returncode, output[-2000:]))
            pump()
        raise AssertionError((mode, scenario, label, "timeout", output[-2000:]))

    try:
        wait(lambda: b"\x1b[?2004h" in output, "composer ready")
        # Place the byte cursor after the UTF-8 pi, then activate voice. Ctrl+O
        # must be consumed as a local action rather than submitting this draft.
        os.write(master, "draft π preserved".encode() + b"\x01" + b"\x1b[C" * 7 + b"\x0f")
        markers = {
            "final": "VOICE_PROMPT:",
            "stop": "VOICE_LOCAL_STOP",
            "unavailable": "VOICE_ERROR:",
            "denied": "VOICE_ERROR:",
            "toggle": "VOICE_STARTED",
        }
        wait(lambda: receipt.exists() and markers[scenario] in receipt.read_text(),
             "voice result")
        if scenario == "toggle":
            os.write(master, b"\x0f")
            wait(lambda: "VOICE_TOGGLED_OFF" in receipt.read_text(), "voice toggled off")
        os.write(master, b"!\r")
        wait(lambda: receipt.exists() and "DRAFT:" in receipt.read_text(), "draft resumed")
        wait(lambda: process.poll() is not None, "fixture exit")
        assert process.returncode == 0, (mode, scenario, process.returncode, output[-2000:])
        text = receipt.read_text()
        assert "DRAFT:draft π! preserved" in text, text
        if scenario == "final":
            assert "VOICE_PROMPT:spoken work request π" in text
        elif scenario == "stop":
            assert "VOICE_PROMPT:" not in text
        elif scenario == "toggle":
            assert "VOICE_PROMPT:" not in text
            assert "stale prompt" not in text
        elif scenario == "denied":
            assert "Microphone permission denied" in text
            if mode == "tui":
                assert b"Microphone permission denied" in output
        else:
            assert "speech recognition is unavailable" in text
            if mode == "tui":
                assert b"speech recognition is unavailable" in output
        if mode == "native":
            assert b"\x1b_G" in output, "native compositor did not remain active"
        return {
            "mode": mode,
            "scenario": scenario,
            "ok": True,
            "same_composer": True,
            "draft_and_utf8_cursor_retained": True,
            "spoken_stop_local": scenario != "stop" or "VOICE_PROMPT:" not in text,
        }
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        os.close(master)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", default="build/native_voice_fixture")
    args = parser.parse_args()
    fixture = Path(args.fixture).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="dsco-native-voice-") as directory:
        for mode in ("tui", "native"):
            for scenario in ("final", "stop", "toggle", "unavailable", "denied"):
                print(json.dumps(run_case(fixture, mode, scenario, directory)))


if __name__ == "__main__":
    main()
