#!/usr/bin/env python3
"""Verify trace discovery and capability policy through the real MCP server."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--binary", default="./dsco")
binary = Path(parser.parse_args().binary).resolve(strict=True)
with tempfile.TemporaryDirectory(prefix="dsco-trace-gate-") as directory:
    env = dict(HOME=directory, TMPDIR=directory, PATH="/usr/bin:/bin:/usr/sbin:/sbin",
               DSCO_ENV_FILE="/dev/null", DSCO_PRICING_OFFLINE="1",
               DSCO_SECURE_STORE_NO_PROMPT="1", DSCO_GOV_MODEL="standard",
               DSCO_ALLOW_WRITE="0", DSCO_NO_AUTO_SUPERVISE="1")
    requests = [dict(jsonrpc="2.0", id=1, method="initialize", params=dict(
        protocolVersion="2024-11-05", capabilities={}, clientInfo=dict(name="trace-test", version="1"))),
        dict(jsonrpc="2.0", method="notifications/initialized"),
        dict(jsonrpc="2.0", id=2, method="tools/list", params={})]
    for ident, action in [(3, "status"), (4, "start"), (5, "stop")]:
        requests.append(dict(jsonrpc="2.0", id=ident, method="tools/call",
                             params=dict(name="ui_trace", arguments=dict(action=action))))
    result = subprocess.run([str(binary), "mcp", "serve", "--toolsets", "terminal", "--tier", "trusted"],
                            env=env, cwd=directory, input="".join(json.dumps(r) + "\n" for r in requests),
                            capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stderr[-1000:]
    replies = {r["id"]: r for r in map(json.loads, result.stdout.splitlines()) if "id" in r}
    assert any(t["name"] == "ui_trace" for t in replies[2]["result"]["tools"])
    status = replies[3]["result"]
    assert not status.get("isError"), status
    payload = json.loads(next(c["text"] for c in status["content"] if c["type"] == "text"))
    assert payload["ok"] and not payload["active"], payload
    for ident in (4, 5):
        assert "DSCO_ALLOW_WRITE" in json.dumps(replies[ident]), replies[ident]
    assert not list(Path(directory).glob("dsco-ui-*.jsonl")), "denied trace created a file"
    print("PASS: ui_trace is discoverable; status works with writes disabled; start/stop are denied by the live capability gate")
