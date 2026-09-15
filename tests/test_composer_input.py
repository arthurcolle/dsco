#!/usr/bin/env python3
"""Verify actual composer type-ahead in ANSI/native modes with owned PTYs."""
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


def run_case(fixture, mode, case, directory):
    results = Path(directory) / f"{mode}-{case}.txt"
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.update(TERM="xterm-kitty", COLORTERM="truecolor", LC_ALL="C",
                          DSCO_NO_SUPERVISE="1", DSCO_KITTY_AGENT_WINDOWS="0")
        os.execl(fixture, fixture, mode, str(results), case)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 1000, 600))
    output = bytearray()
    started = time.monotonic()
    sent_at = None
    interrupted_at = None
    resumed_at = None
    reaped = False
    try:
        while time.monotonic() - started < 5:
            if select.select([fd], [], [], 0.01)[0]:
                try:
                    data = os.read(fd, 262144)
                except OSError:
                    data = b""
                output.extend(data)
            ready = b"\x1b[?2004h" in output
            if ready and sent_at is None:
                if case != "early":
                    # Isolate prompt-to-prompt loss from the initial drain.
                    time.sleep(0.12)
                sent_at = time.monotonic()
                # Real late DA/CPR replies are protocol traffic, not input.
                if case == "handoff":
                    os.write(fd, b"draft\xe2")
                elif case == "malformed":
                    # An incomplete codepoint must not absorb the Enter byte.
                    os.write(fd, b"first\xe2\rsecond\r")
                else:
                    os.write(fd, b"\x1b[?1;2c\x1b[12;30Rfirst\rsecond\r")
            received = results.read_text().splitlines() if results.exists() else []
            if case == "handoff" and sent_at is not None:
                if interrupted_at is None and time.monotonic() - sent_at >= 0.12:
                    interrupted_at = time.monotonic()
                    os.kill(pid, signal.SIGUSR1)
                if received == [""] and resumed_at is None:
                    resumed_at = time.monotonic()
                    assert resumed_at - interrupted_at < 0.75, "UTF-8 prevented prompt handoff"
                    os.write(fd, b"\x82\xac-ok\r")
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                reaped = True
                assert os.waitstatus_to_exitcode(status) == 0, (mode, received)
                expected = ["", "draft€-ok"] if case == "handoff" else \
                    ["first�", "second"] if case == "malformed" else ["first", "second"]
                assert received == expected, (mode, case, received)
                result = {"mode": mode, "case": case,
                        "submissions": received,
                        "input_to_process_completion_ms": round(
                            (time.monotonic() - sent_at) * 1000, 1)}
                if resumed_at is not None:
                    result["interrupt_to_handoff_ms"] = round(
                        (resumed_at - interrupted_at) * 1000, 1)
                return result
        raise AssertionError({"mode": mode, "case": case,
                              "received": received, "ready": ready,
                              "failure": "input lost or submit did not return"})
    finally:
        if not reaped:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", default="build/composer_input_fixture")
    parser.add_argument("--mode", choices=("tui", "native", "both"), default="both")
    parser.add_argument("--case", choices=("early", "queued", "handoff", "malformed", "all"), default="all")
    args = parser.parse_args()
    fixture = str(Path(args.fixture).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="dsco-composer-input-") as directory:
        modes = ("tui", "native") if args.mode == "both" else (args.mode,)
        cases = ("early", "queued", "handoff", "malformed") if args.case == "all" else (args.case,)
        for mode in modes:
            for case in cases:
                print(json.dumps(run_case(fixture, mode, case, directory)))


if __name__ == "__main__":
    main()
