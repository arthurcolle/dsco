#!/usr/bin/env python3
"""Smoke test DSCO callback outbox, drain, signature, and reconcile.

This test is local-only: it creates a temporary DSCO_RUNS_DIR, synthesizes a
single valid journal.wal frame, reconciles that frame into the callback outbox,
drains the outbox into a loopback HTTP server, and verifies the HMAC signature
headers over the exact posted body.

Usage:
  tests/smoke_callbacks.py
  DSCO_BIN=/path/to/dsco tests/smoke_callbacks.py
"""

from __future__ import annotations

import binascii
import hashlib
import hmac
import http.server
import json
import os
import pathlib
import shutil
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any


REPO = pathlib.Path(__file__).resolve().parents[1]
DSCO = pathlib.Path(os.environ.get("DSCO_BIN", REPO / "dsco"))
RUN_ID = "smoke-callbacks-run"
SECRET = "smoke-secret-local-only"
EVENT_TYPE = "run.started"


class CaptureHandler(http.server.BaseHTTPRequestHandler):
    requests: list[dict[str, Any]] = []

    def do_POST(self) -> None:  # noqa: N802 - stdlib callback name
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        CaptureHandler.requests.append(
            {
                "path": self.path,
                "headers": {k.lower(): v for k, v in self.headers.items()},
                "body": body,
            }
        )
        self.send_response(204)
        self.end_headers()

    def log_message(self, fmt: str, *args: object) -> None:
        return


class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True


def write_wal_record(path: pathlib.Path, record_type: str, payload: dict[str, Any]) -> None:
    rec = {
        "v": 1,
        "type": record_type,
        "run_id": RUN_ID,
        "seq": 1,
        "wall_ms": int(time.time() * 1000),
        "payload": payload,
    }
    data = (json.dumps(rec, separators=(",", ":")) + "\n").encode()
    frame = struct.pack("<II", len(data), binascii.crc32(data) & 0xFFFFFFFF) + data
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(frame)


def run_dsco(args: list[str], env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(DSCO), *args],
        cwd=REPO,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=20,
        check=False,
    )


def require(ok: bool, msg: str) -> None:
    if not ok:
        raise AssertionError(msg)


def main() -> int:
    require(DSCO.exists(), f"dsco binary not found: {DSCO}")
    require(os.access(DSCO, os.X_OK), f"dsco binary is not executable: {DSCO}")

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="dsco-callback-smoke-"))
    keep = os.environ.get("DSCO_SMOKE_KEEP_TMP") == "1"
    try:
        runs = tmp / "runs"
        run_dir = runs / RUN_ID
        outbox = run_dir / "outbox"
        journal = run_dir / "journal.wal"
        payload = {"schema": "smoke.callback.v1", "status": "started", "n": 7}
        write_wal_record(journal, EVENT_TYPE, payload)

        with ThreadedTCPServer(("127.0.0.1", 0), CaptureHandler) as server:
            port = int(server.server_address[1])
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()

            env = os.environ.copy()
            env.update(
                {
                    "DSCO_RUNS_DIR": str(runs),
                    "DSCO_CALLBACK_URL": f"http://127.0.0.1:{port}/callback",
                    "DSCO_CALLBACK_EVENTS": "run.*",
                    "DSCO_CALLBACK_SECRET": SECRET,
                    "DSCO_CALLBACK_MAX_ATTEMPTS": "1",
                    # Explicit local-test escape hatch. Production defaults deny loopback/private egress.
                    "DSCO_WEBHOOK_ALLOW_PRIVATE": "1",
                    # Keep unrelated runtime state inside the temp tree if the binary consults HOME.
                    "HOME": str(tmp / "home"),
                }
            )

            reconcile = run_dsco(["callbacks", "reconcile", "--run", RUN_ID], env)
            require(
                reconcile.returncode == 0,
                f"reconcile failed rc={reconcile.returncode}\nstdout={reconcile.stdout}\nstderr={reconcile.stderr}",
            )
            rec_json = json.loads(reconcile.stdout.strip())
            require(rec_json.get("seen") == 1, f"expected one journal record, got {rec_json}")
            require(rec_json.get("enqueued") == 1, f"expected one enqueued callback, got {rec_json}")
            require(outbox.exists(), "reconcile did not create outbox")

            json_files = sorted(outbox.glob("*.json"))
            state_files = sorted(outbox.glob("*.state"))
            require(len(json_files) == 1, f"expected one outbox json, found {json_files}")
            require(len(state_files) == 1, f"expected one outbox state, found {state_files}")
            outbox_doc = json.loads(json_files[0].read_text())
            require(outbox_doc["schema"] == "dsco.callback_outbox.v1", "bad outbox schema")
            require(outbox_doc["run_id"] == RUN_ID, "bad outbox run_id")
            require(outbox_doc["event_name"] == EVENT_TYPE, "bad outbox event_name")
            require(outbox_doc["event"] == payload, "reconciled payload changed")

            listed = run_dsco(["callbacks", "list", "--run", RUN_ID], env)
            require(listed.returncode == 0, f"list failed: {listed.stderr}")
            require(json_files[0].name in listed.stdout, "list output missing outbox json")
            require(state_files[0].name in listed.stdout, "list output missing outbox state")

            drain = run_dsco(["callbacks", "drain", "--run", RUN_ID], env)
            require(
                drain.returncode == 0,
                f"drain failed rc={drain.returncode}\nstdout={drain.stdout}\nstderr={drain.stderr}",
            )
            drain_json = json.loads(drain.stdout.strip())
            require(drain_json.get("delivered") == 1, f"expected one delivery, got {drain_json}")
            require(len(CaptureHandler.requests) == 1, f"expected one HTTP request, got {len(CaptureHandler.requests)}")

            req = CaptureHandler.requests[0]
            body = req["body"]
            headers = req["headers"]
            require(req["path"] == "/callback", f"bad callback path: {req['path']}")
            require(headers.get("x-dsco-delivery-id") == outbox_doc["delivery_id"], "delivery id header mismatch")
            ts = headers.get("x-dsco-timestamp")
            sig = headers.get("x-dsco-signature")
            require(ts and ts.isdigit(), f"missing/bad timestamp header: {ts!r}")
            expected = hmac.new(SECRET.encode(), ts.encode() + b"." + body, hashlib.sha256).hexdigest()
            require(sig == f"v1={expected}", f"signature mismatch: got={sig!r} expected=v1={expected}")
            posted = json.loads(body)
            require(posted == outbox_doc, "posted body differs from outbox document")

            final_state = json.loads(state_files[0].read_text())
            require(final_state["state"] == "delivered", f"bad final state: {final_state}")
            require(final_state["attempts"] == 1, f"bad attempts count: {final_state}")
            require(final_state["last_http_status"] == 204, f"bad HTTP status: {final_state}")

            server.shutdown()

        print(
            json.dumps(
                {
                    "ok": True,
                    "run_id": RUN_ID,
                    "outbox_files": [p.name for p in sorted(outbox.iterdir())],
                    "delivered": 1,
                    "signature": "verified",
                    "tmp": str(tmp) if keep else None,
                },
                sort_keys=True,
            )
        )
        return 0
    finally:
        if not keep:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as e:  # smoke script: concise failure surface
        print(f"FAIL: {e}", file=sys.stderr)
        raise SystemExit(1)
