#!/usr/bin/env python3
"""Exercise SIGTERM -> provider cancellation through a real durable CLI worker.

All credentials, state, and HTTP responses are local fixtures. No model inference.
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    entered = threading.Event()
    release = threading.Event()
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def do_POST(self):
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            parsed = json.loads(body)
            requests.append({"path": self.path, "model": parsed.get("model"),
                             "stream": parsed.get("stream"), "bytes": len(body),
                             "fixture_auth": self.headers.get("Authorization") ==
                             "Bearer fixture-only"})
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.flush()
            if len(requests) == 1:
                event = {"id": "fixture", "choices": [{"index": 0,
                         "delta": {"content": "READY"}, "finish_reason": None}]}
                final = {"id": "fixture", "choices": [{"index": 0,
                         "delta": {}, "finish_reason": "stop"}],
                         "usage": {"prompt_tokens": 1, "completion_tokens": 1, "cost": 0}}
                self.wfile.write(("data: " + json.dumps(event) + "\n\n" +
                                  "data: " + json.dumps(final) + "\n\n" +
                                  "data: [DONE]\n\n").encode())
                self.wfile.flush()
            else:
                entered.set()
                release.wait(15)
            self.close_connection = True

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    result = {"binary": str(binary),
              "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "fixture": "boot success, durable task quiet SSE, SIGTERM",
              "requests": requests}
    proc = None
    try:
        with tempfile.TemporaryDirectory(prefix="dsco-interrupt-binary-") as tmp:
            home = Path(tmp)
            db = home / "bus.db"
            env = {"HOME": tmp, "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
                   "TMPDIR": tmp, "TERM": "dumb", "LANG": "en_US.UTF-8",
                   "OPENAI_API_KEY": "fixture-only",
                   "OPENAI_API_BASE": f"http://127.0.0.1:{server.server_port}/v1",
                   "DSCO_ENV_FILE": "/dev/null", "DSCO_PRICING_OFFLINE": "1",
                   "DSCO_DISABLE_DEFAULT_FALLBACKS": "1",
                   "DSCO_DISABLE_PROVIDER_FABRIC_AUTO": "1",
                   "DSCO_DURABLE_AUTOWAKE": "0", "DSCO_AGENTS_DB": str(db),
                   "DSCO_IPC_DB": str(db), "DSCO_MCP_HEADLESS": "0"}
            setup = []
            for command in [["agents", "create", "fixture-worker", "--model", "fixture-model"],
                            ["agents", "create", "fixture-controller"],
                            ["agents", "task", "--from", "fixture-controller", "--to",
                             "fixture-worker", "Wait for the local fixture response."]]:
                cp = subprocess.run([str(binary), *command], env=env, cwd=tmp,
                                    capture_output=True, text=True, timeout=10)
                setup.append({"command": command, "returncode": cp.returncode,
                              "stdout": cp.stdout, "stderr": cp.stderr})
                assert cp.returncode == 0, setup[-1]
            (output / "setup.json").write_text(json.dumps(setup, indent=2) + "\n")
            env.update({"DSCO_SUBAGENT": "1", "DSCO_DURABLE_AGENT_ID": "fixture-worker",
                        "DSCO_DURABLE_POLL_MS": "100", "DSCO_DURABLE_IDLE_EXIT_MS": "1000"})
            command = [str(binary), "--profile", "worker", "--provider", "openai",
                       "-m", "fixture-model", "-p", "Return READY."]
            result["command"] = command
            with (output / "stdout.txt").open("w") as stdout, (output / "stderr.txt").open("w") as stderr:
                proc = subprocess.Popen(command, env=env, cwd=tmp, stdin=subprocess.DEVNULL,
                                        stdout=stdout, stderr=stderr, start_new_session=True)
                assert entered.wait(12), "worker did not reach second fixture request"
                with sqlite3.connect(db) as conn:
                    before = conn.execute("SELECT status FROM tasks ORDER BY id").fetchall()
                result["task_status_before_signal"] = before
                assert before == [("running",)], before
                time.sleep(0.25)
                start = time.perf_counter()
                proc.send_signal(signal.SIGTERM)
                result["returncode"] = proc.wait(timeout=5)
                result["signal_to_exit_ms"] = (time.perf_counter() - start) * 1000
            assert "READY" in (output / "stdout.txt").read_text(), "boot response failed"
            stderr_text = (output / "stderr.txt").read_text()
            assert "Operation was aborted by an application callback" in stderr_text
            assert "headless accounting unavailable" not in stderr_text
            with sqlite3.connect(db) as conn:
                conn.row_factory = sqlite3.Row
                result["tasks"] = [dict(r) for r in conn.execute("SELECT * FROM tasks")]
                result["worker"] = dict(conn.execute(
                    "SELECT * FROM agents WHERE id='fixture-worker'").fetchone())
                with sqlite3.connect(output / "bus.db") as backup:
                    conn.backup(backup)
            assert len(requests) == 2, requests
            assert all(r["path"] == "/v1/chat/completions" and r["fixture_auth"]
                       and r["model"] == "fixture-model" and r["stream"] for r in requests)
            assert result["returncode"] == 0, result
            assert result["tasks"][0]["status"] == "failed", result["tasks"]
            assert result["tasks"][0]["result"] == "execution failed", result["tasks"]
            assert result["worker"]["status"] == "durable", result["worker"]
            assert result["worker"]["pid"] == 0, result["worker"]
            assert result["worker"]["current_task"] == "", result["worker"]
            result["passed"] = True
    except Exception as exc:
        result["passed"] = False
        result["error"] = repr(exc)
        raise
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait(timeout=3)
        release.set()
        server.shutdown()
        server.server_close()
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
