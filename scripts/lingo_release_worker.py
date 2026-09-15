#!/usr/bin/env python3
"""Owned loopback MCP workers for a source-bound integration release review.

Tools have fixed source roots, validators and report destinations selected by
the host. Tool inputs cannot supply commands, executable paths or credentials.
This server is intended for an isolated local Autobot backend, not deployment.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
import uuid
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
HISTORICAL = "dsco-cli/reports/lingo-foundation-20260910/services/services-verification.json"
INPUTS = (
    "dsco-cli/dsco", "dsco-cli/src/lingo.c", "dsco-cli/src/lingo_workflow.c",
    "dsco-cli/src/toolmgmt.c", "dsco-cli/src/lingo_autobot.c", "dsco-cli/src/lingo_chimera.c",
    "dsco-cli/src/capability.c", "dsco-cli/src/service_boundary.c", "dsco-cli/src/tools.c",
    "dsco-cli/src/lingo_graphsub_world.c", "dsco-cli/lingo/runtime.lua", "dsco-cli/lingo/world_io.lua",
    "dsco-cli/lingo/workflow.lua", "dsco-cli/lingo/workspace.lua", "dsco-cli/lingo/chimera.lua",
    "dsco-cli/scripts/lingo_release_worker.py", "dsco-cli/scripts/verify_lingo_release.py",
    "dsco-cli/scripts/verify_lingo_services.py", "dsco-cli/scripts/verify_lingo_workspace.py",
    "dsco-cli/tests/test_lingo_systems.py", "dsco-cli/tests/test_lingo_systems_contract.py",
    "dsco-cli/examples/lingo/release-desk.lingo",
    "dsco-router/src/dsco_router/routing/chimera.py",
    "dsco-router/src/dsco_router/routing/plan_contract.py",
    "dsco-router/src/dsco_router/api/openai.py",
    "dsco-router/src/dsco_router/providers/transport.py",
    "dsco-router/src/dsco_router/routing/chimera_runtime/route.py",
    "dsco-router/src/dsco_router/routing/chimera_runtime/planner.py",
    "dsco-router/tests/test_chimera_plan_identity.py",
    "dsco-autobot/ToolManagement/src/tool_management/api/composition_routes.py",
    "dsco-autobot/ToolManagement/src/tool_management/api/lingo_composition.py",
    "dsco-autobot/ToolManagement/src/tool_management/persistence/tool_store.py",
    "dsco-autobot/ToolManagement/src/tool_management/persistence/execution_store.py",
    "dsco-graphsub-complex/dsco-graphsub/target/release/graphsub",
)
TOOLS = ("collect", "verify", "seal")


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def environment():
    result = {key: os.environ[key] for key in ("PATH", "HOME", "LANG", "TMPDIR") if key in os.environ}
    result.update(PYTHONDONTWRITEBYTECODE="1", DSCO_ENV_FILE="/dev/null",
                  DSCO_PRICING_OFFLINE="1", DSCO_SECURE_STORE_NO_PROMPT="1",
                  DSCO_GOV_MODEL="standard", DSCO_NO_AUTO_SUPERVISE="1", DSCO_TOOLMGMT="0")
    return result


class ReleaseWorker:
    def __init__(self, workspace, output):
        self.workspace = workspace.resolve(strict=True)
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.jobs = {}
        self.counts = {name: 0 for name in TOOLS}
        self.lock = threading.Lock()
        self.active_child = None

    def fingerprints(self):
        paths = list(INPUTS)
        assets = self.workspace / "dsco-router/src/dsco_router/routing/chimera_assets"
        manifest = json.loads((assets / "manifest.json").read_text())
        paths.append(str((assets / "manifest.json").relative_to(self.workspace)))
        for key in ("operational_artifact", "quality_artifact", "catalog_fallback", "hypercube_projection"):
            path = (assets / manifest[key]).resolve(strict=True)
            paths.append(str(path.relative_to(self.workspace)))
        return [{"path": name, "sha256": digest(self.workspace / name)} for name in sorted(set(paths))]

    def save(self, path, value):
        path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")

    def collect(self, arguments):
        if arguments:
            raise ValueError("collect accepts an empty input record; roots belong to host configuration")
        artifacts = self.fingerprints()
        candidate_id = hashlib.sha256(canonical(artifacts)).hexdigest()
        binary_hash = next(item["sha256"] for item in artifacts if item["path"] == "dsco-cli/dsco")
        historical = self.workspace / HISTORICAL
        prior = json.loads(historical.read_text())
        prior_binary = prior["artifact_identity"]["dsco"]["binary"]["sha256"]
        prior_inputs = {}
        def recorded_inputs(value):
            if isinstance(value, dict):
                if isinstance(value.get("path"), str) and isinstance(value.get("sha256"), str):
                    try:
                        relative = str(Path(value["path"]).resolve().relative_to(self.workspace))
                        prior_inputs[relative] = value["sha256"]
                    except ValueError:
                        pass
                for child in value.values():
                    recorded_inputs(child)
            elif isinstance(value, list):
                for child in value:
                    recorded_inputs(child)
        recorded_inputs(prior["artifact_identity"])
        missing = [item["path"] for item in artifacts if item["path"] not in prior_inputs]
        changed = [item["path"] for item in artifacts if item["path"] in prior_inputs
                   and prior_inputs[item["path"]] != item["sha256"]]
        baseline = {"label": "recorded integration proof", "status": prior["status"],
                    "binary_sha256": prior_binary, "candidate_sha256": binary_hash,
                    "source_current": prior_binary == binary_hash and not missing and not changed,
                    "missing_inputs": missing, "changed_inputs": changed,
                    "observed_at": prior["finished_at"], "report_sha256": digest(historical),
                    "observed_at_epoch": int(datetime.fromisoformat(prior["finished_at"]).timestamp()),
                    "report_path": HISTORICAL, "checks_passed": prior["tests"]["passed"],
                    "meaning": "Historical PASS applies to recorded inputs; missing or changed candidate hashes prevent reuse"}
        run_id = uuid.uuid4().hex
        directory = self.output / run_id
        directory.mkdir(mode=0o700)
        job = {"run_id": run_id, "candidate_id": candidate_id, "binary_sha256": binary_hash,
               "artifacts": artifacts, "baseline": baseline, "captured_at": int(time.time()),
               "directory": directory, "state": "collected"}
        self.jobs[run_id] = job
        result = {key: job[key] for key in ("run_id", "candidate_id", "binary_sha256", "artifacts", "baseline", "captured_at")}
        self.save(directory / "collection.json", result)
        return result

    def lookup(self, arguments, state):
        if set(arguments) != {"run_id"} or not isinstance(arguments["run_id"], str):
            raise ValueError("this tool requires only a collected run_id")
        job = self.jobs.get(arguments["run_id"])
        if not job or job["state"] != state:
            raise ValueError("unknown run or invalid worker state")
        if self.fingerprints() != job["artifacts"]:
            raise ValueError("candidate source or binary changed after collection")
        return job

    def run(self, command, cwd, log, timeout, env=None):
        # argv only; command and deadline are fixed by this source, never a tool input.
        with log.open("wb") as output:
            process = subprocess.Popen(command, cwd=cwd, env=env or environment(),
                                       stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
            self.active_child = process
            try:
                return process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                # Existing service verifiers catch KeyboardInterrupt and clean their
                # separately grouped children in finally before this worker returns.
                os.killpg(process.pid, signal.SIGINT)
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=5)
                raise RuntimeError("fixed validator deadline expired: " + Path(command[1]).name)
            finally:
                self.active_child = None

    def interrupt(self):
        child = self.active_child
        if child is not None and child.poll() is None:
            os.killpg(child.pid, signal.SIGINT)

    def verify(self, arguments):
        job = self.lookup(arguments, "collected")
        job["state"] = "verifying"
        directory, cli = job["directory"], self.workspace / "dsco-cli"
        binary = cli / "dsco"
        start = time.monotonic()
        checks = []
        try:
            services = directory / "services"
            code = self.run([sys.executable, str(cli / "scripts/verify_lingo_services.py"),
                             "--binary", str(binary), "--report-dir", str(services)],
                            cli, directory / "services.log", 40)
            service_report = json.loads((services / "services-verification.json").read_text())
            live_report = json.loads((services / "systems-services-tests.json").read_text())
            checks.append({"id": "three_service_contracts", "passed": code == 0 and service_report["status"] == "PASS"
                           and service_report["observed_artifact_inputs_unchanged"] and service_report["processes_stopped"],
                           "cases": service_report["tests"]["passed"],
                           "report_sha256": digest(services / "services-verification.json")})
            contracts = directory / "contracts.json"
            code = self.run(["/usr/bin/python3", str(cli / "tests/test_lingo_systems_contract.py"), str(binary),
                             "--live-report", str(services / "systems-services-tests.json"), "--report", str(contracts)],
                            cli, directory / "contracts.log", 15)
            contract = json.loads(contracts.read_text())
            checks.append({"id": "advertised_source_output_schema", "passed": code == 0 and contract["status"] == "PASS",
                           "cases": contract["passed"], "report_sha256": digest(contracts)})
            router = self.workspace / "dsco-router"
            junit = directory / "router.xml"
            router_env = environment() | {"PYTHONPATH": str(router / "src"),
                "DSCO_ROUTER_DATABASE_URL": "sqlite:///" + str(directory / "router-test.db"),
                "DSCO_ROUTER_FEEDBACK_DB": str(directory / "router-feedback.db")}
            code = self.run([str(router / ".venv/bin/python"), "-m", "pytest", "-q", "-p", "no:cacheprovider",
                             str(router / "tests/test_chimera_plan_identity.py"), "--junitxml", str(junit)],
                            directory, directory / "router.log", 15, router_env)
            cases = ET.parse(junit).getroot().findall(".//testcase")
            checks.append({"id": "router_budget_model_identity", "passed": code == 0 and len(cases) == 4
                           and not any(case.find("failure") is not None or case.find("error") is not None
                                       or case.find("skipped") is not None for case in cases),
                           "cases": len(cases), "report_sha256": digest(junit)})
            recovery_dir = directory / "recovery"
            code = self.run([sys.executable, str(cli / "scripts/verify_lingo_workspace.py"), str(binary),
                             "--report-dir", str(recovery_dir)], cli, directory / "recovery.log", 15)
            recovery_path = recovery_dir / "real-restart-proof.json"
            recovery = json.loads(recovery_path.read_text())
            checks.append({"id": "graphsub_crash_reconstruction", "passed": code == 0 and recovery["status"] == "PASS"
                           and recovery["execution_inputs_unchanged"] and recovery["all_owned_processes_stopped"],
                           "cases": len(recovery["checks"]), "report_sha256": digest(recovery_path)})
            job["verification"] = {"checks": checks, "all_passed": all(row["passed"] for row in checks),
                "duration_ms": round((time.monotonic() - start) * 1000), "observed_at": int(time.time()),
                "source_current": self.fingerprints() == job["artifacts"],
                "router_inference_rows": sum(service_report["router_execution_rows"].values()),
                "live_tool_calls": live_report["live"]["tool_calls"],
                "route": live_report["live"]["native_outputs"]["chimera_route"]}
            if not job["verification"]["all_passed"] or not job["verification"]["source_current"]:
                raise RuntimeError("focused verification failed or source inputs changed")
            job["state"] = "verified"
            result = {"run_id": job["run_id"], "candidate_id": job["candidate_id"], "verification": job["verification"]}
            self.save(directory / "verification.json", result)
            return result
        except BaseException:
            job["state"] = "failed"
            self.save(directory / "failed-checks.json", {"checks": checks})
            raise

    def seal(self, arguments):
        job = self.lookup(arguments, "verified")
        result = {"format": "lingo.release.evidence/1", "run_id": job["run_id"],
                  "candidate_id": job["candidate_id"], "binary_sha256": job["binary_sha256"],
                  "artifacts": job["artifacts"], "baseline": job["baseline"],
                  "verification": job["verification"], "sealed_at": int(time.time()),
                  "source_current": True,
                  "scope": "Observed source/binary identities and focused local contract checks; no build attestation or deployment"}
        job["state"] = "sealed"
        self.save(job["directory"] / "sealed.json", result)
        return result

    def call(self, name, arguments):
        if name not in TOOLS or type(arguments) is not dict:
            raise ValueError("unknown worker tool or invalid arguments")
        with self.lock:
            self.counts[name] += 1
            self.save(self.output / "worker-counts.json", self.counts)
            return getattr(self, name)(arguments)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=ROOT.parent)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--port", type=int, required=True)
    options = parser.parse_args()
    worker = ReleaseWorker(options.workspace, options.output)

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send(self, status, body):
            data = canonical(body)
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == "/health":
                self.send(200, {"service": "lingo-release-worker", "status": "healthy", "counts": worker.counts})
            else:
                self.send(404, {"error": "not found"})

        def do_POST(self):
            if self.path != "/mcp":
                self.send(404, {"error": "not found"})
                return
            identifier = None
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 0 < size <= 65536:
                    raise ValueError("bounded MCP request required")
                request = json.loads(self.rfile.read(size))
                identifier = request.get("id")
                if request.get("method") == "tools/list":
                    result = {"tools": [{"name": name, "description": "Fixed release evidence " + name,
                        "inputSchema": {"type": "object", "properties": {} if name == "collect" else {"run_id": {"type": "string"}},
                                        "required": [] if name == "collect" else ["run_id"], "additionalProperties": False}}
                                        for name in TOOLS]}
                elif request.get("method") == "tools/call":
                    params = request["params"]
                    value = worker.call(params["name"], params.get("arguments", {}))
                    result = {"content": [{"type": "text", "text": canonical(value).decode()}], "isError": False}
                else:
                    raise ValueError("only tools/list and tools/call are exposed")
            except Exception as exc:
                result = {"content": [{"type": "text", "text": str(exc)[:1000]}], "isError": True}
            self.send(200, {"jsonrpc": "2.0", "id": identifier, "result": result})

    server = ThreadingHTTPServer(("127.0.0.1", options.port), Handler)
    server.daemon_threads = False
    def interrupted(_number, _frame):
        worker.interrupt()
        raise KeyboardInterrupt
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
