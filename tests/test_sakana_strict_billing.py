#!/usr/bin/env python3
"""Linked one-shot CLI retains selected Sakana billing principal on failures."""
import http.server
import json
from pathlib import Path
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
requests = []
current = {}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests.append({"authorization": self.headers.get("Authorization"), "body": body})
        success = current["legacy"] and len(requests) > 1
        if success:
            payload = ('data: ' + json.dumps({"choices": [{"index": 0, "delta": {"content": "OK"}, "finish_reason": "stop"}]}) + '\n\ndata: [DONE]\n\n').encode()
        else:
            payload = json.dumps({"error": {"message": "fixture credits exhausted", "type": "insufficient_quota"}}).encode()
        self.send_response(200 if success else current["status"])
        self.send_header("Content-Type", "text/event-stream" if success else "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix="dsco-sakana-strict-") as tmp:
        env = {"PATH": "/usr/bin:/bin", "HOME": tmp, "TMPDIR": tmp, "DSCO_ENV_FILE": "/dev/null",
               "FUGU_API_KEY": "fish_fixture_subscription", "FUGU_PAYG_API_KEY": "fixture_payg",
               "SAKANA_API_BASE": f"http://127.0.0.1:{server.server_port}/v1",
               "DSCO_DISABLE_SHARED_HOME_OAUTH": "1", "DSCO_PRICING_OFFLINE": "1",
               "DSCO_SECURE_STORE_NO_PROMPT": "1", "DSCO_CREDENTIAL_DISCOVERY_NO_PROMPT": "1",
               "DSCO_DISABLE_DEFAULT_FALLBACKS": "1", "DSCO_AUTO_FALLBACK": "0",
               "DSCO_DYNAMIC_FAILOVER": "0", "DSCO_DISABLE_PROVIDER_FABRIC_AUTO": "1",
               "DSCO_NO_AUTO_SUPERVISE": "1", "DSCO_NO_SUPERVISE": "1", "DSCO_AUTO_GOAL": "0",
               "DSCO_TOOL_CHOICE": "none", "DSCO_OR_DISABLE_TOOLS": "1", "DSCO_TOOLMGMT": "0",
               "DSCO_MCP_HEADLESS": "0", "DSCO_HARD_TURN_CEILING": "1", "DSCO_CHEAP": "1",
               "DSCO_MAX_TOKENS": "256", "DSCO_PIXEL_TUI": "0", "DSCO_BANNER": "0",
               "DSCO_BUDGET": "0.05", "DSCO_LLM_MAX_RETRIES": "0"}
        for strict, billing, status in [(True, "payg", 402), (True, "subscription", 402),
                                       (True, "payg", 429), (True, "subscription", 429),
                                       (False, "payg", 402)]:
            requests.clear()
            current.update(legacy=not strict, status=status)
            env.update(DSCO_STRICT_MODEL_ID="1" if strict else "0", DSCO_SAKANA_KEY_CLASS=billing)
            result = subprocess.run([str(ROOT / "dsco"), "--profile", "worker", "--provider", "sakana",
                                     "--model", "fugu-ultra", "--effort", "high", "--prompt", "Reply exactly OK."],
                                    cwd=tmp, env=env, capture_output=True, text=True, timeout=20)
            key = "fixture_payg" if billing == "payg" else "fish_fixture_subscription"
            assert requests and requests[0]["authorization"] == "Bearer " + key, (strict, billing, result.stderr)
            assert requests[0]["body"]["model"] == "fugu-ultra"
            assert requests[0]["body"]["max_completion_tokens"] == 256
            assert "tools" not in requests[0]["body"]
            if strict:
                assert len(requests) == 1, (billing, status, len(requests), result.stderr)
                assert result.returncode != 0, result.stderr
                assert "fallback:" not in result.stderr, result.stderr
            else:
                assert len(requests) == 2, (len(requests), result.stderr)
                assert requests[1]["authorization"] == "Bearer fish_fixture_subscription"
                assert result.returncode == 0, result.stderr
finally:
    server.shutdown()
    server.server_close()
print("PASS: strict PAYG/subscription 402/429 each sent one request; legacy fallback preserved; 256 output cap and no tools verified")
