#!/usr/bin/env python3
"""Provider-free verification of the protocol commands in the interop manifest."""

import argparse
import json
import pathlib
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    binary = str(pathlib.Path(args.binary).resolve())

    manifest_proc = subprocess.run(
        [binary, "interop", "manifest"],
        text=True,
        capture_output=True,
        timeout=20,
        check=True,
    )
    manifest = json.loads(manifest_proc.stdout)
    inbound = {entry["protocol"]: entry for entry in manifest["inbound"]}
    assert set(inbound) >= {"mcp", "acp", "headless"}

    mcp_command = inbound["mcp"]["command"]
    assert pathlib.Path(mcp_command[0]).resolve() == pathlib.Path(binary)
    requests = [
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "dsco-interop-test", "version": "1"},
            },
        },
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "ping", "params": {}},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/list", "params": {}},
    ]
    mcp = subprocess.run(
        mcp_command,
        input="".join(json.dumps(row) + "\n" for row in requests),
        text=True,
        capture_output=True,
        timeout=30,
    )
    assert mcp.returncode == 0, mcp.stderr
    replies = {
        row["id"]: row
        for line in mcp.stdout.splitlines()
        if line.strip()
        for row in [json.loads(line)]
        if "id" in row
    }
    assert replies[1]["result"]["serverInfo"]["name"] == "dsco"
    assert replies[2]["result"] == {}
    tools = replies[3]["result"]["tools"]
    assert tools and all("name" in tool and "inputSchema" in tool for tool in tools)

    acp_command = inbound["acp"]["command"]
    assert pathlib.Path(acp_command[0]).resolve() == pathlib.Path(binary)
    assert acp_command[1:] == ["acp", "serve"]

    print(
        f"agent interop protocols: MCP initialize/ping/tools-list passed "
        f"({len(tools)} tools); ACP launch contract passed"
    )


if __name__ == "__main__":
    main()
