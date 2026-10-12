#!/usr/bin/env python3
"""Persisted worker and single-tool Chronicle paths with no network use."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve(strict=True)
with tempfile.TemporaryDirectory(prefix="dsco-orchestrator-chronicle-") as home:
    root = Path(home) / "chronicle"
    env = dict(os.environ, HOME=home, DSCO_CHRONICLE_DIR=str(root),
               DSCO_CHRONICLE_MODE="full-local", DSCO_JOURNAL="off",
               DSCO_EVENT_STREAM_DB="", DSCO_ENV_FILE="/dev/null")
    cp = subprocess.run([str(binary)], env=env, cwd=home,
                        capture_output=True, text=True, timeout=15)
    assert cp.returncode == 0 and "PASS:" in cp.stdout, (cp.returncode, cp.stderr)
    with sqlite3.connect(f"file:{root}/indexes/chronicle.sqlite?mode=ro", uri=True) as db:
        rows = db.execute("SELECT seq,event_type,trace_id,span_id,parent_span_id,payload_json "
                          "FROM events ORDER BY seq").fetchall()
        requests = [r for r in rows if r[1] == "llm.request.created"]
        responses = [r for r in rows if r[1] == "llm.response.completed"]
        starts = [r for r in rows if r[1] == "tool.call.created"]
        ends = [r for r in rows if r[1] == "tool.call.completed"]
        assert len(requests) == len(responses) == 4 and len(starts) == len(ends) == 2
        expected = ["WORKER_BEFORE", "WORKER_FINAL", "SINGLE_BEFORE", "SINGLE_FINAL"]
        for index, (request, response, answer) in enumerate(zip(requests, responses, expected)):
            assert request[2:4] == response[2:4] and request[0] < response[0]
            payload = json.loads(response[5])
            assert payload["response_status"] == "completed"
            assert payload["output_kind"] == ("mixed" if index % 2 == 0 else "text")
            assert payload["cost_usd"] == (None if index % 2 == 0 else 0.2)
            sha = payload["output_blob_sha256"]
            blob = root / "blobs/sha256" / sha[:2] / sha[2:4] / f"{sha}.blob"
            assert blob.read_text() == answer and "HIDDEN_ORCHESTRATOR_THOUGHT" not in answer
            span = db.execute("SELECT status,ended_at FROM spans WHERE span_id=?", (request[3],)).fetchone()
            assert span and span[0] == "ok" and span[1]
        for index, (start, end) in enumerate(zip(starts, ends)):
            request = requests[index * 2]
            assert start[4] == request[3] and start[2] == request[2]
            assert start[3] == end[3] and start[0] < end[0]
            result = json.loads(end[5])
            assert result["ok"] and result["result_byte_len"] == len("SYNTHETIC_TOOL_RESULT")
            closed = [r for r in rows if r[1] == "span.completed" and r[3] == request[3]]
            assert len(closed) == 1 and closed[0][0] > end[0]
print("PASS: four paired orchestrator attempts, two tool calls and exact visible blobs")
