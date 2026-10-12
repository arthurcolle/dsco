#!/usr/bin/env python3
"""Kimi refresh readiness, same-process credential recovery, and real breaker isolation.

Production C paths, loopback OAuth/inference, temporary HOME, no real credentials.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import pty
import select
import subprocess
import tempfile
import threading
import time
import urllib.parse


def run(fixture, case, binary=False):
    requests, errors = [], []
    with tempfile.TemporaryDirectory(prefix="dsco-kimi-pool-") as tmp:
        home = Path(tmp)
        credentials = home / ".kimi-code/credentials/kimi-code.json"
        credentials.parent.mkdir(parents=True)

        def credential(expired=False, refresh=True):
            value = dict(access_token="fixture-access", expires_at=time.time() + (-600 if expired else 3600))
            if refresh:
                value["refresh_token"] = "fixture-refresh"
            credentials.write_text(json.dumps(value))
            credentials.chmod(0o600)

        if case not in ("missing", "recover", "environment"):
            credential(expired=case in ("expired", "no_refresh", "refresh_denied"),
                       refresh=case != "no_refresh")
        if case == "skew":
            credentials.write_text(json.dumps(dict(access_token="fixture-access",
                refresh_token="fixture-refresh", expires_at=time.time() + 60)))

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                try:
                    body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                    requests.append(self.path)
                    if self.path == "/api/oauth/token":
                        form = urllib.parse.parse_qs(body.decode())
                        assert form["grant_type"] == ["refresh_token"]
                        assert form["refresh_token"] == ["fixture-refresh"]
                        if case == "refresh_denied":
                            self.send_response(401)
                            data = b'{"error":"invalid_grant"}'
                        else:
                            self.send_response(200)
                            data = json.dumps(dict(access_token="fixture-refreshed",
                                refresh_token="fixture-rotated", expires_in=3600)).encode()
                        self.send_header("Content-Type", "application/json")
                    else:
                        assert self.path == "/coding/v1/chat/completions", self.path
                        expected = "fixture-refreshed" if case in ("expired", "skew") else "fixture-access"
                        assert self.headers.get("Authorization") == "Bearer " + expected
                        assert self.headers.get("X-Msh-Platform") == "kimi_code_cli"
                        self.send_response(200)
                        event = dict(id="fixture", choices=[dict(index=0,
                            delta=dict(content="KIMI_FIXTURE_OK"), finish_reason="stop")],
                            usage=dict(prompt_tokens=1, completion_tokens=1))
                        data = ("data: " + json.dumps(event) + "\n\ndata: [DONE]\n\n").encode()
                        self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                except Exception as exc:
                    errors.append(repr(exc))
                    self.close_connection = True

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        env = dict(HOME=tmp, TMPDIR=tmp, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
                   DSCO_ENV_FILE="/dev/null", KIMI_CODE_HOME=str(home / ".kimi-code"),
                   DSCO_KIMI_CODE_OAUTH_TOKEN_URL=base + "/api/oauth/token", DSCO_SECURE_STORE_NO_PROMPT="1",
                   DSCO_PRICING_OFFLINE="1")
        if case == "environment":
            env["DSCO_KIMI_CODE_OAUTH_TOKEN"] = "fixture-access"
        proc = None
        try:
            if binary:
                env.update(KIMI_CODE_API_BASE=base + "/coding/v1", DSCO_AUTO_GOAL="0",
                    DSCO_AUTO_FALLBACK="0", DSCO_DYNAMIC_FAILOVER="0",
                    DSCO_DISABLE_PROVIDER_FABRIC_AUTO="1", DSCO_NO_AUTO_SUPERVISE="1",
                    DSCO_NO_SUPERVISE="1", DSCO_KITTY_AGENT_WINDOWS="0", DSCO_PIXEL_TUI="0",
                    DSCO_HARD_TURN_CEILING="1", DSCO_BUDGET="0.10",
                    DSCO_SYSTEM_PROMPT="Reply directly. Do not call tools.")
                # A hidden pseudo-terminal exercises the real interactive loop;
                # piped stdin intentionally exits after the initial prompt.
                master, slave = pty.openpty()
                transcript = bytearray()
                try:
                    env["TERM"] = "xterm-256color"
                    proc = subprocess.Popen([str(fixture), "--profile", "worker", "--provider",
                        "kimi-code", "--model", "kimi-code/k3", "--effort", "none", "-i",
                        "-p", "Reply exactly KIMI_FIXTURE_OK."], env=env, cwd=tmp,
                        stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
                    os.close(slave)
                    slave = -1

                    def wait_turn(count):
                        deadline = time.monotonic() + 25
                        while transcript.count(b"Prompt accounting:") < count:
                            assert time.monotonic() < deadline, transcript[-2000:].decode(errors="replace")
                            assert proc.poll() is None, transcript[-2000:].decode(errors="replace")
                            if select.select([master], [], [], 0.1)[0]:
                                transcript.extend(os.read(master, 65536))
                        # Let the composer transition out of the streaming input hook.
                        time.sleep(0.15)

                    wait_turn(1)
                    os.write(master, b"Reply exactly KIMI_FIXTURE_OK again.\r")
                    wait_turn(2)
                    os.write(master, b"/quit\r")
                    deadline = time.monotonic() + 10
                    while proc.poll() is None:
                        assert time.monotonic() < deadline, transcript[-2000:].decode(errors="replace")
                        if select.select([master], [], [], 0.1)[0]:
                            try:
                                transcript.extend(os.read(master, 65536))
                            except OSError:
                                break
                    proc.wait(timeout=3)
                finally:
                    os.close(master)
                    if slave >= 0:
                        os.close(slave)
                stdout, stderr = "", transcript.decode(errors="replace")
                assert proc.returncode == 0, (proc.returncode, stderr[-2000:])
                assert not errors, errors
                assert "provider unavailable" not in stdout + stderr, (stdout[-1000:], stderr[-1000:])
                assert (stdout + stderr).count("KIMI_FIXTURE_OK") >= 2, (stdout[-1000:], stderr[-1000:])
                expected = (["/api/oauth/token"] if case == "expired" else [])
                expected += ["/coding/v1/chat/completions"] * 2
                assert requests == expected, (requests, expected, stderr[-1000:])
                return dict(case="binary_" + case, passed=True, requests=requests)
            proc = subprocess.Popen([str(fixture), base + "/coding/v1", case],
                env=env, cwd=tmp, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True)
            if case == "recover":
                initial = json.loads(proc.stdout.readline())
                assert not initial["initial_healthy"], initial
                assert not requests, requests  # readiness must never perform network I/O
                credential()
                rest, stderr = proc.communicate("\n", timeout=20)
                rows = [initial] + [json.loads(s) for s in rest.splitlines() if s.startswith("{")]
            else:
                stdout, stderr = proc.communicate(timeout=20)
                rows = [json.loads(s) for s in stdout.splitlines() if s.startswith("{")]
            assert proc.returncode == 0, (proc.returncode, stderr[-2000:])
            assert not errors, errors
            initial, final = rows
            available = case not in ("missing", "no_refresh", "recover")
            healthy = case not in ("missing", "no_refresh", "breaker", "quota")
            success = healthy and case != "refresh_denied"
            assert initial["initial_available"] == available, (case, rows, requests)
            assert initial["initial_healthy"] == available, (case, rows, requests)
            assert final["healthy"] == healthy, (case, rows, requests)
            assert final["resolved"] == success and final["ok"] == success, (case, rows, requests)
            assert final["http_status"] == (200 if success else 0), (case, rows)
            expected = (["/api/oauth/token"] if case in ("expired", "skew", "refresh_denied") else [])
            if success:
                expected.append("/coding/v1/chat/completions")
            assert requests == expected, (case, requests, expected)
            if case in ("expired", "skew"):
                saved = json.loads(credentials.read_text())
                assert saved["access_token"] == "fixture-refreshed"
                assert saved["refresh_token"] == "fixture-rotated"
                assert saved["expires_at"] > time.time() + 3000
                assert credentials.stat().st_mode & 0o777 == 0o600
            return dict(case=case, passed=True, requests=requests)
        finally:
            if proc and proc.poll() is None:
                proc.kill()
                proc.communicate()
            server.shutdown()
            server.server_close()
            thread.join(2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--fixture", type=Path)
    inputs.add_argument("--binary", type=Path)
    args = parser.parse_args()
    cases = (("valid", "expired") if args.binary else
             ("valid", "expired", "skew", "no_refresh", "missing", "recover",
              "environment", "breaker", "quota", "refresh_denied"))
    target = (args.binary or args.fixture).resolve()
    failed = 0
    for case in cases:
        try:
            print(json.dumps(run(target, case, binary=bool(args.binary))), flush=True)
        except Exception as exc:
            failed += 1
            print(json.dumps(dict(case=case, passed=False, error=str(exc))), flush=True)
    raise SystemExit(bool(failed))


if __name__ == "__main__":
    main()
