#!/usr/bin/env python3
"""Drive the production MCP gate and verify its execution-attempt spine."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib


INIT = {
    "jsonrpc": "2.0",
    "id": "init",
    "method": "initialize",
    "params": {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "execution-spine-test", "version": "1"},
    },
}


def request(req_id: str, tool: str, arguments: dict[str, object]) -> dict[str, object]:
    return {
        "jsonrpc": "2.0",
        "id": req_id,
        "method": "tools/call",
        "params": {"name": tool, "arguments": arguments},
    }


def read_journal(path: Path) -> list[dict[str, object]]:
    records: list[dict[str, object]] = []
    raw = path.read_bytes()
    offset = 0
    while offset < len(raw):
        assert len(raw) - offset >= 8, f"truncated WAL header in {path}"
        length, expected_crc = struct.unpack_from("<II", raw, offset)
        offset += 8
        assert length <= 2 * 1024 * 1024, f"unbounded WAL frame in {path}"
        frame = raw[offset : offset + length]
        assert len(frame) == length, f"truncated WAL frame in {path}"
        offset += length
        assert zlib.crc32(frame) & 0xFFFFFFFF == expected_crc, f"bad WAL CRC in {path}"
        records.append(json.loads(frame))
    return records


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    marker = "spine-private-marker-do-not-copy"

    with tempfile.TemporaryDirectory(prefix="dsco-execution-spine-") as tmp:
        root = Path(tmp)
        chronicle = root / "chronicle"
        runs = root / "runs"
        env = os.environ.copy()
        for key in list(env):
            if key.startswith("DSCO_ALLOW_") or key in {"DSCO_GOV_BYPASS", "DSCO_GOV_MODEL"}:
                env.pop(key, None)
        env.update(
            {
                "DSCO_CHRONICLE_DIR": str(chronicle),
                "DSCO_RUNS_DIR": str(runs),
                "DSCO_GOV_MODEL": "standard",
            }
        )
        messages = [
            INIT,
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            request("denied", "killswitch", {"action": "trigger", "reason": "spine-test"}),
            request("read-ok", "read_file", {"path": "Makefile"}),
            request("read-fail", "read_file", {"path": marker}),
            request("exec-ok", "bash", {"command": "printf spine-ok"}),
        ]
        stdin = "".join(json.dumps(message, separators=(",", ":")) + "\n" for message in messages)
        proc = subprocess.run(
            [binary, "mcp", "serve", "--toolsets", "all", "--tier", "trusted"],
            cwd=Path(__file__).resolve().parents[1],
            env=env,
            input=stdin,
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
        )
        assert proc.returncode == 0, proc.stderr
        responses = {}
        for line in proc.stdout.splitlines():
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "id" in value:
                responses[str(value["id"])] = value
        assert all(key in responses for key in ("init", "denied", "read-ok", "read-fail", "exec-ok"))
        assert "governance_block" in json.dumps(responses["denied"])
        assert '"isError": false' in json.dumps(responses["read-ok"])
        assert '"isError": true' in json.dumps(responses["read-fail"])
        assert '"isError": false' in json.dumps(responses["exec-ok"])

        execution_events: list[dict[str, object]] = []
        for event_file in chronicle.glob("events/*/*/*/session-*.jsonl"):
            for line in event_file.read_text().splitlines():
                event = json.loads(line)
                if str(event.get("event_type", "")).startswith("execution.attempt."):
                    execution_events.append(event)
        assert execution_events, "no execution attempt events"
        assert marker not in json.dumps(execution_events), "raw arguments leaked into execution receipts"

        groups: dict[str, list[dict[str, object]]] = {}
        for event in execution_events:
            payload = event["payload"]
            assert isinstance(payload, dict)
            execution_id = str(payload["execution_id"])
            groups.setdefault(execution_id, []).append(payload)
            assert payload["schema"] == "execution.attempt.v1"
            assert payload["input_sha256"]
            assert "provider" in payload and "model" in payload
            assert payload["executor"] == "dsco-native"

        expected = {
            "killswitch": ["proposed", "denied"],
            "read_file:ok": ["proposed", "admitted", "started", "succeeded"],
            "read_file:failed": ["proposed", "admitted", "started", "failed"],
            "bash": ["proposed", "admitted", "started", "succeeded"],
        }
        observed: dict[str, list[str]] = {}
        for payloads in groups.values():
            statuses = [str(payload["status"]) for payload in payloads]
            tool = str(payloads[0]["tool"])
            if tool == "read_file":
                key = "read_file:ok" if statuses[-1] == "succeeded" else "read_file:failed"
            else:
                key = tool
            observed[key] = statuses
            assert payloads[-1]["terminal"] is True
            assert payloads[-1]["result_sha256"]
        assert observed == expected, observed

        journal_records: list[dict[str, object]] = []
        for journal in runs.glob("*/journal.wal"):
            journal_records.extend(read_journal(journal))
        attempt_frames = [row for row in journal_records if row.get("type") == "execution.attempt.v1"]
        assert attempt_frames, "no execution attempt WAL frames"
        bash_frames = [
            row["payload"]
            for row in attempt_frames
            if isinstance(row.get("payload"), dict) and row["payload"].get("tool") == "bash"
        ]
        assert [row["status"] for row in bash_frames] == [
            "proposed",
            "admitted",
            "started",
            "succeeded",
        ]

        # Both an operator-disabled journal and a failed journal open must
        # deny an effectful leaf before its inert local marker is created.
        for mode in ("journal-off", "open-failed"):
            case = root / mode
            case.mkdir()
            marker_path = case / "effect-marker"
            case_runs = case / "runs"
            if mode == "open-failed":
                case_runs.write_text("regular file blocks journal directory")
            case_env = env.copy()
            case_env.pop("DSCO_EVENT_STREAM_DB", None)
            case_env.update(
                HOME=str(case),
                DSCO_CHRONICLE_DIR=str(case / "chronicle"),
                DSCO_RUNS_DIR=str(case_runs),
            )
            if mode == "journal-off":
                case_env["DSCO_JOURNAL"] = "off"
            else:
                case_env.pop("DSCO_JOURNAL", None)
            case_messages = [
                INIT,
                {"jsonrpc": "2.0", "method": "notifications/initialized"},
                request("effect", "bash", {"command": f"touch {marker_path}"}),
                request("inspect", "read_file", {"path": "Makefile"}),
            ]
            case_stdin = "".join(json.dumps(m, separators=(",", ":")) + "\n" for m in case_messages)
            case_proc = subprocess.run(
                [binary, "mcp", "serve", "--toolsets", "all", "--tier", "trusted"],
                cwd=Path(__file__).resolve().parents[1], env=case_env, input=case_stdin,
                text=True, capture_output=True, timeout=30, check=False,
            )
            assert case_proc.returncode == 0, (mode, case_proc.stderr)
            case_replies = {}
            for line in case_proc.stdout.splitlines():
                value = json.loads(line)
                if "id" in value:
                    case_replies[str(value["id"])] = value
            assert "durable_intent_failed_before_dispatch" in json.dumps(case_replies["effect"])
            assert not marker_path.exists(), mode
            assert '"isError": false' in json.dumps(case_replies["inspect"]), mode
            statuses = []
            for event_file in (case / "chronicle").glob("events/*/*/*/session-*.jsonl"):
                for line in event_file.read_text().splitlines():
                    event = json.loads(line)
                    payload = event.get("payload") or {}
                    if payload.get("tool") == "bash" and event.get("event_type", "").startswith("execution.attempt."):
                        statuses.append(payload["status"])
            assert statuses == ["proposed", "admitted", "failed"], (mode, statuses)

    print("PASS: live MCP lifecycles, durable effect frames, and missing-journal preflight")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
