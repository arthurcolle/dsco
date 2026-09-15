#!/usr/bin/env python3
"""Real native trace mouse controls and AI handoff in an exclusively owned PTY."""
import argparse
import fcntl
import hashlib
import http.server
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time


def run(binary, output):
    with tempfile.TemporaryDirectory(prefix="dsco-trace-controls-") as directory:
        requests, errors, diagnoses = [], [], []
        completed_reads = []
        expected_draft = "draft π left !preserved"

        def content(message):
            value = message.get("content", "")
            return value if isinstance(value, str) else json.dumps(value, ensure_ascii=False)

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                try:
                    request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                    messages = request["messages"]
                    last_user = next(content(m) for m in reversed(messages) if m["role"] == "user")
                    requests.append(last_user)
                    assert len(requests) <= 5, requests
                    match = re.search(r"Trace path \(JSON string\): (.+)", last_user)
                    if match:
                        trace = Path(json.loads(match[1]))
                        assert trace.resolve().is_relative_to(Path(directory).resolve()), trace
                        assert trace.read_text().splitlines()[-1].startswith('{"event":"end"'), trace
                        already_read = (messages[-1]["role"] == "tool" and
                                        "dsco.native_trace.v1" in content(messages[-1]))
                        if already_read:
                            completed_reads.append(str(trace))
                            delta = {"content": "The local trace was read successfully. TRACE_DIAGNOSIS_COMPLETE"}
                            finish = "stop"
                        else:
                            diagnoses.append(str(trace))
                            assert diagnoses.count(str(trace)) == 1, "duplicate diagnostic action"
                            delta = {"tool_calls": [{"index": 0, "id": f"trace_read_{len(diagnoses)}",
                                "type": "function", "function": {"name": "read_file", "arguments":
                                json.dumps(dict(path=str(trace), limit=12))}}]}
                            finish = "tool_calls"
                    else:
                        assert last_user == expected_draft, last_user
                        delta, finish = {"content": "The preserved draft arrived intact. DRAFT_VERIFIED"}, "stop"
                    events = [dict(id="trace-control-fixture", choices=[dict(index=0, delta=delta, finish_reason=None)]),
                              dict(id="trace-control-fixture", choices=[dict(index=0, delta={}, finish_reason=finish)],
                                   usage=dict(prompt_tokens=10, completion_tokens=10, cost=0))]
                    body = ("".join("data: " + json.dumps(e) + "\n\n" for e in events) + "data: [DONE]\n\n").encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except Exception as exc:
                    errors.append(repr(exc))
                    self.close_connection = True

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = dict(HOME=directory, TMPDIR=directory, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
                   LANG="en_US.UTF-8", TERM="xterm-kitty", TERM_PROGRAM="kitty",
                   OPENAI_API_KEY="fixture-only", OPENAI_API_BASE=f"http://127.0.0.1:{server.server_port}/v1",
                   DSCO_ENV_FILE="/dev/null", DSCO_PRICING_OFFLINE="1", DSCO_SECURE_STORE_NO_PROMPT="1",
                   DSCO_NO_AUTO_SUPERVISE="1", DSCO_NO_SUPERVISE="1", DSCO_KITTY_AGENT_WINDOWS="0",
                   DSCO_PIXEL_TUI="1", DSCO_KITTY_GRAPHICS="1", DSCO_PIXEL_TUI_DPR="1",
                   DSCO_PIXEL_TUI_ANIMATIONS="0", DSCO_TUI_ANIMATIONS="0",
                   DSCO_BANNER="0", DSCO_KITTY_BANNER="0", DSCO_ALLOW_NET="0",
                   DSCO_AUTO_GOAL="0", DSCO_GOAL_NO_AUTORUN="1", DSCO_MCP_HEADLESS="0",
                   DSCO_DISABLE_PROVIDER_FABRIC_AUTO="1", DSCO_AUTO_FALLBACK="0",
                   DSCO_DISABLE_DEFAULT_FALLBACKS="1", DSCO_DYNAMIC_FAILOVER="0",
                   DSCO_SYSTEM_PROMPT="Owned local native UI regression. Follow the requested trace read.")
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 35, 112, 1120, 700))
        proc = subprocess.Popen([str(binary), "--profile", "worker", "--native", "-i",
                                 "--provider", "openai", "-m", "gpt54"],
                                cwd=directory, env=env, stdin=slave, stdout=slave,
                                stderr=slave, start_new_session=True)
        os.close(slave)
        raw = bytearray()

        def pump(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if select.select([master], [], [], .02)[0]:
                    try:
                        raw.extend(os.read(master, 262144))
                    except OSError:
                        return
                    assert len(raw) < 48 * 1024 * 1024, "bounded PTY output"

        def wait(predicate, seconds=8):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                assert not errors, errors
                if predicate():
                    return
                pump(.05)
            raise AssertionError(f"timed out; {len(requests)} requests; {errors}")

        def send(data):
            assert os.write(master, data) == len(data)
            pump(.2)

        def packet(button, x, y, released=False):
            return f"\x1b[<{button};{int(x // 10) + 1};{int(y // 20) + 1}{'m' if released else 'M'}".encode()

        try:
            wait(lambda: b"\x1b_G" in raw)
            pump(.7)
            # Discover actual retained control geometry via the public trace,
            # rather than assuming a screenshot position or test-only API.
            send(b"/ui trace 500ms\r")
            wait(lambda: any(p.stat().st_size for p in Path(directory).glob("dsco-ui-*.jsonl")))
            seed = next(Path(directory).glob("dsco-ui-*.jsonl"))
            output.parent.mkdir(parents=True,exist_ok=True)
            output.with_suffix(".seed.jsonl").write_text(seed.read_text())
            records = [json.loads(line) for line in seed.read_text().splitlines()]
            control = next(r["changes"] for r in records if r.get("id") == "ui_diagnostics")
            x = control["x"][1] + control["width"][1] - 20
            y = control["y"][1] + control["height"][1] / 2
            click = packet(0, x, y) + packet(0, x, y, True)
            pump(.8)
            assert not requests, "automatic completion must never request inference"
            send(b"\x1b[200~draft \xcf\x80 left preserved\x1b[201~")
            send(b"\x01" + b"\x1b[C" * len("draft π left "))
            # Drag-away and release-only input must not dispatch an action.
            send(packet(0, x, y) + packet(32, 20, 30) + packet(0, 20, 30, True))
            send(packet(0, x, y, True))
            assert not requests
            send(click + click)
            wait(lambda: len(completed_reads) == 1)
            pump(.9)
            assert len(requests) == 2 and len(diagnoses) == 1
            # The ready control returns to Record after delivery. A double click
            # starts only one ten-second recording, while the draft stays live.
            send(click + click)
            wait(lambda: len(list(Path(directory).glob("dsco-ui-*.jsonl"))) == 2)
            recorded = next(p for p in Path(directory).glob("dsco-ui-*.jsonl") if p != seed)
            wait(lambda: recorded.stat().st_size > 0, seconds=13)
            pump(.8)  # No input drives the completion repaint; reduced motion is on.
            assert len(requests) == 2, "recording completion sent an unrequested AI turn"
            trace_text = recorded.read_text()
            records = [json.loads(line) for line in trace_text.splitlines()]
            assert records[0]["duration_ms"] == 10000 and records[-1]["reason"] == "duration"
            assert "draft π left" not in trace_text
            send(click + click)
            wait(lambda: len(completed_reads) == 2)
            pump(.9)
            assert len(requests) == 4 and len(diagnoses) == 2
            send(b"!\r")
            wait(lambda: len(requests) == 5)
            pump(.9)
            send(b"/quit\r")
            wait(lambda: proc.poll() is not None)
            assert proc.returncode == 0 and not errors
            output.parent.mkdir(parents=True, exist_ok=True)
            output.with_suffix(".trace.jsonl").write_text(trace_text)
            result = dict(ok=True, binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                          record_control=True, seconds=10, automatic_completion=True, reduced_motion=True,
                          duplicate_clicks_suppressed=True, drag_cancel=True, real_trace_reads=len(completed_reads),
                          provider_requests=len(requests), unicode_draft_and_cursor_preserved=True,
                          trace_bytes=recorded.stat().st_size, control_point=[x, y],
                          clean_quit=True, visible_windows_opened=False, paid_inference=False)
            output.write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result))
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=3)
            os.close(master)
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
            if not output.exists():
                clean = re.sub(rb"\x1b_G.*?\x1b\\", b"[graphics]", bytes(raw), flags=re.S)
                output.with_suffix(".diagnostic.txt").write_bytes(clean[-18000:])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="./dsco")
    parser.add_argument("--output", type=Path, default=Path("build/native-trace-controls.json"))
    args = parser.parse_args()
    run(Path(args.binary).resolve(strict=True), args.output.resolve())
