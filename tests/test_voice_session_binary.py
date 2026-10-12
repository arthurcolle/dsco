#!/usr/bin/env python3
"""Prove explicit Realtime failure recovery through the real CLI (no audio/API)."""
import argparse
import fcntl
import http.server
import json
import os
from pathlib import Path
import pty
import select
import signal
import sqlite3
import struct
import subprocess
import tempfile
import termios
import threading
import time


def run(binary, mode, directory, active=False):
    requests, release = [], threading.Event()
    server = None
    if active:
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                requests.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                first = dict(choices=[dict(index=0, delta=dict(content="Continuity preserved."), finish_reason=None)])
                self.wfile.write(("data: " + json.dumps(first) + "\n\n").encode())
                self.wfile.flush()
                assert release.wait(8), "voice activation was not sent while streaming"
                final = dict(choices=[dict(index=0, delta={}, finish_reason="stop")],
                             usage=dict(prompt_tokens=10, completion_tokens=3))
                self.wfile.write(("data: " + json.dumps(final) + "\n\ndata: [DONE]\n\n").encode())
                self.wfile.flush()
                self.close_connection = True

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
    env = dict(HOME=directory, TMPDIR=directory, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
               LANG="en_US.UTF-8", TERM="xterm-kitty", TERM_PROGRAM="kitty",
               DSCO_ENV_FILE="/dev/null", DSCO_CLOUD_SKIP_SAVED_ENV="1",
               DSCO_PRICING_OFFLINE="1", DSCO_SECURE_STORE_NO_PROMPT="1",
               DSCO_NO_AUTO_SUPERVISE="1", DSCO_NO_SUPERVISE="1",
               DSCO_KITTY_AGENT_WINDOWS="0", DSCO_PIXEL_TUI_DPR="1",
               DSCO_PIXEL_TUI_ANIMATIONS="0", DSCO_TUI_ANIMATIONS="0",
               DSCO_BANNER="0", DSCO_KITTY_BANNER="0", DSCO_ALLOW_NET="0",
               DSCO_AUTO_GOAL="0", DSCO_GOAL_NO_AUTORUN="1", DSCO_MCP_HEADLESS="0",
               DSCO_DISABLE_PROVIDER_FABRIC_AUTO="1", DSCO_AUTO_FALLBACK="0",
               DSCO_DISABLE_DEFAULT_FALLBACKS="1", DSCO_DYNAMIC_FAILOVER="0")
    if active:
        env["LMSTUDIO_API_BASE"] = f"http://127.0.0.1:{server.server_port}/v1"
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 1000, 600))
    proc = subprocess.Popen([str(binary), "--profile", "worker", f"--{mode}", "-i",
                             "--provider", "lmstudio", "-m", "local"], cwd=directory,
                            env=env, stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    os.close(slave)
    raw = bytearray()

    def wait(predicate):
        end = time.monotonic() + 10
        while time.monotonic() < end:
            if predicate():
                return
            if proc.poll() is not None:
                if predicate():
                    return
                raise AssertionError((mode, proc.returncode, raw[-2000:]))
            if select.select([master], [], [], .02)[0]:
                try:
                    raw.extend(os.read(master, 262144))
                except OSError:
                    pass
        raise AssertionError((mode, "timeout", raw[-2000:]))

    try:
        wait(lambda: raw.count(b"\x1b[?2004h") >= 1)
        if active:
            os.write(master, b"Explain continuity in one sentence\r")
            wait(lambda: len(requests) == 1 and raw.count(b"\x1b[?2004h") >= 2)
        os.write(master, b"/voice realtime\r")
        if active:
            wait(lambda: raw.count(b"\x1b[?2004h") >= 3)
            os.write(master, b"/voice hel")
            release.set()
            def voice_failed():
                path = Path(directory) / ".dsco" / "baseline.db"
                with sqlite3.connect(f"file:{path}?mode=ro", uri=True) as connection:
                    return bool(connection.execute(
                        "SELECT 1 FROM events WHERE category='command' AND title='/voice' AND detail='failed'").fetchone())
            wait(voice_failed)
            wait(lambda: raw.count(b"\x1b[?2004h") >= 4)
        wait(lambda: raw.count(b"\x1b[?2004h") >= (4 if active else 2))
        os.write(master, b"p\r" if active else b"/voice help\r")
        wait(lambda: raw.count(b"\x1b[?2004h") >= (5 if active else 3))
        # Also exercise the submitted slash-command path independently.
        os.write(master, b"/voice realtime\r")
        wait(lambda: raw.count(b"\x1b[?2004h") >= (6 if active else 4))
        os.write(master, b"/quit\r")
        wait(lambda: proc.poll() is not None)
        assert proc.returncode == 0, (mode, proc.returncode)
        if mode == "tui":
            assert raw.count(b"no OpenAI API key") == 2, raw[-2000:]
            assert b"/voice realtime" in raw, "voice help did not execute"
        else:
            assert b"\x1b_G" in raw, "native interface was not exercised"
        if active:
            assert len(requests) == 1, "voice action or draft was sent to the text model"
        return dict(mode=mode, binary=str(binary), ok=True, activation="explicit /voice realtime",
                    missing_key_recovery=True, during_active_response=active, clean_exit=True)
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
        os.close(master)
        if server:
            release.set()
            server.shutdown()
            server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="./dsco")
    args = parser.parse_args()
    binary = Path(args.binary).resolve(strict=True)
    for mode in ("tui", "native"):
        with tempfile.TemporaryDirectory(prefix="dsco-voice-binary-") as directory:
            print(json.dumps(run(binary, mode, directory)))
        with tempfile.TemporaryDirectory(prefix="dsco-voice-active-") as directory:
            print(json.dumps(run(binary, mode, directory, active=True)))


if __name__ == "__main__":
    main()
