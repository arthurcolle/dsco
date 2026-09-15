#!/usr/bin/env python3
"""Real native event capture of an isolated streaming HTTP retry, without inference."""
import argparse
import base64
import http.server
import json
import os
from pathlib import Path
import shlex
import socket
import sqlite3
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dsco", type=Path, default=ROOT / "dsco")
    parser.add_argument("--fixture", type=Path, default=ROOT / "build/stream_completion_fixture")
    parser.add_argument("--output", type=Path, default=ROOT / "build/provider-event-capture.json")
    args = parser.parse_args()
    first = b"not-SSE\x00opaque-error\r\n" + b"x" * 4096
    def event(value):
        return b"data: " + json.dumps(value).encode() + b"\r\n\r\n"
    second = b":comment retained in body\r\n"
    second += event({"type": "fixture.unknown", "payload": "keep-this-event"})
    second += event({"type": "message_start", "message": {"usage": {"input_tokens": 7}}})
    second += event({"type": "content_block_start", "index": 0,
                     "content_block": {"type": "text", "text": ""}})
    second += event({"type": "content_block_delta", "index": 0,
                     "delta": {"type": "text_delta", "text": "CAPTURED"}})
    second += event({"type": "content_block_stop", "index": 0})
    second += event({"type": "message_delta", "delta": {"stop_reason": "end_turn"},
                     "usage": {"output_tokens": 3}})
    second += event({"type": "message_stop"}) + b"data: [DONE]\r\n\r\n"
    received = []
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", "0")))
            received.append(self.path)
            body = first if len(received) == 1 else second
            self.send_response(503 if len(received) == 1 else 200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body); self.wfile.flush()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    report = {}
    try:
        with tempfile.TemporaryDirectory(prefix="dsco-provider-event-proof-") as tmp:
            folder = Path(tmp)
            env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": tmp,
                   "TMPDIR": tmp, "LANG": "en_US.UTF-8", "DSCO_ENV_FILE": "/dev/null",
                   "DSCO_ALLOW_RUN": "1", "DSCO_ALLOW_WRITE": "1", "DSCO_ALLOW_NET": "1",
                   "DSCO_LLM_MAX_RETRIES": "1", "DSCO_LLM_RETRY_DELAY_MS": "100",
                   "DSCO_PRICING_OFFLINE": "1", "DSCO_DISABLE_SHARED_HOME_OAUTH": "1",
                   "DSCO_TOOLMGMT": "0", "DSCO_NO_COLOR": "1"}
            url = f"http://127.0.0.1:{server.server_port}/retry?token=fixture-query-must-not-be-endpoint"
            command = shlex.join([str(args.fixture.resolve()), "anthropic", url])
            source = 'local l=require("lingo");local r=l.call("bash",args);assert(r.ok,r.result);return r'
            left, right = socket.socketpair()
            right.setblocking(False)
            wire = bytearray()
            def collect():
                while True:
                    data = left.recv(65536)
                    if not data: return
                    wire.extend(data)
            reader = threading.Thread(target=collect, daemon=True); reader.start()
            dbpath = folder / "events.sqlite"
            process = subprocess.Popen([str(args.dsco.resolve()), "lingo", "eval", source,
                json.dumps({"command": command, "timeout": 20}), "--events-fd", str(right.fileno()),
                "--events-db", str(dbpath)], pass_fds=[right.fileno()], cwd=ROOT, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            right.close()
            try:
                stdout, stderr = process.communicate(timeout=30)
            except BaseException:
                process.kill(); process.communicate(); raise
            reader.join(5); left.close()
            assert not reader.is_alive(), "capture socket did not close"
            assert process.returncode == 0, (stdout, stderr)
            with sqlite3.connect(f"file:{dbpath}?mode=ro", uri=True) as db:
                stored = [json.loads(row[0]) for row in db.execute("SELECT record FROM events ORDER BY seq")]
            frames = [json.loads(line) for line in wire.splitlines()]
            delivered = [json.loads(frame["record"]) if isinstance(frame.get("record"), str)
                         else frame.get("record", frame) for frame in frames]
            report = {"passed": False, "delivered_events": len(delivered), "stored_events": len(stored),
                      "delivered_tail": [{"seq": row.get("sequence", row.get("seq")), "event": row.get("event")}
                                         for row in delivered[-4:]],
                      "stored_tail": [{"seq": row.get("sequence", row.get("seq")), "event": row.get("event")}
                                      for row in stored[-4:]]}
            assert delivered == stored, ("delivered stream differed from durable outbox", report)
            provider = [row for row in stored if row["source"] == "provider"]
            starts = [row["payload"] for row in provider if row["event"] == "provider.attempt.started"]
            finishes = [row["payload"] for row in provider if row["event"] == "provider.attempt.finished"]
            retries = [row["payload"] for row in provider if row["event"] == "provider.retry.scheduled"]
            assert len(received) == len(starts) == len(finishes) == 2, (received, starts, finishes)
            assert len(retries) == 1 and retries[0]["after_attempt_id"] == starts[0]["attempt_id"]
            assert retries[0]["delay_ms"] == 100
            assert [row["http_status"] for row in finishes] == [503, 200]
            assert [row["terminal_received"] for row in finishes] == [False, True]
            for start, expected in zip(starts, [first, second]):
                assert "?" not in start["endpoint"] and "fixture-query" not in start["endpoint"]
                parts = [row["payload"] for row in provider if row["event"] == "provider.response.body"
                         and row["payload"]["attempt_id"] == start["attempt_id"]]
                reconstructed = bytearray()
                for part in parts:
                    assert part["offset"] == len(reconstructed)
                    decoded = base64.b64decode(part["data"], validate=True)
                    assert len(decoded) == part["byte_length"]
                    reconstructed.extend(decoded)
                assert reconstructed == expected, "raw response bytes changed or were dropped"
            assert any(row["event"] == "provider.anthropic.event" and
                       row["payload"].get("type") == "fixture.unknown" for row in provider)
            report = {"passed": True, "checks": 7, "http_requests": len(received),
                      "events": len(stored), "provider_events": len(provider),
                      "byte_exact_attempt_bodies": [len(first), len(second)],
                      "attempts": finishes, "retries": retries,
                      "provider_inference_calls": 0, "delivery_equals_durable_outbox": True}
    finally:
        server.shutdown(); server.server_close()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
