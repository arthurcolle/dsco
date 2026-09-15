#!/usr/bin/env python3
"""Exercise production stream adapters with local HTTP events and no inference."""
import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time


def event(data):
    return "data: " + json.dumps(data) + "\n\n"


def response(lane, case):
    if lane == "anthropic":
        data = event({"type": "message_start", "message": {"usage": {"input_tokens": 7}}})
        tool = case in ("tool_open", "tool_invalid", "tool_success")
        block = {"type": "tool_use", "id": "fixture-call", "name": "write_file", "input": {}} if tool else {"type": "text", "text": ""}
        data += event({"type": "content_block_start", "index": 0, "content_block": block})
        delta = {"type": "input_json_delta", "partial_json": "{}" if case == "tool_success" else "{broken"} if tool else {"type": "text_delta", "text": "PARTIAL-KEPT"}
        data += event({"type": "content_block_delta", "index": 0, "delta": delta})
        if case not in ("tool_open", "cancel"):
            data += event({"type": "content_block_stop", "index": 0})
        if case in ("success", "tool_invalid", "tool_success", "no_newline", "no_space"):
            data += event({"type": "message_delta", "delta": {"stop_reason": "tool_use" if tool else "end_turn"}, "usage": {"output_tokens": 5}})
            data += event({"type": "message_stop"})
        if case == "done_only":
            data += "data: [DONE]\n\n"
    else:
        data = event({"type": "response.created", "response": {"id": "fixture"}})
        if case in ("tool_open", "tool_invalid", "tool_success"):
            item = {"type": "function_call", "name": "write_file", "call_id": "fixture-call", "arguments": "{}" if case == "tool_success" else "{broken"}
            data += event({"type": "response.output_item.added", "item": item})
            if case in ("tool_invalid", "tool_success"):
                data += event({"type": "response.output_item.done", "item": item})
        else:
            data += event({"type": "response.output_text.delta", "delta": "PARTIAL-KEPT"})
        if case in ("success", "tool_invalid", "tool_success", "tool_open", "no_newline", "no_space"):
            # Repeated cumulative cache snapshots must not subtract twice.
            data += event({"type": "response.in_progress", "response": {"usage": {"input_tokens": 7, "input_tokens_details": {"cached_tokens": 3}}}})
            data += event({"type": "response.completed", "response": {"status": "completed", "usage": {"input_tokens": 7, "output_tokens": 5, "input_tokens_details": {"cached_tokens": 3}}}})
        if case == "incomplete":
            data += event({"type": "response.incomplete", "response": {"status": "incomplete", "usage": {"input_tokens": 7, "output_tokens": 5, "input_tokens_details": {"cached_tokens": 3}}}})
        if case == "done_only":
            data += "data: [DONE]\n\n"
    if case == "no_newline":
        data = data.rstrip("\n")
    if case == "no_space":
        data = data.replace("data: ", "data:")
    return data.encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lane", choices=("anthropic", "responses"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", "0")))
            lane, case = self.path.strip("/").split("/")
            requests.append(self.path)
            code = 503 if case in ("backoff_cancel", "partial_503") or (case == "retry" and requests.count(self.path) == 1) else 200
            self.send_response(code)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(b'{"error":{"message":"fixture transient"}}' if code != 200 and case != "partial_503" else response(lane, "success" if case == "retry" else case))
                self.wfile.flush()
                if case == "cancel":
                    time.sleep(2)
            except (BrokenPipeError, ConnectionResetError):
                pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    results = []
    try:
        with tempfile.TemporaryDirectory(prefix="dsco-stream-completion-") as tmp:
            env = {"HOME": tmp, "PATH": "/usr/bin:/bin", "TMPDIR": tmp, "LANG": "en_US.UTF-8",
                   "DSCO_ENV_FILE": "/dev/null", "DSCO_CHATGPT_OAUTH_TOKEN": "fixture-only",
                   "DSCO_CHATGPT_ACCOUNT_ID": "fixture-account", "DSCO_CHATGPT_GATE": "0",
                   "DSCO_LLM_MAX_RETRIES": "2", "DSCO_LLM_RETRY_DELAY_MS": "100",
                   "DSCO_DISABLE_SHARED_HOME_OAUTH": "1", "DSCO_PRICING_OFFLINE": "1"}
            for lane in ([args.lane] if args.lane else ("anthropic", "responses")):
                cases = ["success", "missing_terminal", "done_only", "tool_open", "tool_invalid", "tool_success", "no_newline", "no_space", "cancel", "partial_503"]
                cases += ["retry", "backoff_cancel"] if lane == "anthropic" else ["incomplete"]
                for case in cases:
                    url = f"http://127.0.0.1:{server.server_port}/{lane}/{case}"
                    command = [str(args.fixture.resolve()), lane, url]
                    if case in ("cancel", "backoff_cancel"):
                        command.append("250")
                    case_env = dict(env)
                    if case == "backoff_cancel":
                        case_env["DSCO_LLM_RETRY_DELAY_MS"] = "10000"
                    started = time.monotonic()
                    cp = subprocess.run(command, env=case_env, cwd=tmp, capture_output=True, text=True, timeout=8)
                    elapsed = time.monotonic() - started
                    (args.output / f"{lane}-{case}.stderr").write_text(cp.stderr)
                    assert cp.returncode == 0, (lane, case, cp.stderr)
                    result = json.loads(cp.stdout)
                    result.update(lane=lane, case=case, elapsed_ms=elapsed * 1000,
                                  requests=requests.count(f"/{lane}/{case}"))
                    results.append(result)
                    success = case in ("success", "tool_success", "no_newline", "no_space", "retry")
                    assert result["ok"] == success, result
                    assert result["requests"] == (2 if case == "retry" else 1), result
                    if case in ("missing_terminal", "done_only", "incomplete", "cancel", "partial_503"):
                        assert result["visible"] == "PARTIAL-KEPT", result
                        assert result["preserved_text"] == "PARTIAL-KEPT", result
                    if case in ("cancel", "backoff_cancel"):
                        assert elapsed < 1.7 and result["stop_reason"] == "interrupted", result
                    if success or (lane == "responses" and case == "incomplete"):
                        assert result["input_tokens"] == (4 if lane == "responses" else 7) and result["output_tokens"] == 5, result
                        assert result["cache_read_tokens"] == (3 if lane == "responses" else 0), result
    finally:
        server.shutdown()
        server.server_close()
        (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps({"passed": True, "scenarios": len(results), "requests": len(requests)}))


if __name__ == "__main__":
    main()
