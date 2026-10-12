#!/usr/bin/env python3
"""Offline one-shot Chronicle request/response/tool persistence regression."""
import http.server
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading


def event(value):
    return "data: " + json.dumps(value) + "\n\n"


binary = Path(sys.argv[1] if len(sys.argv) > 1 else "./dsco").resolve()
with tempfile.TemporaryDirectory(prefix="dsco-chronicle-headless-") as tmp:
    root = Path(tmp)
    output = root / "fixture-output.txt"
    requests = []
    errors = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                requests.append(request)
                stage = len(requests)
                if stage == 1:
                    item = {"type": "function_call", "name": "write_file",
                            "call_id": "fixture-tool", "arguments": json.dumps({
                                "path": str(output), "content": "FIXTURE_ONLY"})}
                    body = event({"type": "response.output_item.done", "item": item})
                elif stage == 2:
                    assert output.read_text() == "FIXTURE_ONLY"
                    body = event({"type": "response.output_item.done", "item": {
                        "type": "message", "content": [
                            {"type": "output_text", "text": "FINAL_ONLY_FIXTURE"}]}})
                else:
                    raise AssertionError(f"unexpected request {stage}")
                body += event({"type": "response.completed", "response": {
                    "status": "completed", "usage": {"input_tokens": 7, "output_tokens": 5}}})
                payload = body.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            except Exception as exc:
                errors.append(str(exc))
                self.close_connection = True

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = dict(HOME=tmp, TMPDIR=tmp, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
               DSCO_ENV_FILE="/dev/null", DSCO_CHATGPT_OAUTH_TOKEN="fixture",
               DSCO_CHATGPT_ACCOUNT_ID="fixture", DSCO_CHATGPT_BASE_URL=
               f"http://127.0.0.1:{server.server_port}/responses",
               DSCO_DISABLE_SHARED_HOME_OAUTH="1", DSCO_PRICING_OFFLINE="1",
               DSCO_SECURE_STORE_NO_PROMPT="1", DSCO_NO_AUTO_SUPERVISE="1",
               DSCO_NO_SUPERVISE="1", DSCO_DURABLE_AUTOWAKE="0",
               DSCO_DISABLE_PROVIDER_FABRIC_AUTO="1", DSCO_AUTO_GOAL="0",
               DSCO_HARD_TURN_CEILING="2", DSCO_BUDGET="0",
               DSCO_TOOL_PROXY="1", DSCO_TOOL_ALLOWLIST="write_file",
               DSCO_AUTO_FALLBACK="0", DSCO_DYNAMIC_FAILOVER="0",
               DSCO_CHRONICLE_MODE="full-local", DSCO_CHRONICLE_DIR=str(root / "chronicle"),
               DSCO_SYSTEM_PROMPT="Owned fixture. Execute the tool then finish.")
    try:
        cp = subprocess.run([str(binary), "--profile", "worker", "--provider", "openai-codex",
                             "-m", "gpt-6-astra", "--trust-tier", "trusted",
                             "--approval-mode", "never", "-p", "FIXTURE_OBJECTIVE"],
                            env=env, cwd=tmp, capture_output=True, text=True, timeout=45)
        assert not errors, errors
        assert cp.returncode == 0, (cp.returncode, cp.stderr[-1200:])
        assert len(requests) == 2 and output.read_text() == "FIXTURE_ONLY"
        assert "FINAL_ONLY_FIXTURE" in cp.stdout + cp.stderr, (cp.stdout[-300:], cp.stderr[-900:])
        db = sqlite3.connect(root / "chronicle" / "indexes" / "chronicle.sqlite")
        rows = db.execute("""SELECT e.event_type, e.trace_id, e.span_id, e.parent_span_id,
                          e.payload_json FROM events e WHERE e.event_type IN
                          ('llm.request.created','llm.response.completed',
                           'tool.call.created','tool.call.completed') ORDER BY e.seq""").fetchall()
        by_type = {name: [row for row in rows if row[0] == name] for name in
                   ("llm.request.created", "llm.response.completed",
                    "tool.call.created", "tool.call.completed")}
        assert [len(by_type[name]) for name in by_type] == [2, 2, 1, 1]
        responses = [json.loads(row[4]) for row in by_type["llm.response.completed"]]
        assert [row["output_kind"] for row in responses] == ["tool_only", "text"]
        assert [row["response_status"] for row in responses] == ["completed", "completed"]
        assert all(row["cost_known"] and row["cost_usd"] >= 0 for row in responses)
        assert all(by_type["llm.request.created"][i][1:3] ==
                   by_type["llm.response.completed"][i][1:3] for i in range(2))
        assert by_type["tool.call.created"][0][1] == by_type["llm.response.completed"][0][1]
        assert by_type["tool.call.created"][0][3] == by_type["llm.response.completed"][0][2]
        sha = responses[1]["output_blob_sha256"]
        blob = db.execute("SELECT local_path,byte_len FROM blobs WHERE sha256=?", (sha,)).fetchone()
        assert blob and blob[1] == len("FINAL_ONLY_FIXTURE")
        assert (root / "chronicle" / blob[0]).read_text() == "FINAL_ONLY_FIXTURE"
        db.close()
        print("PASS: headless local fixture persisted two paired attempts, tool events, final-only visible text, and observed costs")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
