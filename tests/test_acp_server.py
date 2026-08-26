#!/usr/bin/env python3
"""Protocol smoke for `dsco acp serve` without consuming a model provider."""

import json
import os
import stat
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="dsco-acp-test-") as tmp:
        fake = Path(tmp) / "fake-dsco"
        fake.write_text(
            "#!/bin/sh\n"
            "model=''\nprompt=''\n"
            "while [ \"$#\" -gt 0 ]; do\n"
            "  case \"$1\" in\n"
            "    --model) model=\"$2\"; shift 2 ;;\n"
            "    --prompt) prompt=\"$2\"; shift 2 ;;\n"
            "    *) shift ;;\n"
            "  esac\n"
            "done\n"
            "printf 'FAKE_DSCO_RESULT model=%s prompt=%s\\n' \"$model\" \"$prompt\"\n"
        )
        fake.chmod(fake.stat().st_mode | stat.S_IXUSR)

        env = os.environ.copy()
        env.update(
            {
                "DSCO_ACP_DSCO_BIN": str(fake),
                "DSCO_ROUTER_POLICY": "adaptive",
            }
        )
        proc = subprocess.Popen(
            [str(ROOT / "dsco"), "acp", "serve"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        assert proc.stdin and proc.stdout and proc.stderr

        def request(payload: dict) -> dict:
            proc.stdin.write(json.dumps(payload) + "\n")
            proc.stdin.flush()
            return json.loads(proc.stdout.readline())

        init = request({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        assert init["result"]["protocolVersion"] == 2
        assert init["result"]["agentInfo"]["name"] == "dsco-acp"

        created = request(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "session/new",
                "params": {"cwd": "/tmp", "systemPrompt": "system instruction"},
            }
        )
        session_id = created["result"]["sessionId"]
        assert session_id.startswith("dsco-")

        proc.stdin.write(
            json.dumps(
                {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "session/prompt",
                    "params": {
                        "sessionId": session_id,
                        "prompt": [{"type": "text", "text": "format this short request"}],
                    },
                }
            )
            + "\n"
        )
        proc.stdin.close()
        rows = [json.loads(line) for line in proc.stdout if line.strip()]
        stderr = proc.stderr.read()
        assert proc.wait(timeout=20) == 0, stderr
        assert rows[0]["method"] == "session/update"
        assert "[DSCO router]" in rows[0]["params"]["update"]["content"]["text"]
        assert rows[1]["method"] == "session/update"
        assert "FAKE_DSCO_RESULT" in rows[1]["params"]["update"]["content"]["text"]
        assert "format this short request" in rows[1]["params"]["update"]["content"]["text"]
        assert rows[2]["result"]["stopReason"] == "end_turn"

    print("dsco ACP server smoke: PASS")


if __name__ == "__main__":
    main()
