#!/usr/bin/env python3
"""Live environment-hook proof against the built dsco binary.

The helper is an owned temporary executable. It receives hook envelopes over
stdin, records only the envelopes needed for assertions, and never invokes a
shell or network.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


HOOK = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

payload = json.load(sys.stdin)
log = Path(os.environ["DSCO_TEST_HOOK_LOG"])
with log.open("a", encoding="utf-8") as stream:
    stream.write(json.dumps(payload, sort_keys=True) + "\n")
decision = os.environ.get("DSCO_TEST_HOOK_DECISION", "allow")
if payload.get("event") == "tool.call":
    print(json.dumps({"decision": decision, "reason": "owned-test-policy"}))
'''


def run(binary, env, name="sha256", arguments='{"text":"hook-proof"}'):
    return subprocess.run(
        [str(binary), "--tool-exec", name, arguments],
        env=env,
        capture_output=True,
        text=True,
        timeout=15,
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    binary = Path(args.binary).resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="dsco-tool-hooks-") as tmp:
        root = Path(tmp)
        hook = root / "hook.py"
        log = root / "events.jsonl"
        hook.write_text(HOOK, encoding="utf-8")
        hook.chmod(0o700)

        env = os.environ.copy()
        for key in (
            "DSCO_TOOL_HOOK",
            "DSCO_TOOL_HOOK_BEFORE",
            "DSCO_TOOL_HOOK_AFTER",
            "DSCO_TOOL_HOOK_FAILED",
            "DSCO_TOOL_HOOK_DENIED",
            "DSCO_TOOL_HOOK_TOOLS",
            "DSCO_TEST_HOOK_DECISION",
        ):
            env.pop(key, None)
        env.update(
            DSCO_TOOL_HOOK_BEFORE=str(hook),
            DSCO_TOOL_HOOK_AFTER=str(hook),
            DSCO_TOOL_HOOK_DENIED=str(hook),
            DSCO_TOOL_HOOK_PAYLOAD="full",
            DSCO_TOOL_HOOK_TIMEOUT_MS="2000",
            DSCO_TOOL_HOOK_MAX_OUTPUT="4096",
            DSCO_TOOL_HOOK_FAIL_MODE="deny",
            DSCO_TEST_HOOK_LOG=str(log),
            DSCO_GOV_MODEL="none",
            DSCO_TOOLMGMT="0",
            DSCO_PRICING_OFFLINE="1",
            DSCO_CLOUD_SKIP_PLUGIN_INIT="1",
            DSCO_TRUST_TIER="trusted",
        )

        allowed = run(binary, env)
        assert allowed.returncode == 0, (allowed.returncode, allowed.stdout, allowed.stderr)
        assert "hook-proof" not in allowed.stdout, "hash input should not be echoed"
        result = json.loads(allowed.stdout)
        assert result.get("ok") is True, result
        events = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
        assert [event["event"] for event in events] == ["tool.call", "tool.result"], events
        assert events[0]["input"] == {"text": "hook-proof"}, events[0]
        assert events[1]["result"] is not None, events[1]
        assert events[0]["execution_id"] == events[1]["execution_id"], events

        env["DSCO_TEST_HOOK_DECISION"] = "deny"
        denied = run(binary, env)
        assert denied.returncode != 0, (denied.returncode, denied.stdout, denied.stderr)
        denied_result = json.loads(denied.stdout)
        assert denied_result.get("ok") is False, denied_result
        assert "tool_hook" in json.dumps(denied_result), denied_result
        events = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
        assert [event["event"] for event in events[-2:]] == [
            "tool.call",
            "tool.policy.blocked",
        ], events[-2:]

        env["DSCO_TEST_HOOK_DECISION"] = "allow"
        env["DSCO_TOOL_HOOK_FAILED"] = str(hook)
        missing_target = root / "tool-target-does-not-exist"
        failed_tool = run(binary, env, "read_file", json.dumps({"path": str(missing_target)}))
        assert failed_tool.returncode != 0, (failed_tool.returncode, failed_tool.stdout, failed_tool.stderr)
        failed_tool_result = json.loads(failed_tool.stdout)
        assert failed_tool_result.get("ok") is False, failed_tool_result
        events = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
        assert [event["event"] for event in events[-2:]] == ["tool.call", "tool.failed"], events[-2:]

        env["DSCO_TEST_HOOK_DECISION"] = "deny"
        env["DSCO_TOOL_HOOK_FAIL_MODE"] = "allow"
        explicit_deny = run(binary, env)
        assert explicit_deny.returncode != 0, (explicit_deny.returncode, explicit_deny.stdout, explicit_deny.stderr)
        explicit_deny_result = json.loads(explicit_deny.stdout)
        assert explicit_deny_result.get("ok") is False, explicit_deny_result
        assert "owned-test-policy" in json.dumps(explicit_deny_result), explicit_deny_result

        env["DSCO_TOOL_HOOK_FAIL_MODE"] = "deny"
        env["DSCO_TEST_HOOK_DECISION"] = "invalid"
        malformed = run(binary, env)
        assert malformed.returncode != 0, (malformed.returncode, malformed.stdout, malformed.stderr)
        malformed_result = json.loads(malformed.stdout)
        assert malformed_result.get("ok") is False, malformed_result
        assert "invalid decision" in json.dumps(malformed_result), malformed_result

        env["DSCO_TOOL_HOOK_BEFORE"] = str(root / "missing-hook")
        failed = run(binary, env)
        assert failed.returncode != 0, (failed.returncode, failed.stdout, failed.stderr)
        failed_result = json.loads(failed.stdout)
        assert failed_result.get("ok") is False, failed_result
        assert "unavailable" in json.dumps(failed_result), failed_result

        print(json.dumps({
            "ok": True,
            "binary": str(binary),
            "events": ["tool.call", "tool.result", "tool.policy.blocked", "tool.failed"],
            "pre_hook_denies": True,
            "failed_before_hook_denies_by_default": True,
            "raw_input_is_opt_in": True,
        }))


if __name__ == "__main__":
    main()
