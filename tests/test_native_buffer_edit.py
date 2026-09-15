#!/usr/bin/env python3
"""Edit and save a real managed buffer through the native CLI in an owned PTY."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import signal
import shutil
import struct
import subprocess
import tempfile
import termios
import time


def run(binary, trace_output=None):
    with tempfile.TemporaryDirectory(prefix="dsco-native-buffer-edit-") as directory:
        env = dict(HOME=directory, TMPDIR=directory, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
                   LANG="en_US.UTF-8", TERM="xterm-kitty", TERM_PROGRAM="kitty",
                   OPENAI_API_KEY="fixture-only", OPENAI_API_BASE="http://127.0.0.1:1/v1",
                   DSCO_ENV_FILE="/dev/null", DSCO_PRICING_OFFLINE="1",
                   DSCO_SECURE_STORE_NO_PROMPT="1", DSCO_NO_AUTO_SUPERVISE="1",
                   DSCO_NO_SUPERVISE="1", DSCO_KITTY_AGENT_WINDOWS="0",
                   DSCO_PIXEL_TUI="1", DSCO_KITTY_GRAPHICS="1", DSCO_PIXEL_TUI_DPR="1",
                   DSCO_BANNER="0", DSCO_KITTY_BANNER="0", DSCO_ALLOW_NET="0",
                   DSCO_AUTO_GOAL="0", DSCO_GOAL_NO_AUTORUN="1",
                   DSCO_DISABLE_PROVIDER_FABRIC_AUTO="1", DSCO_AUTO_FALLBACK="0",
                   DSCO_DISABLE_DEFAULT_FALLBACKS="1", DSCO_DYNAMIC_FAILOVER="0",
                   DSCO_SYSTEM_PROMPT="Owned local slash-command test; no inference requested.")

        def buffer_command(action, arguments):
            proc = subprocess.run([str(binary), "buffer", action, json.dumps(arguments)],
                                  cwd=directory, env=env, capture_output=True, timeout=20)
            assert proc.returncode == 0, proc.stderr[-1000:]
            reply = json.loads(proc.stdout)
            assert reply.get("ok"), reply
            return reply

        name = "AI managed draft"
        original = "AI-created draft\n"
        edit = "Human edit: π 雪 🙂"
        created = buffer_command("create", dict(name=name, kind="scratch", content=original))
        content_path = Path(created["buffer"]["content_path"])
        assert content_path.resolve().is_relative_to(Path(directory).resolve()), content_path
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 35, 112, 1120, 700))
        proc = subprocess.Popen([str(binary), "--profile", "worker", "--native", "-i",
                                 "--provider", "openai", "-m", "gpt54"],
                                cwd=directory, env=env, stdin=slave, stdout=slave,
                                stderr=slave, start_new_session=True)
        os.close(slave)
        output = bytearray()

        def pump(seconds):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                if select.select([master], [], [], .02)[0]:
                    try:
                        data = os.read(master, 262144)
                    except OSError:
                        return
                    output.extend(data)
                    assert len(output) < 64 * 1024 * 1024, "bounded PTY output"

        def send(data):
            assert os.write(master, data) == len(data)
            pump(.25)

        try:
            until = time.monotonic() + 8
            while b"\x1b_G" not in output and time.monotonic() < until:
                pump(.05)
            assert b"\x1b_G" in output, "native framebuffer never started"
            pump(.6)
            if trace_output:
                send(b"/ui trace 3s\r")
                pump(.3)
                assert list(Path(directory).glob("dsco-ui-*.jsonl")), "trace command did not create an artifact"
            send(f'/buffer edit "{name}"\r'.encode())
            pump(.8)
            send(b"\x07")  # Ctrl+G focuses the bound buffer, with its cursor at the end.
            send(edit.encode())
            assert content_path.read_text() == original, "typing must not save implicitly"
            send(b"\x13")  # Ctrl+S must reach the governed canonical buffer write.
            until = time.monotonic() + 5
            while content_path.read_text() != original + edit and time.monotonic() < until:
                pump(.05)
            assert content_path.read_text() == original + edit, "native Ctrl+S did not persist the edit"
            send(b"\x1b")
            if trace_output:
                trace_path = next(Path(directory).glob("dsco-ui-*.jsonl"))
                until = time.monotonic() + 5
                while trace_path.stat().st_size == 0 and time.monotonic() < until:
                    pump(.05)
                records = [json.loads(line) for line in trace_path.read_text().splitlines()]
                assert records[0]["event"] == "begin" and records[-1]["event"] == "end", records[-1:]
                assert records[-1]["reason"] == "duration", records[-1]
                dirty = [r["changes"]["dirty"] for r in records if r.get("id", "").startswith("window/") and "dirty" in r.get("changes", {})]
                assert [0, 1] in dirty and [1, 0] in dirty, dirty
                assert any(r.get("event") == "frame" for r in records)
                assert original not in trace_path.read_text() and edit not in trace_path.read_text(), "trace copied document text"
                trace_output.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(trace_path, trace_output)
            send(b"/quit\r")
            until = time.monotonic() + 5
            while proc.poll() is None and time.monotonic() < until:
                pump(.05)
            assert proc.poll() == 0, "native CLI did not exit cleanly"
            saved = buffer_command("read", dict(name=name))
            assert saved["text"] == original + edit, saved
            assert saved["buffer"]["buffer_id"] == created["buffer"]["buffer_id"], saved
            assert saved["buffer"]["revision"] == hashlib.sha256((original + edit).encode()).hexdigest()
            print(json.dumps(dict(ok=True, binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                                  editor="native", canonical_buffer_saved=True, unicode=True,
                                  explicit_save=True, clean_quit=True, pty_bytes=len(output),
                                  trace=str(trace_output) if trace_output else None)))
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=3)
            os.close(master)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="./dsco")
    parser.add_argument("--trace-output", type=Path)
    args = parser.parse_args()
    run(Path(args.binary).resolve(strict=True), args.trace_output)
