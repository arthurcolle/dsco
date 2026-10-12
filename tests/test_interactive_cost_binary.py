#!/usr/bin/env python3
"""Real --tui/--native cost regression, using owned PTYs and loopback SSE only.

A successful unpriced answer must survive in conversation history. Repeated
cumulative usage must retain one cost receipt with disjoint token categories.
No external model requests, real credentials, or visible terminal windows.
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


def run(binary, mode):
    requests, errors = [], []
    with tempfile.TemporaryDirectory(prefix="dsco-interactive-cost-") as tmp:
        ledger = Path(tmp, "costs.jsonl")

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                try:
                    assert self.path == "/v1/chat/completions", self.path
                    req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                    requests.append(req)
                    stage = len(requests)
                    assert stage <= 2, "unexpected request after complete answer"
                    marker = "COST_UNKNOWN_ANSWER_RETAINED" if stage == 1 else "COST_FINAL_ANSWER"
                    usage = dict(prompt_tokens=1000, completion_tokens=100)
                    if stage == 2:
                        usage.update(cost=.375,
                            input_tokens_details=dict(cached_tokens=400,
                                orchestration_input_tokens=500,
                                orchestration_input_cached_tokens=200),
                            output_tokens_details=dict(orchestration_output_tokens=50))
                    event = dict(id=f"cost-fixture-{stage}", model="cost-fixture-unpriced",
                        choices=[dict(index=0, delta=dict(content=marker), finish_reason="stop")],
                        usage=usage)
                    events = [event]
                    if stage == 2:
                        # Providers may repeat cumulative data at both nesting
                        # levels or send a partial final usage-only chunk.
                        partial = {k: v for k, v in usage.items()
                                   if k not in ("prompt_tokens", "completion_tokens")}
                        events.append(dict(choices=[dict(usage=partial)], usage=partial))
                        events.append(dict(choices=[], usage=dict(completion_tokens=120)))
                    data = ("".join("data: " + json.dumps(e) + "\n\n" for e in events)
                            + "data: [DONE]\n\n").encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                except Exception as exc:
                    errors.append(repr(exc))
                    self.close_connection = True

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        env = dict(HOME=tmp, TMPDIR=tmp, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
                   TERM="xterm-kitty" if mode == "native" else "xterm-256color",
                   LANG="en_US.UTF-8", OPENAI_API_KEY="fixture-only",
                   OPENAI_API_BASE=f"http://127.0.0.1:{server.server_port}/v1",
                   DSCO_ENV_FILE="/dev/null", DSCO_PRICING_OFFLINE="1",
                   DSCO_SECURE_STORE_NO_PROMPT="1", DSCO_CHRONICLE_MODE="off",
                   DSCO_COST_LEDGER_PATH=str(ledger), DSCO_DISABLE_DEFAULT_FALLBACKS="1",
                   DSCO_AUTO_FALLBACK="0", DSCO_DYNAMIC_FAILOVER="0",
                   DSCO_DISABLE_PROVIDER_FABRIC_AUTO="1", DSCO_NO_AUTO_SUPERVISE="1",
                   DSCO_NO_SUPERVISE="1", DSCO_KITTY_AGENT_WINDOWS="0",
                   DSCO_PIXEL_TUI_DPR="1", DSCO_PIXEL_TUI_ANIMATIONS="0",
                   DSCO_TUI_ANIMATIONS="0", DSCO_BANNER="0", DSCO_KITTY_BANNER="0",
                   DSCO_SYSTEM_PROMPT="Owned local fixture. Answer the user directly.",
                   DSCO_HARD_TURN_CEILING="3")
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 1200, 720))
        proc = None
        transcript = bytearray()
        text_filter = TerminalText()
        sent = 0
        due = None
        receipts = []
        try:
            proc = subprocess.Popen([str(binary), "--profile", "worker", "--" + mode,
                                     "-i", "--provider", "openai", "-m", "cost-fixture-unpriced"],
                                    cwd=tmp, env=env, stdin=slave, stdout=slave, stderr=slave,
                                    start_new_session=True)
            os.close(slave)
            slave = -1
            started = time.monotonic()
            due = started + 1.5
            observed_receipts = 0
            while time.monotonic() - started < 25:
                if select.select([master], [], [], .02)[0]:
                    try:
                        data = os.read(master, 1024 * 1024)
                    except OSError:
                        data = b""
                    transcript.extend(text_filter.feed(data))
                    assert len(transcript) < 4 * 1024 * 1024, "PTY text output limit"
                if ledger.exists():
                    try:
                        receipts = [json.loads(line) for line in ledger.read_text().splitlines()]
                    except json.JSONDecodeError:
                        pass  # append may still be in progress
                if len(receipts) != observed_receipts:
                    observed_receipts = len(receipts)
                    due = time.monotonic() + .8
                if due and time.monotonic() >= due:
                    if sent == 0:
                        os.write(master, b"What is a mutex?\r")
                        sent = 1
                    elif sent == 1 and len(receipts) == 1:
                        os.write(master, b"Explain that in one more sentence.\r")
                        sent = 2
                    elif sent == 2 and len(receipts) == 2:
                        os.write(master, b"/quit\r")
                        sent = 3
                    due = None
                if proc.poll() is not None:
                    break
            assert proc.poll() == 0 and sent == 3, (mode, proc.poll(), len(requests),
                                                       bytes(transcript[-3000:]))
            assert not errors and len(requests) == 2, (errors, len(requests))
            retained = any(m.get("role") == "assistant" and
                "COST_UNKNOWN_ANSWER_RETAINED" in json.dumps(m)
                for m in requests[1].get("messages", []))
            assert retained, "successful unpriced answer vanished from the next request"
            assert len(receipts) == 2 and all(row["success"] for row in receipts), receipts
            first, second = receipts
            assert first["budget_accounted_usd"] is None, first
            assert first["provider_reported_usd"] is None and first["estimated_inference_usd"] is None
            assert second["input_tokens"] == 900 and second["output_tokens"] == 170, second
            assert second["cache_read_tokens"] == 600, second
            assert second["provider_reported_usd"] == .375 and second["budget_accounted_usd"] == .375
            if mode == "tui":
                assert b"cost:unknown" in transcript or b"unpriced=1" in transcript, (
                    "unknown cost not visible", bytes(transcript[-3000:]))
                assert b"provider_reported" in transcript or b"reported=$0.375000" in transcript, (
                    "reported basis not visible", bytes(transcript[-3000:]))
            return dict(mode=mode, requests=len(requests), retained_unpriced_answer=retained,
                        receipts=receipts, elapsed_seconds=round(time.monotonic() - started, 3))
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
    parser.add_argument("--mode", choices=("tui", "native", "both"), default="both")
    parser.add_argument("--output", type=Path, help="Write metadata-only JSON evidence")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    modes = ("tui", "native") if args.mode == "both" else (args.mode,)
    rows = [run(binary, mode) for mode in modes]
    assert hashlib.sha256(binary.read_bytes()).hexdigest() == digest, "binary changed during test"
    result = json.dumps(dict(binary=str(binary), sha256=digest, passed=True, cases=rows), indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(result + "\n")
    print(result)


if __name__ == "__main__":
    main()
