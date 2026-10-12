#!/usr/bin/env python3
"""Persisted slim completion and partial-stream checks without HTTP or inference."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve(strict=True)
for case, expected in (("complete", "SLIM_VISIBLE"), ("partial", "SLIM_PARTIAL")):
    with tempfile.TemporaryDirectory(prefix="dsco-slim-chronicle-") as home:
        root = Path(home) / "chronicle"
        env = dict(os.environ, HOME=home, DSCO_CHRONICLE_DIR=str(root),
                   DSCO_CHRONICLE_MODE="full-local", DSCO_JOURNAL="off",
                   DSCO_EVENT_STREAM_DB="", DSCO_ENV_FILE="/dev/null")
        cp = subprocess.run([str(binary), case], env=env, cwd=home,
                            capture_output=True, text=True, timeout=15)
        assert cp.returncode == (0 if case == "complete" else 1), (case, cp.stderr)
        assert expected in cp.stdout and "HIDDEN_SLIM_THOUGHT" not in cp.stdout
        with sqlite3.connect(f"file:{root}/indexes/chronicle.sqlite?mode=ro", uri=True) as db:
            events = db.execute("SELECT event_type,trace_id,span_id,payload_json FROM events "
                                "WHERE event_type IN ('llm.request.created','llm.response.completed') "
                                "ORDER BY seq").fetchall()
            assert [row[0] for row in events] == ["llm.request.created", "llm.response.completed"], events
            assert events[0][1:3] == events[1][1:3]
            payload = json.loads(events[1][3])
            assert payload["response_status"] == ("completed" if case == "complete" else "partial_failed"), payload
            assert payload["output_kind"] == "text" and payload["output_byte_len"] == len(expected)
            assert payload["cost_usd"] == (0.125 if case == "complete" else None)
            sha = payload["output_blob_sha256"]
            blob = root / "blobs/sha256" / sha[:2] / sha[2:4] / f"{sha}.blob"
            assert blob.read_text() == expected
            span = db.execute("SELECT status,ended_at FROM spans WHERE span_id=?", (events[0][2],)).fetchone()
            assert span and span[0] == ("ok" if case == "complete" else "error") and span[1]
print("PASS: slim synthetic complete and partial responses persisted with exact visible blobs")
