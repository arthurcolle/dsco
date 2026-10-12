#!/usr/bin/env python3
"""Offline MCP proof for mixed native and registered external swarm workers."""
import argparse
import base64
import http.server
import json
import os
import pathlib
import select
import subprocess
import tempfile
import threading
import time


class Provider(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        events = [
            {"type": "response.created", "response": {"id": "offline-interoperability"}},
            {"type": "response.output_text.delta", "delta": "NATIVE_FIXTURE_OK"},
            {"type": "response.completed", "response": {
                "status": "completed", "usage": {"input_tokens": 10, "output_tokens": 5,
                "input_tokens_details": {"cached_tokens": 0}}}},
        ]
        body = "".join("data: " + json.dumps(row) + "\n\n" for row in events).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class Mcp:
    def __init__(self, binary, cwd, env):
        self.log_path = pathlib.Path(cwd) / "mcp.stderr"
        self.log = self.log_path.open("a")
        self.proc = subprocess.Popen(
            [binary, "mcp", "serve", "--toolsets", "all", "--tier", "trusted"],
            cwd=cwd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.log, text=True,
        )
        self.counter = 0
        self.generations = {}
        self.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                 "clientInfo": {"name": "interop-fixture", "version": "1"}})

    def call(self, method, params):
        self.counter += 1
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.counter,
                                          "method": method, "params": params}) + "\n")
        self.proc.stdin.flush()
        ready, _, _ = select.select([self.proc.stdout], [], [], 50)
        assert ready, f"MCP timeout; inspect {self.log_path}"
        reply = json.loads(self.proc.stdout.readline())
        assert reply.get("id") == self.counter, reply
        return reply

    def tool(self, tool_name="swarm", auto_fence=True, **arguments):
        if auto_fence and tool_name == "swarm" and "group_id" in arguments and "group_generation" not in arguments:
            generation = self.generations.get(arguments["group_id"])
            if generation:
                arguments["group_generation"] = generation
        reply = self.call("tools/call", {"name": tool_name, "arguments": arguments})
        body = "\n".join(item.get("text", "") for item in
                         reply.get("result", {}).get("content", []))
        try:
            parsed = json.loads(body)
            if "group_id" in parsed and "group_generation" in parsed:
                self.generations[parsed["group_id"]] = parsed["group_generation"]
            return reply.get("result", {}).get("isError", False), parsed
        except json.JSONDecodeError as error:
            raise AssertionError(f"non-JSON swarm response: {body!r}; {self.log_path}") from error

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.terminate()
                self.proc.wait(timeout=5)
        self.log.close()


def wait_for_path(path, timeout=3):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(.01)
    raise AssertionError(f"fixture readiness deadline: {path}")


def running(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    state = subprocess.run(["/bin/ps", "-o", "stat=", "-p", str(pid)],
                           capture_output=True, text=True, timeout=2).stdout.strip()
    return bool(state) and not state.startswith("Z")


def check_tree_stopped(paths):
    pids = [int(path.read_text()) for path in paths]
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline and any(running(pid) for pid in pids):
        time.sleep(.02)
    try:
        assert not any(running(pid) for pid in pids), f"descendants survived: {pids}"
    finally:
        for pid in pids:
            if running(pid):
                os.kill(pid, 9)


def completed(server, root, task, **options):
    denied, launch = server.tool(action="spawn_executor", task=task,
                                 executor="interop:codex", **options)
    assert not denied, launch
    denied, collected = server.tool(action="collect", group_id=launch["group_id"], timeout=8)
    assert not denied and collected["complete"] and len(collected["results"]) == 1, collected
    row = collected["results"][0]
    path = root / ".dsco" / "sessions" / "swarm" / str(server.proc.pid) / (
        f"run-{row['run_id']}.RESULT.json")
    envelope = json.loads(path.read_text())
    assert envelope["run_id"] == launch["run_id"] == row["run_id"]
    return row, json.loads(envelope["output"]), path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    binary = str(pathlib.Path(parser.parse_args().binary).resolve())
    with tempfile.TemporaryDirectory(prefix="dsco-swarm-interop-") as root_name:
        root = pathlib.Path(root_name)
        fake = '''#!/usr/bin/python3
import json, os, pathlib, signal, subprocess, sys, time
prompt = sys.argv[-1]
if prompt.startswith('TREE_FIXTURE:'):
    prefix = prompt.split(':', 1)[1]
    grandchild = "import os,pathlib,signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); pathlib.Path(%r).write_text(str(os.getpid())); time.sleep(30)" % (prefix + '.grand')
    intermediate = "import os,pathlib,subprocess,time; child=subprocess.Popen(['/usr/bin/python3','-c',%r],start_new_session=True); pathlib.Path(%r).write_text(str(os.getpid())); child.wait()" % (grandchild, prefix + '.child')
    child = subprocess.Popen(['/usr/bin/python3', '-c', intermediate], start_new_session=True)
    while not pathlib.Path(prefix + '.grand').exists(): time.sleep(.005)
    print('TREE_READY', flush=True)
    time.sleep(30)
if prompt == 'TERM_ZERO':
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    print('PARTIAL_BEFORE_TERM', flush=True)
    time.sleep(8)
if prompt == 'BINARY_FIXTURE':
    os.write(1, b'A\\x00\\xff\\xe2'); time.sleep(.02); os.write(1, b'\\x82\\xacB'); sys.exit(0)
if prompt == 'FLOOD_FIXTURE':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    while True: os.write(1, b'\\x00\\xff' * 8192)
if prompt.startswith('NEAR:'): time.sleep(float(prompt.split(':')[1]))
if 'SLOW_FIXTURE' in prompt: time.sleep(8)
if 'FAIL_FIXTURE' in prompt: sys.exit(7)
if prompt == 'PROVIDER_ERROR_FIXTURE':
    print(json.dumps({'type':'turn.failed','error':{'message':'offline provider failure'}})); sys.exit(0)
if prompt == 'INCOMPLETE_FIXTURE':
    print(json.dumps({'type':'thread.started','thread_id':'offline'})); sys.exit(0)
print(json.dumps({'prompt': prompt, 'argv': sys.argv[1:], 'cwd': os.getcwd(),
                  'leaked_key': os.getenv('OPENAI_API_KEY'),
                  'leaked_authority': os.getenv('DSCO_ALLOW_RUN'),
                  'type':'turn.completed', 'usage':{'input_tokens':0,'output_tokens':0,'cached_input_tokens':0}}))
'''
        for name in ("codex", "opencode"):
            path = root / name
            path.write_text(fake)
            path.chmod(0o755)
        provider = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
        thread = threading.Thread(target=provider.serve_forever, daemon=True)
        thread.start()
        env = os.environ.copy()
        env.update({
            "PATH": f"{root}:/usr/bin:/bin", "HOME": root_name,
            "DSCO_ENV_FILE": "/dev/null", "DSCO_NO_AUTO_SUPERVISE": "1",
            "DSCO_NO_SUPERVISE": "1", "DSCO_PRICING_OFFLINE": "1",
            "DSCO_SECURE_STORE_NO_PROMPT": "1", "DSCO_TOOLMGMT": "0",
            "DSCO_DISABLE_SHARED_HOME_OAUTH": "1",
            "DSCO_DISABLE_CODEX_OAUTH_DISCOVERY": "1",
            "DSCO_CHATGPT_OAUTH_TOKEN": "offline-fixture-only",
            "DSCO_CHATGPT_ACCOUNT_ID": "offline-fixture-account",
            "OPENAI_API_KEY": "offline-fixture-only",
            "DSCO_CHATGPT_BASE_URL": f"http://127.0.0.1:{provider.server_port}/responses",
            "DSCO_DISABLE_DEFAULT_FALLBACKS": "1", "DSCO_AUTO_FALLBACK": "0",
            "DSCO_DYNAMIC_FAILOVER": "0", "DSCO_AUTO_GOAL": "0",
            "DSCO_REQUIRE_TOOL_USE": "0", "DSCO_PIXEL_TUI": "0",
            "DSCO_MCP_HEADLESS": "0", "DSCO_SWARM_MAX_CHILDREN": "4",
            "DSCO_MAX_TOKENS": "32", "DSCO_HARD_TURN_CEILING": "2",
            "DSCO_ALLOW_RUN": "1", "DSCO_ALLOW_CONTROL": "1",
            "DSCO_SWARM_INHERIT_ENV": "0",
        })
        server = Mcp(binary, root_name, env)
        try:
            denied, invalid = server.tool(action="spawn_executor", task="fixture",
                                          executor="interop:unknown")
            assert denied and "failed to spawn" in invalid["error"], invalid
            denied, budget = server.tool(action="spawn_executor", task="fixture",
                                         executor="interop:codex", budget=.01)
            assert denied and "no dollar budget" in budget["error"], budget

            denied, launch = server.tool(
                action="create_executor_swarm", name="offline-mixed",
                run_timeout_ms=1000,
                tasks=[
                    {"task": "EXTERNAL_OK", "executor": "interop:codex", "model": "fixture"},
                    {"task": "FAIL_FIXTURE", "executor": "interop:opencode"},
                    {"task": "SLOW_FIXTURE", "executor": "interop:codex"},
                    {"task": "Return NATIVE_FIXTURE_OK without tools.",
                     "provider": "openai-codex", "model": "gpt-6-astra"},
                ])
            assert not denied and launch["agents_spawned"] == 4, launch
            group_id = launch["group_id"]
            denied, status = server.tool(action="status", group_id=group_id)
            assert not denied and len(status["status"]["children"]) == 4, status
            denied, collected = server.tool(action="collect", group_id=group_id, timeout=25)
            assert not denied and collected["complete"], collected
            rows = {row["task"]: row for row in collected["results"]}
            assert len(rows) == 4 and len({row["run_id"] for row in rows.values()}) == 4, rows
            assert collected["lifecycle"]["shutdown_complete"], collected
            assert collected["lifecycle"]["persisted_results"] == 4, collected
            for row in rows.values():
                assert row["reaped"] and row["result_persisted"] and row["result_errno"] == 0, row
                envelope = json.loads(pathlib.Path(row["result_path"]).read_text())
                assert envelope["run_id"] == row["run_id"] and envelope["reaped"], envelope
            successful = rows["EXTERNAL_OK"]
            receipt = json.loads(successful["output"])
            assert successful["interop_outcome"] == "success", successful
            payload = json.loads(receipt["output"])
            assert successful["status"] == "done" and successful["provider"] == "codex"
            assert successful["interop_outcome"] == "success" and receipt["adapter"] == "codex"
            assert successful["est_cost_usd"] is None and successful["unpriced_responses"] == 1
            assert payload["prompt"] == "EXTERNAL_OK"
            assert payload["leaked_key"] is None and payload["leaked_authority"] is None
            assert rows["FAIL_FIXTURE"]["interop_outcome"] == "exit_error", rows
            assert rows["SLOW_FIXTURE"]["interop_outcome"] == "timeout", rows
            assert rows["Return NATIVE_FIXTURE_OK without tools."]["status"] == "done", rows
            assert "NATIVE_FIXTURE_OK" in rows["Return NATIVE_FIXTURE_OK without tools."]["output"]
            persisted = json.loads((root / ".swarm" / "latest.json").read_text())
            external = [worker for worker in persisted["workers"] if worker["executor"] == "interop"]
            assert len(external) == 3 and all(worker["billing_class"] == "unknown" for worker in external)
            assert {worker["run_id"] for worker in external} == {
                row["run_id"] for row in rows.values() if row["provider"] in ("codex", "opencode")}
            for task, outcome in (("PROVIDER_ERROR_FIXTURE", "provider_error"), ("INCOMPLETE_FIXTURE", "incomplete")):
                row, evidence, _ = completed(server, root, task, run_timeout_ms=1000)
                assert row["interop_outcome"] == outcome and row["status"] == "error", row
                assert evidence["process_outcome"] == "exit_zero" and not evidence["ok"], evidence
                assert evidence["semantic_acceptance"] == "unverified", evidence
            print("PASS: swarm retains provider-error and incomplete receipts without promoting exit zero")
            stable_path = root / ".dsco" / "sessions" / "swarm" / str(server.proc.pid) / (
                f"run-{successful['run_id']}.RESULT.json")
            assert json.loads(stable_path.read_text())["interop_outcome"] == "success"
            print("PASS: mixed native and two external adapters; identity, outcomes, and credential isolation")
            denied, slow = server.tool(action="spawn_executor", task="SLOW_FIXTURE",
                                       executor="interop:opencode", run_timeout_ms=20000)
            assert not denied and slow["run_id"] > max(row["run_id"] for row in rows.values())
            assert slow["est_cost_usd"] is None
            assert slow["agent_id"] in {row["id"] for row in rows.values()}, slow
            denied, stale = server.tool(action="abort", group_id=slow["group_id"],
                                        group_generation=launch["group_generation"], grace_ms=100)
            assert denied and "group_generation" in stale["error"], stale
            for control in ({"group_generation": launch["group_generation"]}, {},
                            {"group_generation": slow["group_generation"], "run_id": successful["run_id"]}):
                denied, stale_agent = server.tool("agent", action="kill", id=slow["agent_id"], **control)
                assert denied and ("group_generation" in stale_agent["error"] or
                                   "run_id" in stale_agent["error"]), stale_agent
            denied, aborted = server.tool(action="abort", group_id=slow["group_id"], grace_ms=100)
            assert not denied and aborted["results"][0]["status"] == "killed", aborted
            assert aborted["results"][0]["interop_outcome"] == "cancelled", aborted
            assert aborted["lifecycle"]["shutdown_complete"] and not aborted["complete"], aborted
            assert aborted["results"][0]["reaped"] and aborted["results"][0]["result_persisted"], aborted
            denied, repeated = server.tool(action="abort", group_id=slow["group_id"], grace_ms=100)
            assert not denied and repeated["lifecycle"]["shutdown_complete"], repeated
            assert repeated["cancellation"]["kill_requests"] == repeated["cancellation"]["forced_kills"] == 0, repeated
            assert json.loads(stable_path.read_text())["interop_outcome"] == "success"
            print("PASS: adapter cancellation and durable receipt after child-slot reuse")

            denied, single = server.tool(action="spawn_executor", task="SLOW_FIXTURE",
                                         executor="interop:codex", run_timeout_ms=20000)
            assert not denied, single
            for invalid_run in (None, -1, "1", 1.5):
                denied, rejected = server.tool("agent", action="kill", id=single["agent_id"],
                    group_generation=single["group_generation"], run_id=invalid_run)
                assert denied and "run_id" in rejected["error"], rejected
            denied, killed = server.tool("agent", action="kill", id=single["agent_id"],
                group_generation=single["group_generation"], run_id=single["run_id"])
            assert not denied and killed["killed"] and killed["run_id"] == single["run_id"], killed
            assert killed["reaped"] and killed["result_persisted"], killed
            denied, repeated = server.tool("agent", action="kill", id=single["agent_id"],
                group_generation=single["group_generation"], run_id=single["run_id"])
            assert not denied and repeated["reaped"] and repeated["run_id"] == single["run_id"], repeated
            denied, single_done = server.tool(action="collect", group_id=single["group_id"], timeout=8)
            assert not denied and single_done["complete"] and single_done["results"][0]["interop_outcome"] == "cancelled", single_done
            print("PASS: agent kill rejects stale/missing group identity and invalid run fences; current identity cancels")

            row, receipt, _ = completed(server, root, "TERM_ZERO", run_timeout_ms=350)
            assert receipt["timed_out"] and receipt["exit_code"] == 0 and not receipt["ok"], receipt
            assert row["interop_outcome"] == "timeout" and row["status"] == "error", row
            assert "PARTIAL_BEFORE_TERM" in receipt["output"]
            for delay in (.01, .16, .19, .25):
                row, receipt, _ = completed(server, root, f"NEAR:{delay}", run_timeout_ms=200)
                assert (row["status"] == "done") == (receipt["outcome"] == "success"), row
            print("PASS: termination-exit-zero and deadline/completion classification")

            for action in ("abort", "timeout"):
                prefix = root / f"tree-{action}"
                denied, tree = server.tool(action="spawn_executor", task=f"TREE_FIXTURE:{prefix}",
                                           executor="interop:codex", run_timeout_ms=1500)
                assert not denied, tree
                paths = [pathlib.Path(str(prefix) + suffix) for suffix in (".child", ".grand")]
                for path in paths:
                    wait_for_path(path)
                if action == "abort":
                    denied, end = server.tool(action="abort", group_id=tree["group_id"], grace_ms=100)
                    assert not denied and end["results"][0]["interop_outcome"] == "cancelled", end
                else:
                    denied, end = server.tool(action="collect", group_id=tree["group_id"], timeout=8)
                    assert not denied and end["results"][0]["interop_outcome"] == "timeout", end
                check_tree_stopped(paths)
            print("PASS: abort and timeout stop children and grandchildren in separate process groups")

            before = stable_path.read_bytes()
            server.close()
            server = Mcp(binary, root_name, env)
            reply = server.call("tools/call", {"name": "read_file", "arguments": {"path": str(stable_path)}})
            content = "\n".join(item.get("text", "") for item in reply["result"]["content"])
            assert not reply["result"].get("isError") and '"interop_outcome":"success"' in content, content
            assert stable_path.read_bytes() == before
            print("PASS: completed receipt survives DSCO restart and is readable by a new runtime")

            env["DSCO_SWARM_MAX_OUTPUT"] = "4096"
            server.close()
            server = Mcp(binary, root_name, env)
            row, receipt, _ = completed(server, root, "BINARY_FIXTURE", run_timeout_ms=1000)
            assert row["interop_outcome"] == "incomplete" and row["status"] == "error", row
            assert base64.b64decode(receipt["output_base64"]) == b"A\x00\xff\xe2\x82\xacB", receipt
            assert stable_path.read_bytes() == before, "new runtime overwrote an older receipt"
            started = time.monotonic()
            row, receipt, _ = completed(server, root, "FLOOD_FIXTURE", run_timeout_ms=200)
            assert row["interop_outcome"] == "timeout" and receipt["truncated"], row
            assert receipt["captured_bytes"] < 4096 and time.monotonic() - started < 4
            print("PASS: bounded binary/flood receipts remain valid under a 4096-byte swarm output cap")

            denied, bounded_group = server.tool(action="create_executor_swarm", name="bounded",
                budget=.01, executor="interop:codex", tasks=["EXTERNAL_OK"])
            assert denied and "unpriced" in bounded_group["error"], bounded_group
            print("PASS: new runtime preserves older receipt and rejects external dollar budgets")

            env["DSCO_SWARM_MAX_GROUPS"] = "1"
            server.close()
            server = Mcp(binary, root_name, env)
            denied, prior = server.tool(action="spawn_executor", task="EXTERNAL_OK",
                                        executor="interop:codex")
            assert not denied, prior
            denied, prior_done = server.tool(action="collect", group_id=prior["group_id"], timeout=8)
            assert not denied and prior_done["complete"], prior_done
            denied, current = server.tool(action="spawn_executor", task="SLOW_FIXTURE",
                                          executor="interop:codex", run_timeout_ms=20000)
            assert not denied and current["group_id"] == prior["group_id"], current
            assert current["group_generation"] != prior["group_generation"], current
            gid = current["group_id"]
            stale = prior["group_generation"]
            fresh = current["group_generation"]
            for arguments in ({"group_generation": stale}, {}):
                auto_fence = bool(arguments)
                denied, rejected = server.tool(action="abort", group_id=gid,
                                               auto_fence=auto_fence, **arguments)
                assert denied and "group_generation" in rejected["error"], rejected
                denied, rejected = server.tool(action="spawn_executor", group_id=gid,
                    task="EXTERNAL_OK", executor="interop:codex",
                    auto_fence=auto_fence, **arguments)
                assert denied and "group_generation" in rejected["error"], rejected
                denied, rejected = server.tool(action="collect", group_id=gid,
                                               timeout=1, auto_fence=auto_fence, **arguments)
                assert denied and "group_generation" in rejected["error"], rejected
            denied, rejected = server.tool(action="status", group_id=gid,
                                           group_generation=stale)
            assert denied and "group_generation" in rejected["error"], rejected
            denied, discovered = server.tool(action="status", group_id=gid, auto_fence=False)
            assert not denied and discovered["group_generation"] == fresh, discovered
            denied, current_status = server.tool(action="status", group_id=gid,
                                                  group_generation=fresh)
            assert not denied and len(current_status["status"]["children"]) == 1, current_status
            denied, appended = server.tool(action="spawn_executor", group_id=gid,
                group_generation=fresh, task="EXTERNAL_OK", executor="interop:codex")
            assert not denied and appended["group_generation"] == fresh, appended
            denied, stopped = server.tool(action="abort", group_id=gid,
                                          group_generation=fresh, grace_ms=100)
            assert not denied and any(row["task"] == "SLOW_FIXTURE" and
                row["interop_outcome"] == "cancelled" for row in stopped["results"]), stopped
            print("PASS: recycled group ID rejects stale and unfenced controls; current generation can spawn and abort")

            server.close()
            server = Mcp(binary, root_name, env)
            denied, restarted = server.tool(action="spawn_executor", task="SLOW_FIXTURE",
                                           executor="interop:codex", run_timeout_ms=20000)
            assert not denied and restarted["group_id"] == gid, restarted
            assert restarted["group_generation"] != fresh, restarted
            denied, rejected = server.tool(action="abort", group_id=gid,
                                           group_generation=fresh, grace_ms=100)
            assert denied and "group_generation" in rejected["error"], rejected
            denied, rejected = server.tool(action="abort", group_id=gid,
                                           auto_fence=False, grace_ms=100)
            assert denied and "group_generation" in rejected["error"], rejected
            denied, stopped = server.tool(action="abort", group_id=gid,
                group_generation=restarted["group_generation"], grace_ms=100)
            assert not denied and stopped["results"][0]["interop_outcome"] == "cancelled", stopped
            print("PASS: generation fencing also rejects a prior DSCO process token")

            env.pop("DSCO_SWARM_MAX_OUTPUT")
            server.close()
            server = Mcp(binary, root_name, env)
            project = root / "project"
            project.mkdir()
            row, receipt, _ = completed(server, root, "CWD_FIXTURE", cwd="project", run_timeout_ms=1500)
            assert json.loads(receipt["output"])["cwd"] == str(project.resolve()), receipt
            for invalid_cwd in (root / "missing", root / "codex"):
                denied, bad_cwd = server.tool(action="spawn_executor", task="CWD_FIXTURE",
                                              executor="interop:codex", cwd=str(invalid_cwd))
                assert denied, bad_cwd
            denied, group_cwd = server.tool(action="create_executor_swarm", name="cwd-group",
                cwd="project", executor="interop:codex", run_timeout_ms=1500, tasks=["GROUP_CWD_FIXTURE"])
            assert not denied and group_cwd["agents_spawned"] == 1, group_cwd
            denied, cwd_results = server.tool(action="collect", group_id=group_cwd["group_id"], timeout=8)
            assert not denied and cwd_results["complete"], cwd_results
            assert json.loads(json.loads(cwd_results["results"][0]["output"])["output"])["cwd"] == str(project.resolve())
            print("PASS: single and grouped adapters resolve cwd; missing and non-directory paths reject")

            # A missing interpreter fails spawn without ever selecting an
            # installed real provider CLI from another discovery location.
            (root / "codex").write_text("#!/definitely/missing-dsco-fixture-interpreter\n")
            try:
                row, receipt, _ = completed(server, root, "MISSING_EXEC_FIXTURE", run_timeout_ms=1500)
                assert row["status"] == "error" and row["interop_outcome"] == "spawn_error", row
                assert not receipt["ok"] and "error" in receipt, receipt
            finally:
                (root / "codex").write_text(fake)
            print("PASS: executable spawn failure produces an explicit error receipt without fallback")
        finally:
            server.close()
            provider.shutdown()
            provider.server_close()
            thread.join(timeout=3)


if __name__ == "__main__":
    main()
