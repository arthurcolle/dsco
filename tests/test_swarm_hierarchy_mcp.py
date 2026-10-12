#!/usr/bin/env python3
"""Real MCP/worker hierarchy proof. All inference is a loopback SSE fixture."""
import argparse
import hashlib
import http.server
import json
import os
import pathlib
import re
import tempfile
import threading
import time

from test_swarm_interop_mcp import Mcp, running, wait_for_path


class Provider(http.server.BaseHTTPRequestHandler):
    requests = []

    def log_message(self, *_args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        Provider.requests.append(request)
        match = re.search(r"hierarchy node=([A-Za-z0-9_.-]+) parent=", json.dumps(request))
        assert match, "native worker lost its hierarchy task"
        events = [
            {"type": "response.created", "response": {"id": "offline-hierarchy"}},
            {"type": "response.output_text.delta", "delta": "NATIVE_RESULT_" + match[1]},
            {"type": "response.completed", "response": {"status": "completed", "usage": {
                "input_tokens": 10, "output_tokens": 5,
                "input_tokens_details": {"cached_tokens": 0}}}},
        ]
        if "messages" in request:
            events = [
                {"choices": [{"index": 0, "delta": {"content": "NATIVE_RESULT_" + match[1]}, "finish_reason": None}]},
                {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                 "usage": {"prompt_tokens": 10, "completion_tokens": 5}},
            ]
        body = "".join("data: " + json.dumps(event) + "\n\n" for event in events).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--artifact-dir", required=True)
    args = parser.parse_args()
    binary = pathlib.Path(args.binary).resolve()
    artifact = pathlib.Path(args.artifact_dir).resolve()
    artifact.mkdir(parents=True, exist_ok=True)
    snapshots, transcript = [], []
    with tempfile.TemporaryDirectory(prefix="dsco-hierarchy-proof-") as work:
        root = pathlib.Path(work)
        adapter_source = '''#!/usr/bin/python3
import json, os, pathlib, re, sys, time
root = pathlib.Path(%r)
prompt = sys.argv[-1]
node = re.search(r'hierarchy node=([A-Za-z0-9_.-]+) parent=', prompt).group(1)
task = prompt.split('Task:\\n', 1)[1].split('Parent result', 1)[0]
record = {'node': node, 'prompt': prompt, 'argv': sys.argv[1:], 'cwd': os.getcwd(),
          'pid': os.getpid(), 'leaked_key': os.getenv('OPENAI_API_KEY'),
          'leaked_authority': os.getenv('DSCO_ALLOW_RUN')}
with (root / 'executions.jsonl').open('a') as log: log.write(json.dumps(record) + '\\n')
if 'SLOW_SELF' in task:
    (root / 'slow.ready').write_text(str(os.getpid()))
    print('EXTERNAL_PARTIAL_' + node, flush=True)
    time.sleep(30)
if 'DELAY_SELF' in task:
    (root / 'sibling.ready').write_text(str(os.getpid()))
    time.sleep(2)
if 'FAIL_SELF' in task: sys.exit(7)
if 'BINARY_SELF' in task:
    os.write(1, b'A\\x00\\xff'); sys.exit(0)
print('EXTERNAL_RESULT_' + node)
''' % work
        for adapter in ("codex", "opencode"):
            executable = root / adapter
            executable.write_text(adapter_source)
            executable.chmod(0o755)
        provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        thread = threading.Thread(target=provider.serve_forever, daemon=True)
        thread.start()
        # Deliberately never inherit operator credentials or fallback endpoints.
        env = {
            "PATH": f"{work}:/usr/bin:/bin", "HOME": work,
            "DSCO_ENV_FILE": "/dev/null", "DSCO_PRICING_OFFLINE": "1",
            "DSCO_SECURE_STORE_NO_PROMPT": "1", "DSCO_TOOLMGMT": "0",
            "DSCO_DISABLE_SHARED_HOME_OAUTH": "1", "DSCO_DISABLE_CODEX_OAUTH_DISCOVERY": "1",
            "DSCO_CHATGPT_OAUTH_TOKEN": "offline-fixture-only", "DSCO_CHATGPT_ACCOUNT_ID": "offline-fixture-account",
            "OPENAI_API_KEY": "offline-fixture-only",
            "OPENAI_API_BASE": f"http://127.0.0.1:{provider.server_port}/v1",
            "DSCO_CHATGPT_BASE_URL": f"http://127.0.0.1:{provider.server_port}/responses",
            "DSCO_NO_AUTO_SUPERVISE": "1", "DSCO_NO_SUPERVISE": "1",
            "DSCO_DISABLE_DEFAULT_FALLBACKS": "1", "DSCO_AUTO_FALLBACK": "0",
            "DSCO_DYNAMIC_FAILOVER": "0", "DSCO_AUTO_GOAL": "0", "DSCO_REQUIRE_TOOL_USE": "0",
            "DSCO_PIXEL_TUI": "0", "DSCO_MCP_HEADLESS": "0", "DSCO_GOV_MODEL": "standard",
            "DSCO_SWARM_MAX_CHILDREN": "3", "DSCO_SWARM_MAX_GROUPS": "1",
            "DSCO_MAX_TOKENS": "32", "DSCO_HARD_TURN_CEILING": "2",
            "DSCO_ALLOW_RUN": "1", "DSCO_ALLOW_CONTROL": "1", "DSCO_SWARM_INHERIT_ENV": "0",
        }
        server = Mcp(str(binary), work, env)

        def tool(expected=False, **request):
            denied, reply = server.tool(action="hierarchy", **request)
            transcript.append({"request": request, "isError": denied, "receipt": reply})
            assert denied == expected, reply
            if "schema" in reply:
                assert reply["schema"] == "dsco.swarm_hierarchy.v1", reply
                snapshots.append(reply)
            return reply

        def nodes(receipt):
            return {row["id"]: row for row in receipt["nodes"]}

        def finish(handle):
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                receipt = tool(operation="advance", hierarchy_id=handle, timeout=1)
                if receipt["complete"]:
                    return receipt
            raise AssertionError(f"hierarchy deadline; inspect {server.log_path}")

        def executions():
            return [json.loads(line) for line in (root / "executions.jsonl").read_text().splitlines()] if (root / "executions.jsonl").exists() else []

        native = {"provider": "openai-codex", "model": "gpt-6-astra"}
        tree = [
            {"id": "root", "task": "Return a concise root plan without tools.", **native},
            {"id": "manager", "parent": "root", "task": "Refine parent plan.", "executor": "interop:codex", "model": "fixture-codex"},
            {"id": "reviewer", "parent": "root", "task": "Review parent plan without tools.", **native},
            {"id": "external-leaf", "parent": "manager", "task": "Use manager context.", "executor": "interop:opencode", "model": "fixture-opencode"},
            {"id": "native-leaf", "parent": "manager", "task": "Use manager context without tools.", **native},
            {"id": "review-leaf", "parent": "reviewer", "task": "Use reviewer context.", "executor": "interop:codex"},
        ]
        try:
            for invalid in (
                [{"id": "r", "task": "x", "parent": "missing"}],
                [{"id": "r", "task": "x"}, {"id": "a", "task": "y", "parent": "b"}, {"id": "b", "task": "z", "parent": "a"}],
                [{"id": "r", "task": "x", "executor": "interop:unknown"}],
            ):
                tool(expected=True, nodes=invalid)
            planned = tool(nodes=tree, dry_run=True)
            assert not planned["submitted"] and planned["hierarchy_id"] is None and not Provider.requests and not executions()
            print("PASS: invalid trees and dry-run planning submit zero workers")
            for _ in range(2):
                denied, rejected = server.tool(action="create_executor_swarm", name="invalid-held-group", retain_until_collect=True, tasks=[])
                assert denied and "malformed or empty tasks" in rejected["error"], rejected

            held = tool(name="collection-hold", nodes=[{"id": "held", "task": "finish quickly", "executor": "interop:codex"}])
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                denied, status = server.tool(action="status", group_id=held["group_id"], group_generation=held["group_generation"])
                assert not denied, status
                if status["status"]["complete"]:
                    break
                time.sleep(.02)
            else:
                raise AssertionError("collection hold readiness deadline")
            assert status["status"]["retain_until_collect"], status
            before = len(executions())
            denied, rejected = server.tool(action="create_executor_swarm", name="competing-group", tasks=[{"task": "never run", "executor": "interop:codex"}])
            assert denied and "max groups" in rejected["error"] and len(executions()) == before, rejected
            assert finish(held["hierarchy_id"])["outcome"] == "success"
            print("PASS: uncollected hierarchy wave holds its group against competing capacity recycling")

            launch = tool(nodes=tree, name="mixed-hierarchy", max_concurrency=3,
                          max_worker_turns=1, max_tokens=32, run_timeout_ms=10000, cwd=work)
            handle = launch["hierarchy_id"]
            assert nodes(launch)["root"]["status"] == "running" and launch["wave"] == 1
            first = tool(operation="advance", hierarchy_id=handle, timeout=2)
            root_ref = nodes(first)["root"].copy()
            assert root_ref["status"] == "done" and nodes(first)["manager"]["status"] == "running", first
            receipt = finish(handle)
            rows = nodes(receipt)
            assert receipt["outcome"] == "success" and receipt["wave"] == 3 and receipt["logical_depth"] == 3, receipt
            assert len(rows) == 6 and all(row["status"] == "done" for row in rows.values())
            assert len({row["run_id"] for row in rows.values()}) == 6
            assert len({row["group_generation"] for row in rows.values()}) == 3 and {row["group_id"] for row in rows.values()} == {0}
            assert rows["root"]["run_id"] == root_ref["run_id"]
            root_output = tool(operation="status", hierarchy_id=handle, node_id="root")["nodes"][0]
            assert "NATIVE_RESULT_root" in root_output["output"] and root_output["run_id"] == root_ref["run_id"]
            events = executions()
            manager = next(event for event in events if event["node"] == "manager")
            leaf = next(event for event in events if event["node"] == "external-leaf")
            assert "NATIVE_RESULT_root" in manager["prompt"] and "EXTERNAL_RESULT_manager" in leaf["prompt"]
            assert "fixture-codex" in manager["argv"] and "fixture-opencode" in leaf["argv"]
            assert all(event["leaked_key"] is None and event["leaked_authority"] is None and
                       pathlib.Path(event["cwd"]).resolve() == root.resolve() for event in events), events
            assert len(Provider.requests) == 3 and all(request["model"] == native["model"] for request in Provider.requests)
            native_leaf_request = next(request for request in Provider.requests if "hierarchy node=native-leaf" in json.dumps(request))
            assert "EXTERNAL_RESULT_manager" in json.dumps(native_leaf_request)
            assert "  manager [done]" in receipt["tree"] and "    external-leaf [done]" in receipt["tree"]
            print("PASS: six nodes, three levels, native/Codex/OpenCode; parent context and route pins survive slot/group recycling")

            token_root = tool(name="native-token-cap", max_worker_turns=1, max_tokens=32, nodes=[
                {"id": "token-root", "task": "Return a concise answer without tools.", "provider": "openai", "model": "gpt54"}])
            assert finish(token_root["hierarchy_id"])["outcome"] == "success"
            api_request = Provider.requests[-1]
            assert api_request.get("max_completion_tokens", api_request.get("max_tokens")) == 32, api_request
            print("PASS: native API hierarchy worker receives the configured output-token ceiling")

            binary_tree = tool(name="binary-parent-context", nodes=[
                {"id": "binary-root", "task": "BINARY_SELF", "executor": "interop:codex"},
                {"id": "binary-leaf", "parent": "binary-root", "task": "Use encoded data as context.", "executor": "interop:opencode"},
            ])
            assert finish(binary_tree["hierarchy_id"])["outcome"] == "success"
            binary_parent = tool(operation="status", hierarchy_id=binary_tree["hierarchy_id"], node_id="binary-root")["nodes"][0]
            assert binary_parent["output_encoding"] == "base64" and binary_parent["output"] == "QQD/", binary_parent
            binary_leaf = next(event for event in executions() if event["node"] == "binary-leaf")
            assert '"encoding":"base64"' in binary_leaf["prompt"] and "QQD/" in binary_leaf["prompt"]
            print("PASS: binary parent output reaches child context as exact, explicitly encoded base64")

            count = len(executions())
            failed = tool(name="failure-isolation", nodes=[
                {"id": "fr", "task": "root", "executor": "interop:codex"},
                {"id": "bad", "parent": "fr", "task": "FAIL_SELF", "executor": "interop:opencode"},
                {"id": "good", "parent": "fr", "task": "success", "executor": "interop:codex"},
                {"id": "blocked", "parent": "bad", "task": "never run", "executor": "interop:codex"},
                {"id": "good-leaf", "parent": "good", "task": "success", "executor": "interop:opencode"},
            ])
            failure = finish(failed["hierarchy_id"])
            assert failure["outcome"] == "failed" and nodes(failure)["bad"]["status"] == "error"
            assert nodes(failure)["blocked"]["status"] == "blocked" and nodes(failure)["good-leaf"]["status"] == "done"
            assert len(executions()) == count + 4 and "blocked" not in {event["node"] for event in executions()}
            print("PASS: failed parent blocks its descendants while successful sibling branch continues")

            cancelled = tool(name="branch-cancel", run_timeout_ms=20000, nodes=[
                {"id": "cr", "task": "root", **native},
                {"id": "slow", "parent": "cr", "task": "SLOW_SELF", "executor": "interop:codex"},
                {"id": "sibling", "parent": "cr", "task": "DELAY_SELF", "executor": "interop:opencode"},
                {"id": "slow-leaf", "parent": "slow", "task": "never run", "executor": "interop:codex"},
                {"id": "sibling-leaf", "parent": "sibling", "task": "success", "executor": "interop:codex"},
            ])
            cancel_id = cancelled["hierarchy_id"]
            tool(operation="advance", hierarchy_id=cancel_id, timeout=2)
            wait_for_path(root / "slow.ready"); wait_for_path(root / "sibling.ready")
            slow_pid = int((root / "slow.ready").read_text())
            sibling_pid = int((root / "sibling.ready").read_text())
            cancel = tool(operation="abort", hierarchy_id=cancel_id, node_id="slow")
            assert nodes(cancel)["slow"]["status"] == "cancel_pending" and nodes(cancel)["slow-leaf"]["status"] == "cancelled", cancel
            assert not running(slow_pid) and running(sibling_pid), (slow_pid, sibling_pid)
            result = finish(cancel_id)
            assert result["outcome"] == "cancelled" and nodes(result)["sibling-leaf"]["status"] == "done"
            assert "slow-leaf" not in {event["node"] for event in executions()}
            print("PASS: fenced branch cancellation stops owned process and preserves sibling work")

            tool(expected=True, operation="status", hierarchy_id="expired-controller-handle")
            tool(expected=True, operation="advance", hierarchy_id=handle, node_id="root")
            tool(expected=True, operation="status", hierarchy_id=handle, node_id="missing")
            denied_env = dict(env, DSCO_ALLOW_RUN="0")
            denied_server = Mcp(str(binary), work, denied_env)
            try:
                before = len(executions())
                denied, error = denied_server.tool(action="hierarchy", nodes=[{"id": "noexec", "task": "never run", "executor": "interop:codex"}])
                assert denied and len(executions()) == before, error
                # A handle never authorizes cross-controller access.
                denied, error = denied_server.tool(action="hierarchy", operation="status", hierarchy_id=handle)
                assert denied, error
            finally:
                denied_server.close()
            print("PASS: stale handles, invalid controls, and explicit execution denial create no workers")
            (artifact / "hierarchy-receipts.json").write_text(json.dumps(snapshots, indent=2) + "\n")
            (artifact / "mcp-transcript.json").write_text(json.dumps(transcript, indent=2) + "\n")
            (artifact / "fixture-executions.json").write_text(json.dumps(executions(), indent=2) + "\n")
            (artifact / "native-requests.json").write_text(json.dumps(Provider.requests, indent=2) + "\n")
            (artifact / "summary.json").write_text(json.dumps({
                "passed": True, "provider_inference": False, "native_mock_requests": len(Provider.requests),
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "mixed_nodes": 6, "mixed_levels": 3, "mixed_max_concurrency": 3,
                "failure_isolation": True, "branch_cancellation": True,
                "group_recycling": True, "authority_denial": True,
                "collection_hold": True,
                "native_api_token_ceiling": 32,
                "binary_parent_context": True,
                "codex_subscription_token_ceiling": "not emitted by existing provider adapter",
            }, indent=2) + "\n")
        finally:
            server.close()
            provider.shutdown(); provider.server_close(); thread.join(timeout=2)
            (artifact / "mcp.stderr").write_bytes(server.log_path.read_bytes())
            (artifact / "fixture-executions.json").write_text(json.dumps(executions(), indent=2) + "\n")
            (artifact / "native-requests.json").write_text(json.dumps(Provider.requests, indent=2) + "\n")
            (artifact / "mcp-transcript.json").write_text(json.dumps(transcript, indent=2) + "\n")


if __name__ == "__main__":
    main()
