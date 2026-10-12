#!/usr/bin/env python3
"""Real agent/orchestrator with local mock provider and link-time speech peer."""
import argparse
import fcntl
import http.server
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
import threading
import time


def run(binary, mode, orchestrate, directory):
    requests, release = [], threading.Event()
    direction = "Tell the scout to focus on tests and preserve the current swarm work."
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_POST(self):
            requests.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
            number = len(requests)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            text = "Working on the original direction." if number == 1 else "Spoken direction applied in the same session."
            chunk = dict(choices=[dict(index=0, delta=dict(content=text), finish_reason=None)])
            self.wfile.write(("data: " + json.dumps(chunk) + "\n\n").encode())
            self.wfile.flush()
            if number == 1: assert release.wait(10), "speech not delivered during work"
            end = dict(choices=[dict(index=0, delta={}, finish_reason="stop")],
                       usage=dict(prompt_tokens=10, completion_tokens=8))
            self.wfile.write(("data: " + json.dumps(end) + "\n\ndata: [DONE]\n\n").encode())
            self.wfile.flush(); self.close_connection = True
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    speech_read, speech_write = os.pipe()
    report_read, report_write = os.pipe()
    env = dict(HOME=directory, TMPDIR=directory, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
               LANG="en_US.UTF-8", TERM="xterm-kitty", TERM_PROGRAM="kitty",
               DSCO_ENV_FILE="/dev/null", DSCO_CLOUD_SKIP_SAVED_ENV="1",
               DSCO_PRICING_OFFLINE="1", DSCO_SECURE_STORE_NO_PROMPT="1",
               DSCO_NO_AUTO_SUPERVISE="1", DSCO_NO_SUPERVISE="1",
               DSCO_KITTY_AGENT_WINDOWS="0", DSCO_PIXEL_TUI_DPR="1",
               DSCO_PIXEL_TUI_ANIMATIONS="0", DSCO_TUI_ANIMATIONS="0",
               DSCO_BANNER="0", DSCO_KITTY_BANNER="0", DSCO_ALLOW_NET="0", DSCO_ALLOW_RUN="0",
               DSCO_AUTO_GOAL="0", DSCO_GOAL_NO_AUTORUN="1", DSCO_MCP_HEADLESS="0",
               DSCO_DISABLE_PROVIDER_FABRIC_AUTO="1", DSCO_AUTO_FALLBACK="0",
               DSCO_DISABLE_DEFAULT_FALLBACKS="1", DSCO_DYNAMIC_FAILOVER="0",
               DSCO_NATIVE_VOICE_TEST_IN_FD=str(speech_read),
               DSCO_NATIVE_VOICE_TEST_REPORT_FD=str(report_write),
               LMSTUDIO_API_BASE=f"http://127.0.0.1:{server.server_port}/v1")
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 1000, 600))
    args = [str(binary), "--profile", "worker", f"--{mode}", "--hands-free",
            "--provider", "lmstudio", "-m", "local"]
    if orchestrate: args += ["--orchestrate", "--worker-model", "local"]
    proc = subprocess.Popen(args, cwd=directory, env=env, stdin=slave, stdout=slave,
                            stderr=slave, pass_fds=(speech_read, report_write), start_new_session=True)
    os.close(slave); os.close(speech_read); os.close(report_write)
    raw, reports = bytearray(), bytearray()
    def pump():
        for fd in select.select([master, report_read], [], [], .02)[0]:
            try: data = os.read(fd, 262144)
            except OSError: data = b""
            (raw if fd == master else reports).extend(data)
    def wait(predicate):
        until = time.monotonic() + 10
        while time.monotonic() < until:
            if predicate(): return
            pump()
            if proc.poll() is not None:
                pump()
                if predicate(): return
                raise AssertionError((mode, orchestrate, proc.returncode, raw[-2500:], reports))
        raise AssertionError((mode, orchestrate, "timeout", raw[-2500:], reports))
    try:
        wait(lambda: b"READY" in reports and raw.count(b"\x1b[?2004h") >= 1)
        # Native --hands-free must not replace the initial working turn.
        os.write(master, b"Coordinate the current task through the existing workers.\r")
        wait(lambda: len(requests) == 1 and raw.count(b"\x1b[?2004h") >= 2)
        # Leave a draft mid-codepoint/cursor while speech steers live work.
        os.write(master, "/voice hel".encode())
        os.write(speech_write, ("PARTIAL " + direction + "\n").encode())
        wait(lambda: b"DELIVERED PARTIAL" in reports)
        assert len(requests) == 1, "interim speech must not reach the model"
        os.write(speech_write, ("FINAL " + direction + "\n").encode())
        wait(lambda: b"DELIVERED FINAL" in reports and raw.count(b"\x1b[?2004h") >= 3)
        release.set()
        wait(lambda: len(requests) == 2 and b"SPEAK Spoken direction applied" in reports)
        # Await the idle remount before submitting retained draft as /voice help.
        wait(lambda: raw.count(b"\x1b[?2004h") >= 4)
        second = json.dumps(requests[1], ensure_ascii=False)
        assert direction in second and "Coordinate the current task" in second
        assert "Working on the original direction" in second
        assert requests[0]["model"] == requests[1]["model"] == "local"
        assert b"SPEAK Working on the original direction" not in reports, "stale answer was spoken"
        if orchestrate:
            names = [t.get("function", {}).get("name") for t in requests[1].get("tools", [])]
            assert "dispatch_agent" in names and "dispatch_topology" in names, names
        os.write(master, b"p\r")
        wait(lambda: raw.count(b"\x1b[?2004h") >= 5)
        os.write(speech_write, b"FINAL Stop voice.\n")
        wait(lambda: b"OFF" in reports)
        assert len(requests) == 2, "local voice control leaked into work"
        os.write(master, b"/voice status\r/quit\r")
        wait(lambda: proc.poll() is not None)
        assert proc.returncode == 0
        assert len(requests) == 2, "typed draft or control was sent to provider"
        if mode == "native": assert b"\x1b_G" in raw
        return dict(mode=mode, orchestrator=orchestrate, same_agent=True, same_model=True,
                    speech_steers_active_work=True, partial_not_submitted=True,
                    no_stale_spoken_answer=True, draft_preserved=True, local_stop=True)
    finally:
        release.set()
        if proc.poll() is None: os.killpg(proc.pid, signal.SIGKILL); proc.wait()
        for fd in (master, speech_write, report_read): os.close(fd)
        server.shutdown(); server.server_close()


def run_pause_control(binary, mode, action, directory):
    """Pause active work by voice, then steer or resume without leaking controls."""
    requests, release = [], threading.Event()
    direction = "Inspect the verifier receipts, then continue from the same conversation."

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args): pass

        def do_POST(self):
            requests.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
            number = len(requests)
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            text = "Original work is still running." if number == 1 else \
                   "Paused coordinator continued in the same session."
            chunk = dict(choices=[dict(index=0, delta=dict(content=text), finish_reason=None)])
            try:
                self.wfile.write(("data: " + json.dumps(chunk) + "\n\n").encode())
                self.wfile.flush()
                if number == 1:
                    release.wait(10)
                end = dict(choices=[dict(index=0, delta={}, finish_reason="stop")],
                           usage=dict(prompt_tokens=10, completion_tokens=8))
                self.wfile.write(("data: " + json.dumps(end) +
                                  "\n\ndata: [DONE]\n\n").encode())
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            self.close_connection = True

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    speech_read, speech_write = os.pipe()
    report_read, report_write = os.pipe()
    env = dict(HOME=directory, TMPDIR=directory, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
               LANG="en_US.UTF-8", TERM="xterm-kitty", TERM_PROGRAM="kitty",
               DSCO_ENV_FILE="/dev/null", DSCO_CLOUD_SKIP_SAVED_ENV="1",
               DSCO_PRICING_OFFLINE="1", DSCO_SECURE_STORE_NO_PROMPT="1",
               DSCO_NO_AUTO_SUPERVISE="1", DSCO_NO_SUPERVISE="1",
               DSCO_KITTY_AGENT_WINDOWS="0", DSCO_PIXEL_TUI_DPR="1",
               DSCO_PIXEL_TUI_ANIMATIONS="0", DSCO_TUI_ANIMATIONS="0",
               DSCO_BANNER="0", DSCO_KITTY_BANNER="0", DSCO_ALLOW_NET="0", DSCO_ALLOW_RUN="0",
               DSCO_AUTO_GOAL="0", DSCO_GOAL_NO_AUTORUN="1", DSCO_MCP_HEADLESS="0",
               DSCO_DISABLE_PROVIDER_FABRIC_AUTO="1", DSCO_AUTO_FALLBACK="0",
               DSCO_DISABLE_DEFAULT_FALLBACKS="1", DSCO_DYNAMIC_FAILOVER="0",
               DSCO_NATIVE_VOICE_TEST_IN_FD=str(speech_read),
               DSCO_NATIVE_VOICE_TEST_REPORT_FD=str(report_write),
               LMSTUDIO_API_BASE=f"http://127.0.0.1:{server.server_port}/v1")
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 1000, 600))
    proc = subprocess.Popen(
        [str(binary), "--profile", "worker", f"--{mode}", "--hands-free",
         "--provider", "lmstudio", "-m", "local"],
        cwd=directory, env=env, stdin=slave, stdout=slave, stderr=slave,
        pass_fds=(speech_read, report_write), start_new_session=True)
    os.close(slave); os.close(speech_read); os.close(report_write)
    raw, reports = bytearray(), bytearray()

    def pump():
        for fd in select.select([master, report_read], [], [], .02)[0]:
            try: data = os.read(fd, 262144)
            except OSError: data = b""
            (raw if fd == master else reports).extend(data)

    def wait(predicate, label):
        until = time.monotonic() + 12
        while time.monotonic() < until:
            if predicate(): return
            pump()
            if proc.poll() is not None:
                pump()
                if predicate(): return
                raise AssertionError((mode, action, label, proc.returncode,
                                      raw[-2500:], reports[-1000:]))
        raise AssertionError((mode, action, label, "timeout", raw[-2500:], reports[-1000:]))

    try:
        wait(lambda: b"READY" in reports and raw.count(b"\x1b[?2004h") >= 1,
             "voice/composer ready")
        os.write(master, b"Continue the current verification without restarting workers.\r")
        wait(lambda: len(requests) == 1 and raw.count(b"\x1b[?2004h") >= 2,
             "active provider turn")

        os.write(speech_write, b"FINAL Pause work.\n")
        wait(lambda: b"DELIVERED FINAL Pause work." in reports, "pause delivered")
        wait(lambda: b"INTERRUPT" in reports, "pause consumed by composer")
        # The control is local. It must not become a provider request while the
        # original stream is deliberately held open.
        time.sleep(.10); pump()
        assert len(requests) == 1
        assert "Pause work" not in json.dumps(requests[0], ensure_ascii=False)
        release.set()

        spoken = direction if action == "direction" else "Resume work."
        os.write(speech_write, ("FINAL " + spoken + "\n").encode())
        wait(lambda: ("DELIVERED FINAL " + spoken).encode() in reports,
             f"{action} delivered")
        wait(lambda: len(requests) >= 2, f"{action} resumed provider work")
        assert len(requests) == 2, requests
        second = json.dumps(requests[1], ensure_ascii=False)
        assert "Pause work" not in second, second
        assert "Resume work" not in second, second
        if action == "direction":
            assert direction in second
        else:
            assert direction not in second
        assert "Continue the current verification" in second
        assert requests[0]["model"] == requests[1]["model"] == "local"
        assert b"SPEAK Original work is still running" not in reports

        # Resume now keeps a live follow-up reader. Its mount is not the idle
        # REPL: wait for accepted final speech and the subsequent idle mount
        # before issuing cleanup commands, rather than racing stream handoff.
        wait(lambda: b"SPEAK Paused coordinator continued" in reports,
             "accepted final answer")
        idle_mount = 4
        wait(lambda: raw.count(b"\x1b[?2004h") >= idle_mount, "idle composer restored")
        os.write(master, b"/voice off\r")
        wait(lambda: b"OFF" in reports and raw.count(b"\x1b[?2004h") > idle_mount,
             "voice stopped")
        os.write(master, b"/quit\r")
        wait(lambda: proc.poll() is not None, "clean exit")
        assert proc.returncode == 0
        assert len(requests) == 2
        if mode == "native": assert b"\x1b_G" in raw
        return dict(mode=mode, pause_action=action, same_conversation=True,
                    control_not_sent_to_model=True, resumed_provider_work=True)
    finally:
        release.set()
        if proc.poll() is None: os.killpg(proc.pid, signal.SIGKILL); proc.wait()
        for fd in (master, speech_write, report_read): os.close(fd)
        server.shutdown(); server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/dsco_native_voice_fixture")
    args = parser.parse_args()
    binary = Path(args.binary).resolve(strict=True)
    for mode in ("tui", "native"):
        for orchestrate in (False, True):
            with tempfile.TemporaryDirectory(prefix="dsco-native-agent-") as directory:
                print(json.dumps(run(binary, mode, orchestrate, directory)))
        for action in ("direction", "resume"):
            with tempfile.TemporaryDirectory(prefix="dsco-native-pause-") as directory:
                print(json.dumps(run_pause_control(binary, mode, action, directory)))


if __name__ == "__main__": main()
