#!/usr/bin/env python3
"""Standalone voice text capture and provider status, with no WebSocket or audio logging."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve(strict=True)
with tempfile.TemporaryDirectory(prefix="dsco-realtime-chronicle-") as home:
    root = Path(home) / "chronicle"
    env = dict(os.environ, HOME=home, DSCO_CHRONICLE_DIR=str(root),
               DSCO_CHRONICLE_MODE="full-local", DSCO_JOURNAL="off",
               DSCO_EVENT_STREAM_DB="", DSCO_ENV_FILE="/dev/null")
    cp = subprocess.run([str(binary)], env=env, cwd=home,
                        capture_output=True, text=True, timeout=15)
    assert cp.returncode == 0 and "PASS:" in cp.stdout, (cp.returncode, cp.stderr)
    with sqlite3.connect(f"file:{root}/indexes/chronicle.sqlite?mode=ro", uri=True) as db:
        rows = db.execute("SELECT span_id,payload_json FROM events "
                          "WHERE event_type='llm.response.completed' ORDER BY seq").fetchall()
        assert len(rows) == 6, rows
        expected = (("completed", "STANDALONE_OK", "text"),
                    ("partial_failed", "STANDALONE_FAILED", "text"),
                    ("partial_failed", "STANDALONE_INCOMPLETE", "text"),
                    ("truncated", "STANDALONE_TRUNCATED_PREFIX", "text"),
                    ("completed", "", "tool_only"),
                    ("completed", "MIXED_VOICE_TEXT", "mixed"))
        for (span_id, encoded), (status, answer, kind) in zip(rows, expected):
            payload = json.loads(encoded)
            assert payload["provider"] == "openai-realtime" and payload["model"] == "gpt-realtime-2"
            assert payload["response_status"] == status and payload["cost_usd"] is None
            assert payload["output_kind"] == kind and payload["output_byte_len"] == len(answer)
            sha = payload["output_blob_sha256"]
            blob = root / "blobs/sha256" / sha[:2] / sha[2:4] / f"{sha}.blob"
            assert blob.read_text() == answer and "SECRET_AUDIO" not in blob.read_text()
            span = db.execute("SELECT status,ended_at FROM spans WHERE span_id=?", (span_id,)).fetchone()
            assert span and span[0] == ("ok" if status == "completed" else "error") and span[1]
            if status == "truncated":
                metadata = db.execute("SELECT payload_json FROM spans WHERE span_id=?", (span_id,)).fetchone()[0]
                assert json.loads(metadata)["capture_truncated"] is True
        started = db.execute("SELECT span_id,parent_span_id,payload_json FROM events "
                             "WHERE event_type='tool.call.created' ORDER BY seq").fetchall()
        ended = db.execute("SELECT span_id,payload_json FROM events "
                           "WHERE event_type='tool.call.completed' ORDER BY seq").fetchall()
        assert len(started) == len(ended) == 2
        assert [row[1] for row in started] == [rows[4][0], rows[5][0]]
        assert [row[0] for row in started] == [row[0] for row in ended]
        for (span_id, encoded), expected_ok, expected_result in zip(ended,
                (True, False), ("SYNTHETIC_TOOL_OK", "ERROR: SYNTHETIC_TOOL_FAILED")):
            payload = json.loads(encoded)
            assert payload["ok"] is expected_ok and payload["result_byte_len"] == len(expected_result)
            sha = payload["result_blob_sha256"]
            blob = root / "blobs/sha256" / sha[:2] / sha[2:4] / f"{sha}.blob"
            assert blob.read_text() == expected_result
            tool_span = db.execute("SELECT status,ended_at FROM spans WHERE span_id=?", (span_id,)).fetchone()
            assert tool_span and tool_span[0] == ("ok" if expected_ok else "error") and tool_span[1]
print("PASS: standalone voice persisted six responses and paired success/failure tool events")
