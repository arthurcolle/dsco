#!/usr/bin/env python3
"""Real Autobot composition -> release evidence -> GraphSub -> live Lingo desk.

Creates only owned loopback services and fresh local databases. No provider
inference or production state is used. Existing validators do the actual work.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
for module_name, relative in (("services", "scripts/verify_lingo_services.py"),
                              ("session_test", "tests/test_lingo_session.py")):
    spec = importlib.util.spec_from_file_location(module_name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    globals()[module_name] = module

AUTOBOT_BOOTSTRAP = '''import os
from tool_management.persistence import get_tool_store, RegisteredTool
store=get_tool_store()
url=os.environ["LINGO_RELEASE_WORKER_URL"]
store.register_backend("lingo_release","Owned release evidence workers",url)
for name in ("collect","verify","seal"):
    schema={"type":"object","properties":{} if name=="collect" else {"run_id":{"type":"string"}},
            "required":[] if name=="collect" else ["run_id"],"additionalProperties":False}
    store.register_tool(RegisteredTool(tool_id="lingo_release:"+name,name=name,
        backend_id="lingo_release",backend_url=url,input_schema=schema,
        description="Fixed local release evidence "+name,category="release-evidence"))
from tool_management.app import app
import uvicorn
uvicorn.run(app,host="127.0.0.1",port=int(os.environ["PORT"]),log_level="warning")
'''


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(data)
    return result.hexdigest()


def request(url, body=None, token=None, timeout=5):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    request_value = urllib.request.Request(url, data=None if body is None else json.dumps(body).encode(), headers=headers)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(request_value, timeout=timeout) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


def stop(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGINT)
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "dsco")
    parser.add_argument("--report-dir", type=Path, default=ROOT / "reports/lingo-release-20260910")
    options = parser.parse_args()
    binary = options.binary.resolve(strict=True)
    # The worker's release identity is the workspace binary, not an arbitrary
    # executable supplied by a workflow. Reject ambiguous proof configuration.
    if binary != (ROOT / "dsco").resolve():
        raise ValueError("release verification requires this checkout's dsco binary")
    report_dir = options.report_dir.resolve()
    report_dir.mkdir(parents=True, exist_ok=True)
    binary_hash = digest(binary)
    program = ROOT / "examples/lingo/release-desk.lingo"
    program_hash = digest(program)
    report = {"status": "RUNNING", "started_at": datetime.now(timezone.utc).isoformat(),
              "binary_sha256": binary_hash, "program_sha256": program_hash,
              "checks": [], "processes": [], "provider_inference": False,
              "scope": "Actual isolated Autobot profile composition and native GraphSub; fixed local source validators"}
    owned = []
    private_values = []

    def save(name, value):
        text = json.dumps(value, indent=2, allow_nan=False) + "\n"
        if any(secret in text for secret in private_values):
            raise RuntimeError("credential found in candidate report")
        (report_dir / name).write_text(text)

    def check(name, condition):
        if not condition:
            raise AssertionError(name)
        report["checks"].append(name)

    with tempfile.TemporaryDirectory(prefix="lingo-release-services-") as temporary:
        folder = Path(temporary)
        try:
            worker_port, auto_port, graph_port, graph_tcp = services.ports(4)
            worker_url = f"http://127.0.0.1:{worker_port}"
            auto_url = f"http://127.0.0.1:{auto_port}"
            graph_url = f"http://127.0.0.1:{graph_port}"
            autobot = ROOT.parent / "dsco-autobot/ToolManagement"
            graphsub = ROOT.parent / "dsco-graphsub-complex/dsco-graphsub/target/release/graphsub"
            auto_dir, graph_dir = folder / "autobot", folder / "graphsub"
            auto_dir.mkdir(mode=0o700)
            graph_dir.mkdir(mode=0o700)
            token, secret = secrets.token_urlsafe(32), secrets.token_hex(32)
            private_values.extend([token, secret])
            env = services.child_environment() | {"PYTHONDONTWRITEBYTECODE": "1"}

            def start(name, command, cwd, child_env):
                log_path = report_dir / (name + ".log")
                log = log_path.open("wb")
                child = subprocess.Popen(command, cwd=cwd, env=child_env, stdout=log,
                                         stderr=subprocess.STDOUT, start_new_session=True)
                owned.append((name, child, log, log_path))
                report["processes"].append({"name": name, "pid": child.pid})
                return child

            worker = start("worker", [sys.executable, str(ROOT / "scripts/lingo_release_worker.py"),
                "--workspace", str(ROOT.parent), "--output", str(report_dir / "worker"),
                "--port", str(worker_port)], folder, env)
            services.wait_health(worker, worker_url, "lingo-release-worker")
            bootstrap = auto_dir / "serve.py"
            bootstrap.write_text(AUTOBOT_BOOTSTRAP)
            auto_env = env | {"PYTHONPATH": str(autobot / "src") + os.pathsep + str(autobot),
                "DATA_DIR": str(auto_dir / "data"), "HOST": "127.0.0.1", "PORT": str(auto_port),
                "REQUIRE_BEARER_AUTH": "1", "DEFAULT_BEARER_TOKEN": token, "TOKEN_SECRET": secret,
                "LINGO_RELEASE_WORKER_URL": worker_url, "AUTO_START_QUEUE": "0", "AUTO_START_NATIVE_MCPS": "0",
                "REGISTER_PERSISTENT_BACKENDS": "0", "ENABLE_BG_TASKS": "0", "ENABLE_BACKEND_WARM_POOL": "0",
                "SYNC_ON_STARTUP": "0", "ALLOW_ANON_ANNOUNCE": "0"}
            auto = start("autobot", [str(autobot / ".venv/bin/python"), str(bootstrap)], auto_dir, auto_env)
            services.wait_health(auto, auto_url, "autobot")
            graph_command = [str(graphsub), "start", "--bind", f"127.0.0.1:{graph_tcp}",
                "--http", f"127.0.0.1:{graph_port}", "--persist", str(graph_dir / "data"),
                "--memory-max", "64MB", "--shard-count", "1"]
            graph = start("graphsub", graph_command, graph_dir, env)
            services.wait_health(graph, graph_url, "graphsub")
            report["endpoints"] = {"worker": worker_url, "autobot": auto_url, "graphsub": graph_url}
            status, _ = request(auto_url + "/api/v1/compose/execute", {})
            check("Autobot composition requires its isolated bearer credential", status == 401)

            client_env = env | {"TOOLS_API_URL": auto_url, "TOOLS_API_TOKEN": token,
                "GRAPHSUB_HOST": graph_url, "DSCO_ENV_FILE": "/dev/null", "DSCO_PRICING_OFFLINE": "1",
                "DSCO_SECURE_STORE_NO_PROMPT": "1", "DSCO_GOV_MODEL": "standard",
                "DSCO_NO_AUTO_SUPERVISE": "1", "DSCO_TOOLMGMT": "1", "DSCO_NO_COLOR": "1"}
            key = "release-proof-" + secrets.token_hex(8)

            def invoke(arguments, source=None, changes=None, timeout=120, success=True):
                command = [str(binary), "lingo", "eval" if source else "run", source or str(program), json.dumps(arguments)]
                result = subprocess.run(command, cwd=folder, env=client_env | (changes or {}),
                                        capture_output=True, text=True, timeout=timeout)
                if (result.returncode == 0) != success:
                    # Native errors carry no credentials; retain bounded details to diagnose real integration failures.
                    raise RuntimeError("native Lingo failed: " + (result.stdout + result.stderr)[-6000:])
                return json.loads(result.stdout)

            published = invoke({"mode": "publish", "idempotency_key": key, "trace_id": key})
            save("publication.json", published)
            value = published["value"]
            check("real historical proof is rejected for the current binary", value["review"]["historical"]["status"] == "HOLD_STALE")
            check("fresh focused contracts produce an accepted release evidence decision", value["review"]["decision"]["status"] == "PASS")
            check("workflow result is published through the native GraphSub binding", value["artifact"]["sha256"] == value["publication"]["artifact"]["sha256"])
            counts = request(worker_url + "/health")[1]["counts"]
            check("Autobot invoked each actual worker once", counts == {"collect": 1, "verify": 1, "seal": 1})
            snapshot = json.loads(value["publication"]["snapshot"])
            refreshed = next(obj for obj in snapshot["objects"] if obj["id"] == "evidence:refreshed")["values"]
            sealed = refreshed["payload"]
            verification = sealed["verification"]
            # Independent evidence oracle: never trust only a workflow status string.
            required = {"three_service_contracts", "advertised_source_output_schema",
                        "router_budget_model_identity", "graphsub_crash_reconstruction"}
            check("all four named evidence obligations passed", {row["id"] for row in verification["checks"]} == required
                  and all(row["passed"] is True and row["cases"] > 0 for row in verification["checks"]))
            check("evidence uses the exact current binary and unchanged source manifest", sealed["binary_sha256"] == binary_hash
                  and sealed["source_current"] and verification["source_current"])
            check("verification kept provider execution and billing rows empty", verification["router_inference_rows"] == 0)
            check("world stores evidence and recomputes its decision", all("decision" not in obj["values"] for obj in snapshot["objects"]))
            save("world.json", snapshot)
            save("artifact.json", value["artifact"])

            replayed = invoke({"mode": "replay", "idempotency_key": key, "trace_id": key})
            save("idempotent-replay.json", replayed)
            check("same workflow/input/key reuses the completed Autobot execution", replayed["value"]["execution_id"] == value["execution_id"]
                  and replayed["value"]["idempotent_replay"] is True)
            check("idempotent replay performs no additional worker calls", request(worker_url + "/health")[1]["counts"] == counts)
            stop(auto)
            auto = start("autobot-restarted", [str(autobot / ".venv/bin/python"), str(bootstrap)], auto_dir, auto_env)
            services.wait_health(auto, auto_url, "autobot")
            reconciled = invoke({"key": key}, 'local wf=require("lingo.workflow");return wf.reconcile(args.key)')
            save("reconciled-execution.json", reconciled)
            check("original key reconciles the same durable execution after an actual Autobot restart",
                  reconciled["value"]["execution_id"] == value["execution_id"]
                  and reconciled["value"]["status"] == "completed"
                  and reconciled["value"]["request_hash"] == value["request_hash"])
            conflict = invoke({"mode": "replay", "idempotency_key": key, "trace_id": key,
                               "inputs": {"different": True}}, success=False)
            save("idempotency-conflict.json", conflict)
            check("changed input cannot reuse the original idempotency key or dispatch workers",
                  "error" in conflict and request(worker_url + "/health")[1]["counts"] == counts)
            durable = invoke({"execution_id": value["execution_id"]},
                             'local wf=require("lingo.workflow");return wf.read(args.execution_id)')
            save("durable-execution.json", durable)
            record = durable["value"]
            check("Autobot retained the three completed steps and request identity",
                  record["execution_id"] == value["execution_id"] and record["status"] == "completed"
                  and record["workflow_kind"] == "lingo.v1" and record["request_hash"] == value["request_hash"]
                  and [(step["step_key"], step["status"]) for step in record["steps"]]
                  == [("collect", "completed"), ("verify", "completed"), ("seal", "completed")])

            report["persistence_before_crash"] = request(graph_url + "/api/v1/shards/0/persistence")[1]
            os.killpg(graph.pid, signal.SIGKILL)
            check("the release artifact engine was killed without a graceful checkpoint", graph.wait(timeout=10) == -signal.SIGKILL)
            graph = start("graphsub-restarted", graph_command, graph_dir, env)
            services.wait_health(graph, graph_url, "graphsub")
            report["persistence_after_restart"] = request(graph_url + "/api/v1/shards/0/persistence")[1]
            opened = invoke({"mode": "read", "artifact": value["artifact"]}, changes={"DSCO_ALLOW_WRITE": "0"})
            save("graphsub-reopened.json", opened)
            check("new native process reconstructs this exact release artifact after GraphSub crash/restart",
                  opened["value"] == value["review"])

            desk_args = {"artifact": value["artifact"]}
            session_path = report_dir / "release.session.json"
            command = [str(binary), "lingo", "open", str(program), json.dumps(desk_args), "--json"]
            with session_test.Wire(command, folder, client_env) as wire:
                initial = wire.line()
                check("desk opens the stored world without rerunning Autobot", initial["view"]["value"]["status"] == "PASS" and initial["tool_calls"] == 1)
                comparisons = []
                for control, val, expected in ((1, "historical", "HOLD_STALE"), (1, "refreshed", "PASS"),
                                               (2, 0, "HOLD_VALIDATION_BUDGET"), (2, 120000, "PASS"),
                                               (3, 1000000000, "HOLD_AGE"), (3, 0, "PASS")):
                    reply = wire.send({"action": "set", "control": control, "value": val})
                    check("scenario control " + str(control) + " -> " + expected,
                          reply["view"]["value"]["status"] == expected and reply["command_tool_calls"] == 0)
                    comparisons.append(reply)
                saved = wire.send({"action": "save", "path": str(session_path)})
                check("save is the one explicit session write", saved["view"]["saved"] and saved["command_tool_calls"] == 1)
                wire.send({"action": "close"})
                check("interactive protocol closes cleanly", wire.process.wait(timeout=10) == 0)
                save("policy-comparisons.json", comparisons)
            command = [str(binary), "lingo", "open", str(program), "--restore", str(session_path), "--json"]
            with session_test.Wire(command, folder, client_env | {"DSCO_ALLOW_NET": "0", "DSCO_ALLOW_WRITE": "0"}) as wire:
                restored = wire.line()
                check("saved desk restores offline with no host calls", restored["view"]["value"]["status"] == "PASS"
                      and restored["tool_calls"] == 0)
                wire.send({"action": "close"})
                check("offline restored session closes cleanly", wire.process.wait(timeout=10) == 0)
                save("desk-restored.json", restored)
            check("all policy comparisons and restores performed zero worker reexecution", request(worker_url + "/health")[1]["counts"] == counts)
            check("source and binary remained stable through verification", digest(binary) == binary_hash and digest(program) == program_hash)
            report["worker_counts"] = counts
            report["execution_id"] = value["execution_id"]
            report["candidate_id"] = value["review"]["candidate_id"]
            report["artifact"] = value["artifact"]
            report["source_artifacts"] = sealed["artifacts"]
            report["status"] = "PASS"
        except BaseException as exc:
            report["status"] = "FAIL"
            report["error"] = str(exc)
        finally:
            for name, child, log, log_path in reversed(owned):
                try:
                    stop(child)
                finally:
                    log.close()
                    text = log_path.read_text(errors="replace")
                    for private in private_values:
                        text = text.replace(private, "[REDACTED EPHEMERAL CREDENTIAL]")
                    log_path.write_text(text)
            report["all_owned_processes_stopped"] = all(child.poll() is not None for _, child, _, _ in owned)
            report["finished_at"] = datetime.now(timezone.utc).isoformat()
            for private in private_values:
                if private in report.get("error", ""):
                    report["error"] = report["error"].replace(private, "[REDACTED EPHEMERAL CREDENTIAL]")
            save("verification.json", report)
    print(json.dumps({"status": report["status"], "passed": len(report["checks"]),
                      "error": report.get("error"), "report": str(report_dir / "verification.json")}, indent=2))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
