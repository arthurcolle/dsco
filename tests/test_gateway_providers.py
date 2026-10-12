#!/usr/bin/env python3
"""Qualify real Ramp/Vercel C transport against isolated, local wire fixtures."""
import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
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
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append((self.path, dict(self.headers), body))
            case = self.path.split("/")[1]
            if self.path.endswith("/chat/completions"):
                data = ('data: ' + json.dumps({"choices": [{"index": 0, "delta": {"content": "VERCEL-OK"}, "finish_reason": "stop"}]}) + '\n\ndata: [DONE]\n\n').encode()
            else:
                data = response("responses", case)
            self.send_response(401 if case == "auth_failure" else 200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            # Exercise arbitrary TCP boundaries, including split SSE JSON.
            for start in range(0, len(data), 13):
                self.wfile.write(data[start:start + 13])
                self.wfile.flush()

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    results = []
    try:
        with tempfile.TemporaryDirectory(prefix="dsco-gateway-fixture-") as tmp:
            env = {"HOME": tmp, "PATH": "/usr/bin:/bin", "TMPDIR": tmp,
                   "DSCO_ENV_FILE": "/dev/null", "DSCO_PRICING_OFFLINE": "1",
                   "DSCO_OR_DISABLE_TOOLS": "1", "DSCO_DISABLE_SHARED_HOME_OAUTH": "1",
                   "DSCO_DISABLE_CLAUDE_CODE_OAUTH_DISCOVERY": "1",
                   "DSCO_DISABLE_CODEX_OAUTH_DISCOVERY": "1",
                   "DSCO_CHATGPT_OAUTH_TOKEN": "must-not-leak-codex",
                   "DSCO_CHATGPT_ACCOUNT_ID": "must-not-leak-account",
                   "DSCO_CHATGPT_BASE_URL": "http://127.0.0.1:1/must-not-route-here",
                   "ABLITERATION_API_KEY": "must-not-leak-ablit",
                   "DSCO_ABLITERATION_PARAMS": '{"flagged_categories":["must-not-leak"]}'}
            for alias, case in [("ramp-router", "success"), ("ramp", "tool_success"),
                                ("router-com", "missing_terminal"), ("ramp-router", "tool_invalid"),
                                ("ramp-router", "incomplete"), ("ramp-router", "missing"),
                                ("ramp-router", "auth_failure"), ("vercel-ai-gateway", "success"),
                                ("vercel", "missing")]:
                before = len(requests)
                endpoint = f"http://127.0.0.1:{server.server_port}/{case}/v1"
                cp = subprocess.run([str(args.fixture.resolve()), alias, endpoint, case],
                                    cwd=tmp, env=env, capture_output=True, text=True, timeout=15)
                assert cp.returncode == 0, (alias, case, cp.stderr)
                result = json.loads(cp.stdout)
                ramp = not alias.startswith("vercel")
                body = result["body"]
                assert body["model"] == ("kimi-k3-fast" if ramp else "moonshotai/kimi-k3"), body
                if ramp:
                    assert body["max_output_tokens"] == 64 and body["stream"] and not body["store"]
                    assert any(item["type"] == "function_call" for item in body["input"])
                    assert any(item["type"] == "function_call_output" for item in body["input"])
                    assert any(part["type"] == "input_image" for item in body["input"] for part in item.get("content", []))
                    assert not {"prompt_cache_key", "include", "flagged_categories", "service_tier", "reasoning"} & body.keys(), body.keys()
                if case == "missing":
                    assert len(requests) == before and not result["ok"] and result["http_status"] == 0
                    if ramp: assert result["stop_reason"] == "missing_api_key"
                else:
                    assert len(requests) == before + 1
                    path, raw_headers, _ = requests[-1]
                    headers = {key.lower(): value for key, value in raw_headers.items()}
                    assert headers["authorization"] == "Bearer fixture-gateway-key"
                    assert not {"chatgpt-account-id", "openai-beta", "x-api-key"} & headers.keys(), headers.keys()
                    assert "must-not-leak" not in json.dumps(raw_headers) + json.dumps(body)
                    assert path == f"/{case}/v1/" + ("responses" if ramp else "chat/completions")
                    assert result["ok"] == (case in ("success", "tool_success")), result
                    if case == "tool_success":
                        assert result["tools"] == [{"name": "write_file", "id": "fixture-call", "arguments": "{}"}], result
                    if case == "success":
                        assert result["visible"] == ("PARTIAL-KEPT" if ramp else "VERCEL-OK")
                    if case == "auth_failure":
                        assert "ramp-router auth failed" in cp.stderr and "/login chatgpt" not in cp.stderr
                results.append({"provider": alias, "case": case, "passed": True})
    finally:
        server.shutdown()
        server.server_close()
    print(json.dumps({"passed": True, "scenarios": results, "local_requests": len(requests),
                      "paid_requests": 0}, indent=2))


if __name__ == "__main__":
    main()
