#!/usr/bin/env python3
"""Recovery projection: hostile WALs and an actual killed MCP tool process."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import struct
import subprocess
import tempfile
import time
import uuid
import zlib


def frame(record):
    body = json.dumps(record, separators=(",", ":")).encode()
    return struct.pack("<II", len(body), zlib.crc32(body)) + body


def attempt(run, ident, status, effectful=True):
    started = status in {"started", "succeeded", "failed", "cancelled", "effect_unknown"}
    return {"schema": "execution.attempt.v1", "execution_id": ident, "session_id": run,
            "tool": "bash" if effectful else "read_file", "input_sha256": "a" * 64,
            "status": status, "capability_mask": 8 if effectful else 1,
            "effectful": effectful, "admitted": status not in {"proposed", "denied"},
            "started": started, "terminal": status in {"succeeded", "failed", "denied", "cancelled", "effect_unknown"}}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--inspector", help="Optional sanitizer build of the production recovery reader")
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    inspector = str(Path(args.inspector).resolve()) if args.inspector else binary
    with tempfile.TemporaryDirectory(prefix="dsco-recovery-test-") as tmp:
        root = Path(tmp)
        runs = root / "runs"
        env = dict(os.environ, DSCO_RUNS_DIR=str(runs), DSCO_CHRONICLE_DIR=str(root / "chronicle"),
                   DSCO_CHRONICLE_MODE="metadata", DSCO_JOURNAL="1", DSCO_GOV_MODEL="standard")
        for key in list(env):
            if key.startswith("DSCO_ALLOW_") or key == "DSCO_GOV_BYPASS": env.pop(key)
        checks = 0

        def inspect(run, expected):
            nonlocal checks
            proc = subprocess.run([inspector, "runs", "inspect", run], env=env, text=True,
                                  capture_output=True, timeout=10)
            assert proc.returncode == expected, (proc.returncode, proc.stdout, proc.stderr)
            value = json.loads(proc.stdout)
            assert value["read_only"] and not value["retry_authorized"]
            checks += 1
            return value

        def fixture(statuses, effectful=True, suffix=b"", transform=None):
            run, ident = str(uuid.uuid4()), str(uuid.uuid4())
            records = [{"v": 1, "run_id": run, "seq": i + 1, "type": "execution.attempt.v1",
                        "payload": attempt(run, ident, s, effectful)} for i, s in enumerate(statuses)]
            if transform: transform(records)
            path = runs / run / "journal.wal"
            path.parent.mkdir(parents=True)
            path.write_bytes(b"".join(frame(r) for r in records) + suffix)
            return run, path

        for terminal in ("succeeded", "failed", "cancelled", "effect_unknown"):
            run, _ = fixture(["proposed", "admitted", "started", terminal])
            value = inspect(run, 0 if terminal == "succeeded" else 3)
            assert value["effect_unknown_count"] == (terminal != "succeeded")
        for statuses in (["proposed"], ["proposed", "admitted"], ["proposed", "admitted", "started"]):
            for effectful in (True, False):
                run, _ = fixture(statuses, effectful)
                value = inspect(run, 3)
                assert value["effect_unknown_count"] == (effectful and statuses[-1] == "started")
        for terminal in ("denied",):
            run, _ = fixture(["proposed", terminal])
            assert inspect(run, 0)["unresolved_count"] == 0
        for suffix, error in ((b"abc", "truncated_header"), (struct.pack("<II", 30, 0) + b"x", "truncated_frame"),
                              (struct.pack("<II", 2, 0) + b"{}", "crc_mismatch"),
                              (struct.pack("<II", 0xffffffff, 0), "frame_size_limit")):
            run, path = fixture(["proposed", "admitted", "started"], suffix=suffix)
            before = path.read_bytes()
            value = inspect(run, 1)
            assert value["error"] == error and not value["journal_complete"]
            assert value["effect_unknown_count"] == 1 and path.read_bytes() == before
        bad_cases = [
            (lambda rs: rs[-1].update(seq=1), "invalid_record_sequence"),
            (lambda rs: rs[-1].update(run_id=str(uuid.uuid4())), "record_identity_mismatch"),
            (lambda rs: rs[-1]["payload"].update(effectful=False), "invalid_effect_class"),
            (lambda rs: rs[-1]["payload"].update(status="invented"), "unknown_attempt_status"),
            (lambda rs: rs[-1]["payload"].update(terminal=True), "invalid_attempt_state"),
            (lambda rs: rs[-1]["payload"].update(input_sha256="b" * 64), "invalid_attempt_transition"),
        ]
        for transform, error in bad_cases:
            run, _ = fixture(["proposed", "admitted", "started"], transform=transform)
            assert inspect(run, 1)["error"] == error
        for statuses in (["started"], ["proposed", "succeeded"], ["proposed", "admitted", "succeeded"],
                         ["proposed", "admitted", "started", "succeeded", "failed"]):
            run, _ = fixture(statuses)
            assert inspect(run, 1)["error"] in {"missing_attempt_prefix", "invalid_attempt_transition"}
        run, _ = fixture(["proposed", "admitted", "failed"],
                         transform=lambda rs: rs[-1]["payload"].update(started=False))
        assert inspect(run, 0)["unresolved_count"] == 0  # rejected before leaf start
        run, path = fixture(["proposed"])
        # A CRC-valid record can still have an ambiguous JSON contract.
        raw = path.read_bytes()[8:]
        raw = raw[:-1] + b',"seq":999}'
        path.write_bytes(struct.pack("<II", len(raw), zlib.crc32(raw)) + raw)
        assert inspect(run, 1)["error"] == "invalid_record"
        run, path = fixture([])
        assert inspect(run, 1)["error"] == "empty_journal"
        path.unlink()
        path.symlink_to(root / "nonexistent")
        assert inspect(run, 1)["error"] == "journal_unavailable"
        proc = subprocess.run([inspector, "runs", "inspect", "../escape"], env=env, capture_output=True, timeout=10)
        assert proc.returncode == 2
        checks += 1

        # Execute a real side effect, kill the owning harness before the tool
        # completes, and verify that the retained start cannot be read as success.
        live_runs = root / "live-runs"
        live_env = dict(env, DSCO_RUNS_DIR=str(live_runs))
        marker, child_pid = root / "committed", root / "child.pid"
        command = f"printf '%s' $$ > {shlex.quote(str(child_pid))}; printf committed > {shlex.quote(str(marker))}; sleep 3"
        proc = subprocess.Popen([binary, "mcp", "serve", "--toolsets", "core", "--tier", "trusted"],
                                env=live_env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            messages = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                    "protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "recovery", "version": "1"}}},
                {"jsonrpc": "2.0", "method": "notifications/initialized"},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "bash", "arguments": {"command": command}}},
            ]
            proc.stdin.write("".join(json.dumps(m) + "\n" for m in messages))
            proc.stdin.flush()
            deadline = time.monotonic() + 10
            while not marker.exists() and proc.poll() is None and time.monotonic() < deadline: time.sleep(0.01)
            assert marker.exists(), "real MCP side effect did not run"
            os.kill(proc.pid, signal.SIGKILL)
            proc.wait(timeout=5)
            journals = list(live_runs.glob("*/journal.wal"))
            assert len(journals) == 1
            journal = journals[0]
            digest = hashlib.sha256(journal.read_bytes()).hexdigest()
            checked = subprocess.run([inspector, "runs", "inspect", journal.parent.name], env=live_env,
                                     capture_output=True, text=True, timeout=10)
            assert checked.returncode == 3, (checked.stdout, checked.stderr)
            value = json.loads(checked.stdout)
            assert value["journal_complete"] and value["effect_unknown_count"] == 1
            assert value["unresolved"][0]["recovery_state"] == "effect_unknown"
            assert value["unresolved"][0]["last_recorded_status"] == "started"
            assert hashlib.sha256(journal.read_bytes()).hexdigest() == digest
            assert marker.read_text() == "committed"
            checks += 1
        finally:
            if proc.poll() is None: proc.kill(); proc.wait(timeout=5)
            if child_pid.exists():
                pid = int(child_pid.read_text())
                try: os.killpg(pid, signal.SIGKILL)
                except ProcessLookupError: pass
            for stream in (proc.stdin, proc.stdout, proc.stderr): stream.close()
        print(f"PASS: {checks} recovery checks including real commit then SIGKILL; no replay or journal mutation")


if __name__ == "__main__": main()
