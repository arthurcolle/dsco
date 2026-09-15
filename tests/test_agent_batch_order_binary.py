#!/usr/bin/env python3
"""Exercise the interactive scheduler through a real binary and local SSE server.

Mixed batches must preserve write/read order and bypass pre-write cache hits;
pure readers must still overlap. No model inference or external services.
"""
import argparse
import fcntl
import hashlib
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

from test_buffer_slash import TerminalText


def run(binary, mixed_only=False):
    requests, errors, observations = [], [], []
    parallel = threading.Barrier(2)
    state = {"mutations": 0}
    with tempfile.TemporaryDirectory(prefix="dsco-batch-order-") as tmp:
        sample = Path(tmp, "sample.txt")
        sample.write_text("ORIGINAL_OWNED_CONTENT\n")

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                try:
                    if self.path == "/observation":
                        body = f"OBSERVED_MUTATIONS={state['mutations']}".encode()
                        self.send_response(200)
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)
                        return
                    assert self.path in ("/reader/a", "/reader/b"), self.path
                    parallel.wait(timeout=4)
                    body = ("PARALLEL_READER_PROOF " + self.path).encode()
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except Exception as exc:
                    errors.append("reader overlap: " + repr(exc))
                    self.close_connection = True

            def do_POST(self):
                try:
                    if self.path == "/mutation":
                        self.rfile.read(int(self.headers.get("Content-Length", "0")))
                        time.sleep(.2)
                        state["mutations"] += 1
                        body = f"APPLIED_MUTATIONS={state['mutations']}".encode()
                        self.send_response(200)
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)
                        return
                    assert self.path == "/v1/chat/completions", self.path
                    req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                    requests.append(req)
                    stage = len(requests) - 1
                    outputs = {m.get("tool_call_id"): m.get("content")
                               for m in req.get("messages", []) if m.get("role") == "tool"}
                    if stage == 1:
                        observations.append(dict(check="write_then_read",
                            passed="FIRST_VERIFIED_WRITE" in json.dumps(outputs.get("read_first"))))
                    elif stage == 2:
                        observations.append(dict(check="cached_read_after_write",
                            passed="SECOND_LONGER_VERIFIED_WRITE" in json.dumps(outputs.get("read_second"))))
                    elif stage == 3:
                        observations.append(dict(check="reader_overlap", passed=all(
                            "PARALLEL_READER_PROOF" in json.dumps(outputs.get(call))
                            for call in ("parallel_a", "parallel_b"))))
                    elif stage == 4:
                        observations.append(dict(check="http_mutation_before_read",
                            passed="OBSERVED_MUTATIONS=1" in json.dumps(outputs.get("observe_mutation"))))
                    elif stage == 5:
                        observations.append(dict(check="http_mutation_never_cached",
                            passed=state["mutations"] == 2 and
                            "APPLIED_MUTATIONS=2" in json.dumps(outputs.get("mutate_again"))))
                    elif stage == 7:
                        repeated = Path(tmp, "repeated.txt")
                        observations.append(dict(check="repeated_shell_executes_twice",
                            passed=repeated.exists() and repeated.read_text() == "xx"))
                    elif stage == 10:
                        observations.append(dict(check="write_invalidates_read_across_turns",
                            passed="THIRD__LONGER_VERIFIED_WRITE" in json.dumps(outputs.get("read_third"))))
                    if stage == 8:
                        state["read_mtime_ns"] = sample.stat().st_mtime_ns
                    elif stage == 9:
                        # Reproduce a filesystem with coarse timestamp resolution:
                        # changed bytes, same size, and the exact previous mtime.
                        stamp = state["read_mtime_ns"]
                        os.utime(sample, ns=(stamp, stamp))

                    if stage == 0:
                        calls = [("pause", "bash", {"command": "sleep 0.2"}),
                                 ("write_first", "write_file", {"path": str(sample),
                                  "content": "FIRST_VERIFIED_WRITE\n"}),
                                 ("read_first", "read_file", {"path": str(sample)})]
                    elif stage == 1:
                        calls = [("write_second", "write_file", {"path": str(sample),
                                  "content": "SECOND_LONGER_VERIFIED_WRITE\n"}),
                                 ("read_second", "read_file", {"path": str(sample)})]
                    elif stage == 2 and not mixed_only:
                        calls = [("parallel_" + label, "http_request", {
                            "url": f"http://127.0.0.1:{server.server_port}/reader/{label}",
                            "method": "GET"}) for label in ("a", "b")]
                    elif stage in (3, 4) and not mixed_only:
                        calls = [("mutate" if stage == 3 else "mutate_again", "http_request", {
                            "url": f"http://127.0.0.1:{server.server_port}/mutation", "method": "POST"})]
                        if stage == 3:
                            calls.append(("observe_mutation", "http_request", {
                                "url": f"http://127.0.0.1:{server.server_port}/observation"}))
                    elif stage in (5, 6) and not mixed_only:
                        calls = [(f"shell_repeat_{stage}", "bash", {
                            "command": "printf x >> repeated.txt"})]
                    elif stage in (7, 9) and not mixed_only:
                        calls = [("read_before_third" if stage == 7 else "read_third",
                                  "read_file", {"path": str(sample)})]
                    elif stage == 8 and not mixed_only:
                        calls = [("write_third", "write_file", {"path": str(sample),
                                  "content": "THIRD__LONGER_VERIFIED_WRITE\n"})]
                    else:
                        assert stage == (2 if mixed_only else 10), "unexpected extra provider request"
                        calls = []
                    if calls:
                        delta = {"tool_calls": [dict(index=i, id=call_id, type="function",
                                 function=dict(name=name, arguments=json.dumps(arguments)))
                                 for i, (call_id, name, arguments) in enumerate(calls)]}
                        finish = "tool_calls"
                    else:
                        delta = {"content": "BATCH_ORDER_FIXTURE_FINISHED"}
                        finish = "stop"
                    events = [dict(id="batch-fixture", choices=[dict(index=0, delta=delta,
                                   finish_reason=None)]),
                              dict(id="batch-fixture", choices=[dict(index=0, delta={},
                                   finish_reason=finish)],
                                   usage=dict(prompt_tokens=10, completion_tokens=10, cost=0))]
                    payload = "".join("data: " + json.dumps(e) + "\n\n" for e in events)
                    data = (payload + "data: [DONE]\n\n").encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                except Exception as exc:
                    errors.append("provider fixture: " + repr(exc))
                    self.close_connection = True

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = dict(HOME=tmp, TMPDIR=tmp, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
                   TERM="xterm-256color", LANG="en_US.UTF-8", OPENAI_API_KEY="fixture-only",
                   OPENAI_API_BASE=f"http://127.0.0.1:{server.server_port}/v1",
                   DSCO_ENV_FILE="/dev/null", DSCO_PRICING_OFFLINE="1",
                   DSCO_SECURE_STORE_NO_PROMPT="1", DSCO_DISABLE_DEFAULT_FALLBACKS="1",
                   DSCO_AUTO_FALLBACK="0", DSCO_DYNAMIC_FAILOVER="0",
                   DSCO_DISABLE_PROVIDER_FABRIC_AUTO="1", DSCO_NO_AUTO_SUPERVISE="1",
                   DSCO_NO_SUPERVISE="1", DSCO_KITTY_AGENT_WINDOWS="0", DSCO_PIXEL_TUI="0",
                   DSCO_KITTY_GRAPHICS="0", DSCO_MCP_HEADLESS="0", DSCO_BANNER="0",
                   DSCO_KITTY_BANNER="0", DSCO_TOOL_PROXY="1",
                   DSCO_AUTO_GOAL="0",
                   DSCO_SYSTEM_PROMPT="Owned local fixture. Follow the user request.",
                   DSCO_HARD_TURN_CEILING="14")
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
        proc = None
        transcript = bytearray()
        text_filter = TerminalText()
        sent = quit_sent = False
        try:
            proc = subprocess.Popen([str(binary), "--profile", "worker", "--tui", "-i",
                                     "--provider", "openai", "-m", "fixture-model",
                                     "--trust-tier", "trusted", "--approval-mode", "never"],
                                    cwd=tmp, env=env, stdin=slave, stdout=slave, stderr=slave,
                                    start_new_session=True)
            os.close(slave)
            slave = -1
            started = time.monotonic()
            ready_at = finished_at = None
            while time.monotonic() - started < 30:
                if select.select([master], [], [], .04)[0]:
                    try:
                        data = os.read(master, 262144)
                    except OSError:
                        data = b""
                    transcript.extend(text_filter.feed(data))
                    assert len(transcript) < 2 * 1024 * 1024, "PTY output limit"
                if b"Ctrl+G swarm" in transcript:
                    ready_at = ready_at or time.monotonic()
                if not sent and ready_at and time.monotonic() - ready_at > .3:
                    os.write(master, b"Update sample.txt twice, verify each write, then read both local endpoints.\r")
                    sent = True
                if b"BATCH_ORDER_FIXTURE_FINISHED" in transcript:
                    finished_at = finished_at or time.monotonic()
                    if not quit_sent and time.monotonic() - finished_at > .5:
                        os.write(master, b"/quit\r")
                        quit_sent = True
                if proc.poll() is not None:
                    break
            assert proc.poll() == 0 and quit_sent, (proc.poll(), observations,
                                                  bytes(transcript[-3000:]))
            assert not errors, (errors, observations, [req.get("messages", [])[-2:]
                                                       for req in requests])
            assert len(requests) == (3 if mixed_only else 11), len(requests)
            assert sample.read_text() == ("SECOND_LONGER_VERIFIED_WRITE\n" if mixed_only
                                          else "THIRD__LONGER_VERIFIED_WRITE\n")
            assert len(observations) == (2 if mixed_only else 7), observations
            assert all(row["passed"] for row in observations), observations
            return observations
        finally:
            if proc is not None and proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=3)
            if slave >= 0:
                os.close(slave)
            os.close(master)
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("./dsco"))
    parser.add_argument("--mixed-only", action="store_true",
                        help="Isolate mixed-batch ordering and cache checks")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    before = hashlib.sha256(binary.read_bytes()).hexdigest()
    observations = run(binary, args.mixed_only)
    assert hashlib.sha256(binary.read_bytes()).hexdigest() == before, "binary changed during test"
    print(json.dumps(dict(binary=str(binary), sha256=before, passed=True,
                          checks=observations), indent=2))


if __name__ == "__main__":
    main()
