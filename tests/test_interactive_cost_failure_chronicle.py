#!/usr/bin/env python3
"""A failed worker-cost descriptor must not rewrite the provider's Chronicle outcome."""
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


binary = Path(sys.argv[1]).resolve(strict=True)
with tempfile.TemporaryDirectory(prefix="dsco-interactive-cost-capture-") as home:
    root = Path(home)
    (root / ".dsco").mkdir()
    (root / ".dsco/codex_models.json").write_text(json.dumps({"models": [
        {"slug": "gpt-6-astra", "supported_in_api": True, "context_window": 272000, "priority": 1}]}))
    requests = []
    errors = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                requests.append(body)
                assert len(requests) == 1 and "COST_FAILURE_OBJECTIVE" in json.dumps(body)
                payload = (event({"type": "response.created", "response": {"id": "cost-fixture"}})
                           + event({"type": "response.output_text.delta", "delta": "INTERACTIVE_COST_VISIBLE"})
                           + event({"type": "response.completed", "response": {
                               "status": "completed", "usage": {"input_tokens": 9, "output_tokens": 3}}})).encode()
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
    env = dict(HOME=home, TMPDIR=home, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
               DSCO_ENV_FILE="/dev/null", DSCO_CHATGPT_OAUTH_TOKEN="fixture",
               DSCO_CHATGPT_ACCOUNT_ID="fixture",
               DSCO_CHATGPT_BASE_URL=f"http://127.0.0.1:{server.server_port}/responses",
               DSCO_DISABLE_SHARED_HOME_OAUTH="1", DSCO_PRICING_OFFLINE="1",
               DSCO_SECURE_STORE_NO_PROMPT="1", DSCO_NO_AUTO_SUPERVISE="1",
               DSCO_NO_SUPERVISE="1", DSCO_DURABLE_AUTOWAKE="0",
               DSCO_DISABLE_PROVIDER_FABRIC_AUTO="1", DSCO_AUTO_GOAL="0",
               DSCO_HARD_TURN_CEILING="2", DSCO_BUDGET="0", DSCO_TOOL_PROXY="1",
               DSCO_AUTO_FALLBACK="0", DSCO_DYNAMIC_FAILOVER="0",
               DSCO_WORKER_COST_FD="999999", DSCO_CHRONICLE_MODE="full-local",
               DSCO_CHRONICLE_DIR=str(root / "chronicle"),
               DSCO_SYSTEM_PROMPT="Owned fixture. Respond briefly without tools.")
    try:
        cp = subprocess.run([str(binary), "--tui", "-i", "--profile", "worker",
                             "--provider", "openai-codex", "-m", "gpt-6-astra",
                             "--trust-tier", "trusted", "--approval-mode", "never",
                             "-p", "COST_FAILURE_OBJECTIVE"], env=env, cwd=home,
                            input="/quit\n", capture_output=True, text=True, timeout=45)
        assert not errors and len(requests) == 1, (errors, len(requests), cp.stderr[-800:])
        assert "could not deliver native worker cost record" in cp.stderr
        with sqlite3.connect(f"file:{root}/chronicle/indexes/chronicle.sqlite?mode=ro", uri=True) as db:
            requests_db = db.execute("SELECT trace_id,span_id FROM events "
                                     "WHERE event_type='llm.request.created' ORDER BY seq").fetchall()
            responses = db.execute("SELECT trace_id,span_id,payload_json FROM events "
                                   "WHERE event_type='llm.response.completed' ORDER BY seq").fetchall()
            assert len(requests_db) == len(responses) == 1
            assert requests_db[0] == responses[0][:2]
            payload = json.loads(responses[0][2])
            assert payload["response_status"] == "completed" and payload["output_kind"] == "text", payload
            assert payload["output_byte_len"] == len("INTERACTIVE_COST_VISIBLE")
            sha = payload["output_blob_sha256"]
            blob = root / "chronicle/blobs/sha256" / sha[:2] / sha[2:4] / f"{sha}.blob"
            assert blob.read_text() == "INTERACTIVE_COST_VISIBLE"
            span = db.execute("SELECT status,payload_json FROM spans WHERE span_id=?",
                              (responses[0][1],)).fetchone()
            assert span and span[0] == "error" and json.loads(span[1])["accounting_failed"] is True
        print("PASS: interactive cost delivery failure preserved completed provider text and runtime error span")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
