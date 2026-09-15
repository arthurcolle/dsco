#!/usr/bin/env python3
"""Local provider replay for DSCO auto-goal limits and the goal_queue wire schema."""

import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=Path("./dsco"))
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    requests = []
    errors = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            try:
                assert self.path == "/v1/chat/completions", self.path
                length = int(self.headers["Content-Length"])
                request = json.loads(self.rfile.read(length))
                requests.append(request)
                event = {
                    "id": "goal-controller-fixture",
                    "choices": [{
                        "index": 0,
                        "delta": {"content": "FIXTURE_STEP_FINISHED"},
                        "finish_reason": None,
                    }],
                }
                stop = {
                    "id": "goal-controller-fixture",
                    "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                    "usage": {"prompt_tokens": 10, "completion_tokens": 2, "cost": 0},
                }
                payload = (
                    "data: " + json.dumps(event) + "\n\n"
                    + "data: " + json.dumps(stop) + "\n\n"
                    + "data: [DONE]\n\n"
                ).encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            except Exception as exc:  # surfaced after the child exits
                errors.append(repr(exc))
                self.close_connection = True

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="dsco-goal-live-") as temp:
            skill = Path(temp) / ".dsco/workspace/skills/fixture-skill/SKILL.md"
            skill.parent.mkdir(parents=True)
            skill.write_text("---\nname: fixture-skill\ndescription: Use only for fixture skill routing.\n---\n# Fixture\nUNLOADED_SKILL_BODY_SENTINEL\n")
            env = {
                "HOME": temp,
                "TMPDIR": temp,
                "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
                "TERM": "dumb",
                "LANG": "en_US.UTF-8",
                "OPENAI_API_KEY": "fixture-only",
                "OPENAI_API_BASE": f"http://127.0.0.1:{server.server_port}/v1",
                "DSCO_ENV_FILE": "/dev/null",
                "DSCO_PRICING_OFFLINE": "1",
                "DSCO_SECURE_STORE_NO_PROMPT": "1",
                "DSCO_DISABLE_DEFAULT_FALLBACKS": "1",
                "DSCO_AUTO_FALLBACK": "0",
                "DSCO_DYNAMIC_FAILOVER": "0",
                "DSCO_DISABLE_PROVIDER_FABRIC_AUTO": "1",
                "DSCO_NO_AUTO_SUPERVISE": "1",
                "DSCO_NO_SUPERVISE": "1",
                "DSCO_KITTY_AGENT_WINDOWS": "0",
                "DSCO_PIXEL_TUI": "0",
                "DSCO_KITTY_GRAPHICS": "0",
                "DSCO_MCP_HEADLESS": "0",
                "DSCO_BANNER": "0",
                "DSCO_KITTY_BANNER": "0",
                "DSCO_GOAL_MAX_TURNS": "1",
                "DSCO_GOAL_TOKEN_BUDGET": "12345",
                "DSCO_HARD_TURN_CEILING": "4",
                "DSCO_BUDGET": "1",
            }
            proc = subprocess.run(
                [str(binary), "--profile", "worker", "--provider", "openai",
                 "-m", "fixture-model", "Build a verified fixture"],
                cwd=temp,
                env=env,
                text=True,
                capture_output=True,
                timeout=30,
            )
        assert not errors, errors
        assert len(requests) == 1, f"expected one bounded request, got {len(requests)}"
        request = requests[0]
        wire = json.dumps(request, ensure_ascii=False)
        assert "[Two-queue hierarchical goal controller]" in wire, wire[-4000:]
        assert "Build a verified fixture" in wire, wire[-4000:]
        assert "fixture-skill: Use only for fixture skill routing." in wire, wire[-8000:]
        assert "fixture-skill: ---" not in wire
        assert "UNLOADED_SKILL_BODY_SENTINEL" not in wire
        assert "Select skills by their description and current task" in wire, wire[-8000:]
        assert "Root must be decomposed first" in wire, wire[-6000:]
        assert "decompose: action, task_id, revision, children ONLY" in wire, wire[-6000:]
        assert "Never guess task IDs or increment revisions yourself" in wire, wire[-6000:]
        assert "no extra update_goal" in wire, wire[-6000:]
        functions = {
            item.get("function", item).get("name"): item.get("function", item)
            for item in request.get("tools", [])
        }
        assert "goal_queue" in functions, sorted(functions)
        schema = functions["goal_queue"].get("parameters", {})
        evidence = schema["properties"]["evidence"]
        reason = schema["properties"]["reason"]
        assert evidence.get("maxLength") == 511, evidence
        assert reason.get("maxLength") == 255, reason
        action_help = schema["properties"]["action"]["description"]
        assert "decompose=action,task_id,revision,children (no evidence/reason)" in action_help
        assert "never predict" in schema["properties"]["revision"]["description"]
        combined = proc.stdout + "\n" + proc.stderr
        assert "goal turn limit reached" in combined, combined[-4000:]
        assert proc.returncode == 1, (proc.returncode, combined[-4000:])
        print(json.dumps({
            "passed": True,
            "binary": str(binary),
            "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "provider_requests": len(requests),
            "auto_goal": "default action classifier",
            "controller_context": True,
            "exit_code": proc.returncode,
            "stop_reason": "goal turn limit reached",
            "goal_queue_schema": {
                "evidence_maxLength": evidence["maxLength"],
                "reason_maxLength": reason["maxLength"],
            },
        }, indent=2))
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


if __name__ == "__main__":
    main()
