#!/usr/bin/env python3
"""Native OAuth and streaming proof; external harness executables are traps."""
import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from test_stream_completion import response


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True)
    args = parser.parse_args()
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            self.rfile.read(int(self.headers["Content-Length"]))
            requests.append(dict(self.headers))
            lane = "anthropic" if self.path == "/anthropic" else "responses"
            data = response(lane, "success")
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            boundary = b'data: {"type": "message_delta"' if lane == "anthropic" else b'data: {"type": "response.completed"'
            split = data.index(boundary)
            self.wfile.write(data[:split])
            self.wfile.flush()
            time.sleep(0.2)
            self.wfile.write(data[split:])

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    results = []
    try:
        for case in ("unconfigured", "env", "dsco-login", "imported-login", "legacy-flags", "anthropic"):
            with tempfile.TemporaryDirectory(prefix="dsco-native-transport-") as tmp:
                folder = Path(tmp)
                marker = folder / "harness-was-executed"
                for executable in ("codex", "claude"):
                    trap = folder / executable
                    trap.write_text('#!/bin/sh\nprintf invoked > "$HARNESS_TRAP"\nexit 92\n')
                    trap.chmod(0o700)
                env = {"PATH": tmp + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin"),
                       "HOME": tmp, "TMPDIR": tmp, "DSCO_ENV_FILE": "/dev/null",
                       "DSCO_PRICING_OFFLINE": "1", "HARNESS_TRAP": str(marker),
                       "DSCO_DISABLE_CLAUDE_CODE_OAUTH_DISCOVERY": "1"}
                token = "fixture-native-token"
                account = "fixture-account"
                if case == "dsco-login":
                    cache = folder / ".dsco" / "chatgpt-oauth.json"
                    cache.parent.mkdir()
                    cache.write_text(json.dumps({"access_token": token, "account_id": account}))
                elif case in ("imported-login", "legacy-flags"):
                    cache = folder / ".codex" / "auth.json"
                    cache.parent.mkdir()
                    cache.write_text(json.dumps({"auth_mode": "chatgpt", "tokens": {
                        "access_token": token, "account_id": account}}))
                    if case == "legacy-flags":
                        env["DSCO_DISABLE_CHATGPT_NATIVE"] = "1"
                        env["DSCO_CHATGPT_NATIVE"] = "0"
                elif case == "env":
                    env["DSCO_CHATGPT_OAUTH_TOKEN"] = token
                    env["DSCO_CHATGPT_ACCOUNT_ID"] = account
                elif case == "anthropic":
                    token = "sk-ant-oat01-fixture-native-oauth"
                    env["CLAUDE_CODE_OAUTH_TOKEN"] = token
                lane = "anthropic-auth" if case == "anthropic" else (
                    "responses" if case == "unconfigured" else "responses-auth")
                endpoint = "anthropic" if case == "anthropic" else "responses"
                cp = subprocess.run([str(args.fixture.resolve()), lane,
                    f"http://127.0.0.1:{server.server_port}/{endpoint}"],
                    env=env, capture_output=True, text=True, timeout=20)
                assert cp.returncode == 0, (case, cp.stderr)
                result = json.loads(cp.stdout)
                assert result["ok"] and result["visible"] == "PARTIAL-KEPT", (case, result)
                assert result["thinking"] == "", (case, result)
                assert result["text_before_completion_ms"] >= 100, (case, result)
                assert not marker.exists(), (case, "external harness was launched")
                headers = {key.lower(): value for key, value in requests[-1].items()}
                expected = "fixture-only" if case == "unconfigured" else token
                assert headers["authorization"] == "Bearer " + expected, (case, headers.keys())
                if case not in ("anthropic", "unconfigured"):
                    assert headers["chatgpt-account-id"] == account, (case, headers.keys())
                if case == "anthropic":
                    assert "oauth-2025-04-20" in headers["anthropic-beta"], headers.keys()
                    assert "x-api-key" not in headers, headers.keys()
                results.append({"case": case, "passed": True,
                                "text_before_completion_ms": result["text_before_completion_ms"]})
    finally:
        server.shutdown()
        server.server_close()
    print(json.dumps({"passed": True, "scenarios": results,
                      "native_requests": len(requests), "external_harness_launches": 0}, indent=2))


if __name__ == "__main__":
    main()
