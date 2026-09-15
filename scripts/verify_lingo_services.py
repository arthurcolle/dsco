#!/usr/bin/env python3
"""Verify Lingo against fresh local GraphSub, Autobot and Chimera services.

Uses the actual sibling checkouts and their installed environments. No fake
service fallback, provider inference, production credentials, or existing
database is used. All owned processes stop in finally; only redacted reports
and the composed observation artifact remain.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
AUTOBOT_SOURCE = '''import asyncio, os
from tool_management.app import app
from tool_management.registry import get_registry, tool
from tool_management.tools.examples import uppercase
from tool_management.embeddings.advanced_tool_index import get_advanced_index
# The real index uses backend:name addresses. Register the existing callable
# at that address, then index its actual schema. No route/result is replaced.
tool(name="local:uppercase",description="Convert text to uppercase.",
     tags=["string","utility"],category="text")(uppercase)
metadata=get_registry().get_by_name("uppercase").to_mcp_format()
asyncio.run(get_advanced_index().index_tool(metadata,"local"))
import uvicorn
uvicorn.run(app,host="127.0.0.1",port=int(os.environ["PORT"]),log_level="warning")
'''
ROUTER_SEED = '''import json, os
from dsco_router.control_plane.db import connect
from dsco_router.auth.keys import hash_api_key
db=connect(os.environ["DSCO_ROUTER_DATABASE_URL"])
db.execute("INSERT INTO organizations(id,name) VALUES (?,?)",("lingo-local","Lingo local proof"))
db.execute("INSERT INTO projects(id,org_id,name) VALUES (?,?,?)",("lingo-local","lingo-local","Lingo local proof"))
db.execute("INSERT INTO api_keys(id,org_id,project_id,key_hash,name,scopes_json) VALUES (?,?,?,?,?,?)",
    ("lingo-local","lingo-local","lingo-local",hash_api_key(os.environ["LINGO_SEED_KEY"]),
     "Isolated planning only",json.dumps(["inference"])))
db.commit();db.close()
'''
FOUNDATION_REOPEN_SOURCE = '''local p=require("lingo.platform")
local w=p.world{id=args.snapshot.world_id}
w:load(args.snapshot)
local object=w:ref(args.address.object_id)
return {summary=w:read(args.address),address=w:address(object,args.address.field)}
'''


def child_environment() -> dict[str, str]:
    # Retain HOME's actual value; do not repurpose it as a fake sandbox.
    return {key: os.environ[key] for key in ("PATH", "HOME", "LANG", "TMPDIR") if key in os.environ}


def file_identity(path: Path) -> dict:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"path": str(path.resolve()), "sha256": digest.hexdigest(), "bytes": path.stat().st_size}


def checkout_identity(path: Path, source_files: tuple[str, ...] = ()) -> dict:
    """Record only commit/dirty status, never filenames or full git status."""
    result = {"path": str(path), "head": None, "dirty": None, "git_metadata_available": False}
    env = {**child_environment(), "GIT_OPTIONAL_LOCKS": "0"}
    try:
        head = subprocess.run(["git", "-C", str(path), "rev-parse", "--show-toplevel", "HEAD"], env=env,
                              capture_output=True, text=True, timeout=15)
        dirty = subprocess.run(["git", "-C", str(path), "status", "--porcelain=v1", "--untracked-files=normal"],
                               env=env, capture_output=True, timeout=15)
        if head.returncode == 0 and dirty.returncode == 0:
            git_root, commit = head.stdout.strip().splitlines()
            result.update(git_root=git_root, head=commit, dirty=bool(dirty.stdout), git_metadata_available=True)
    except (OSError, subprocess.TimeoutExpired):
        pass
    result["source_files"] = {name: file_identity(path / name) for name in source_files}
    return result


def evidence_identity(binary: Path, graphsub: Path, autobot: Path, router: Path) -> dict:
    return {
        "provenance_note": "Binary hashes identify executed artifacts; checkout metadata is observed source state, not a compiler provenance claim.",
        "dsco": {"binary": file_identity(binary), "checkout": checkout_identity(ROOT, (
            "src/lingo.c", "src/toolmgmt.c", "src/graphsub_operator.c", "src/lingo_autobot.c",
            "src/lingo_chimera.c", "src/lingo_origin.c", "src/capability.c", "lingo/runtime.lua",
            "lingo/world_io.lua", "lingo/platform.lua", "lingo/workspace.lua", "lingo/dsco.lua",
            "lingo/operator.lua", "lingo/autobot.lua", "lingo/chimera.lua"))},
        "graphsub": {"binary": file_identity(graphsub), "checkout": checkout_identity(graphsub.parent)},
        "router": {"checkout": checkout_identity(router, (
            "src/dsco_router/routing/chimera.py", "src/dsco_router/api/openai.py",
            "src/dsco_router/routing/chimera_runtime/route.py",
            "src/dsco_router/routing/chimera_runtime/planner.py"))},
        "autobot": {"checkout": checkout_identity(autobot, (
            "src/tool_management/app.py", "src/tool_management/registry/registry.py",
            "src/tool_management/api/routes.py", "src/tool_management/tools/examples.py",
            "src/tool_management/embeddings/advanced_tool_index.py"))},
        "launcher": file_identity(Path(__file__).resolve()),
        "test_suite": file_identity(ROOT / "tests" / "test_lingo_systems.py"),
        "workflow": file_identity(ROOT / "examples" / "lingo" / "cross-system.lingo"),
        "foundation_program": file_identity(ROOT / "examples" / "lingo" / "platform-observations.lingo"),
        "foundation_reopen_source_sha256": hashlib.sha256(FOUNDATION_REOPEN_SOURCE.encode()).hexdigest(),
    }


def execution_fingerprints(identity: dict) -> dict:
    """Ignore dirty-flag changes caused by writing our own verification reports."""
    result = {}
    def walk(value):
        if not isinstance(value, dict):
            return
        if "sha256" in value:
            result[value["path"]] = value["sha256"]
        if value.get("git_metadata_available"):
            result["HEAD:" + value["git_root"]] = value["head"]
        for child in value.values():
            walk(child)
    walk(identity)
    return result


def ports(count: int) -> list[int]:
    held = []
    try:
        for _ in range(count):
            sock = socket.socket()
            sock.bind(("127.0.0.1", 0))
            held.append(sock)
        return [sock.getsockname()[1] for sock in held]
    finally:
        for sock in held:
            sock.close()


def http(url: str, body: dict | None = None) -> tuple[int, dict]:
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    # Ignore inherited machine proxy settings for this loopback-only proof.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(request, timeout=2) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as exc:
        return exc.code, {}


def wait_health(process: subprocess.Popen, url: str, kind: str) -> dict:
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"{kind} exited during startup ({process.returncode}); see redacted service log")
        try:
            status, data = http(url + "/health")
            if status == 200 and isinstance(data, dict):
                if kind in ("graphsub", "dsco-router") and data.get("service") != kind:
                    raise RuntimeError(f"Unexpected service identity on {kind} loopback port")
                return data
        except (OSError, ValueError):
            pass
        time.sleep(0.2)
    raise RuntimeError(f"{kind} did not become healthy within 60 seconds")


def verify_foundation(binary: Path, test_result: dict, report_dir: Path,
                      cwd: Path, private_values: list[str]) -> dict:
    """Consume this run's real observations, then reopen in a distinct native process."""
    program = ROOT / "examples" / "lingo" / "platform-observations.lingo"
    live = test_result["live"]
    inputs = {
        "graphsub": live["summary"]["graphsub"]["page"],
        "autobot": live["native_outputs"]["autobot_discover"],
        "chimera": live["native_outputs"]["chimera_route"],
        "recorded_at": datetime.now(timezone.utc).isoformat(),
    }
    # No service credentials enter the pure-calculation subprocesses. A hard
    # NET opt-out and a zero-tool-call assertion independently constrain proof.
    env = child_environment()
    env.update({"DSCO_ALLOW_RUN": "1", "DSCO_ALLOW_NET": "0", "DSCO_ALLOW_WRITE": "0",
                "DSCO_ALLOW_SECRETS": "0", "DSCO_ALLOW_CONTROL": "0"})

    def invoke(mode: str, source: str, args: dict, source_hash: str) -> dict:
        result = subprocess.run([str(binary), "lingo", mode, source,
                                 json.dumps(args, allow_nan=False, separators=(",", ":"))],
                                env=env, cwd=cwd, capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise RuntimeError("Foundation native invocation failed; no success receipt was accepted")
        receipt = json.loads(result.stdout)
        if (receipt.get("api") != "0.2" or receipt.get("engine") != "LuaJIT"
                or receipt.get("profile") != "governed-interpreter"
                or receipt.get("source_sha256") != source_hash
                or type(receipt.get("tool_calls")) is not int or receipt["tool_calls"] != 0):
            raise RuntimeError("Foundation invocation returned an unexpected identity or performed effects")
        return receipt

    def save(name: str, value: dict) -> Path:
        text = json.dumps(value, indent=2, allow_nan=False) + "\n"
        if any(secret in text for secret in private_values):
            raise RuntimeError("Credential found in foundation artifact; artifact was not retained")
        path = report_dir / name
        path.write_text(text)
        return path

    evaluated = invoke("run", str(program), inputs, file_identity(program)["sha256"])
    value = evaluated["value"]
    if (value["baseline"]["eligible"] is not True
            or value["hypothetical"]["eligible"] is not False
            or value["after"] != value["baseline"] or value["reopened"] != value["baseline"]
            or value["effects_executed"] is not False or value["graphsub_commit"] is not False):
        raise RuntimeError("Foundation scenario/restoration proof failed")
    reopened = invoke("eval", FOUNDATION_REOPEN_SOURCE,
                      {"snapshot": value["snapshot"], "address": value["address"]},
                      hashlib.sha256(FOUNDATION_REOPEN_SOURCE.encode()).hexdigest())
    if (reopened["value"]["summary"] != value["baseline"]
            or reopened["value"]["address"] != value["address"]):
        raise RuntimeError("Fresh native process did not reconstruct the same value and address")

    artifacts = {
        "input": save("foundation-input.json", inputs),
        "program_receipt": save("foundation-program.json", evaluated),
        "snapshot": save("foundation-snapshot.json", value["snapshot"]),
        "address": save("foundation-address.json", value["address"]),
        "reopen_receipt": save("foundation-reopened.json", reopened),
    }
    proof = {
        "status": "PASS", "native_api": "0.2", "native_processes": 2, "tool_calls": 0,
        "inputs": "Fresh genuine GraphSub, Autobot and Chimera responses captured by this verifier run",
        "semantics": "Recorded independent observations; local advisory calculation, no GraphSub commit",
        "program_source_sha256": evaluated["source_sha256"],
        "reopen_source_sha256": reopened["source_sha256"],
        "definition_sha256": value["address"]["definition_sha256"],
        "scenario_restored": True, "fresh_process_value_equal": True,
        "fresh_process_address_equal": True,
        "artifacts": {name: file_identity(path) for name, path in artifacts.items()},
    }
    save("foundation-verification.json", proof)
    return proof


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "dsco")
    parser.add_argument("--autobot", type=Path, default=ROOT.parent / "dsco-autobot" / "ToolManagement")
    parser.add_argument("--router", type=Path, default=ROOT.parent / "dsco-router")
    parser.add_argument("--graphsub-binary", type=Path,
                        default=ROOT.parent / "dsco-graphsub-complex" / "dsco-graphsub" / "target" / "release" / "graphsub")
    parser.add_argument("--report-dir", type=Path, default=ROOT / "reports" / "lingo-systems-20260909")
    options = parser.parse_args()
    options.report_dir = options.report_dir.resolve()
    options.report_dir.mkdir(parents=True, exist_ok=True)
    report = {"status": "RUNNING", "started_at": datetime.now(timezone.utc).isoformat(),
              "scope": "Actual isolated local services; no provider inference or production state",
              "processes_stopped": False, "services": {}}
    owned: list[tuple[str, subprocess.Popen, object, Path]] = []
    private_values: list[str] = []

    def redact(text: str) -> str:
        for value in private_values:
            text = text.replace(value, "[REDACTED EPHEMERAL CREDENTIAL]")
        return text

    def start(name: str, command: list[str], cwd: Path, env: dict[str, str]) -> subprocess.Popen:
        log_path = cwd / (name + ".log")
        log = log_path.open("w")
        try:
            process = subprocess.Popen(command, cwd=cwd, env=env, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
        except BaseException:
            log.close()
            raise
        owned.append((name, process, log, log_path))
        return process

    with tempfile.TemporaryDirectory(prefix="dsco-lingo-services-") as temporary:
        run_dir = Path(temporary)
        try:
            binary = options.binary.resolve(strict=True)
            graphsub = options.graphsub_binary.resolve(strict=True)
            autobot = options.autobot.resolve(strict=True)
            router = options.router.resolve(strict=True)
            # Keep venv executable paths: resolving their symlinks bypasses the venv.
            auto_python = autobot / ".venv" / "bin" / "python"
            router_python = router / ".venv" / "bin" / "python"
            for executable in (binary, graphsub, auto_python, router_python):
                if not executable.is_file() or not os.access(executable, os.X_OK):
                    raise RuntimeError(f"Required built executable or environment missing: {executable}")
            report["artifact_identity"] = evidence_identity(binary, graphsub, autobot, router)
            graph_port, graph_tcp, auto_port, router_port = ports(4)
            graph_url, auto_url, router_url = [f"http://127.0.0.1:{p}" for p in (graph_port, auto_port, router_port)]
            graph_dir, auto_dir, router_dir = [run_dir / name for name in ("graphsub", "autobot", "router")]
            for directory in (graph_dir, auto_dir, router_dir):
                directory.mkdir(mode=0o700)
            auto_token, router_token = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
            auto_secret, router_secret = secrets.token_hex(32), secrets.token_hex(32)
            private_values.extend((auto_token, router_token, auto_secret, router_secret))

            auto_env = child_environment()
            auto_env.update({"PYTHONPATH": str(autobot / "src") + os.pathsep + str(autobot),
                "PYTHONDONTWRITEBYTECODE": "1", "DATA_DIR": str(auto_dir / "data"),
                "HOST": "127.0.0.1", "PORT": str(auto_port), "REQUIRE_BEARER_AUTH": "1",
                "DEFAULT_BEARER_TOKEN": auto_token, "TOKEN_SECRET": auto_secret,
                "AUTO_START_QUEUE": "0", "AUTO_START_NATIVE_MCPS": "0",
                "REGISTER_PERSISTENT_BACKENDS": "0", "ENABLE_BG_TASKS": "0",
                "ENABLE_BACKEND_WARM_POOL": "0", "SYNC_ON_STARTUP": "0", "ALLOW_ANON_ANNOUNCE": "0"})
            bootstrap = auto_dir / "serve.py"
            bootstrap.write_text(AUTOBOT_SOURCE)

            router_env = child_environment()
            router_env.update({"PYTHONPATH": str(router / "src"), "PYTHONDONTWRITEBYTECODE": "1",
                "DSCO_ROUTER_DATABASE_URL": "sqlite:///" + str(router_dir / "router.db"),
                "DSCO_ROUTER_FEEDBACK_DB": str(router_dir / "feedback.db"),
                "DSCO_ROUTER_CATALOG_PATH": str(router_dir / "catalog.json"),
                "DSCO_ROUTER_SESSION_SECRET": router_secret, "DSCO_ROUTER_REQUIRE_AUTH": "1",
                "DSCO_ROUTER_COOKIE_SECURE": "1", "DSCO_ROUTER_RATE_LIMIT_PER_MINUTE": "1000"})
            seed_env = {**router_env, "LINGO_SEED_KEY": router_token}
            seeded = subprocess.run([str(router_python), "-c", ROUTER_SEED], cwd=router_dir,
                                    env=seed_env, capture_output=True, text=True, timeout=30)
            seed_env.pop("LINGO_SEED_KEY", None)
            if seeded.returncode:
                raise RuntimeError("Actual Router auth bootstrap failed: " + redact(seeded.stderr[-4000:]))

            graph_process = start("graphsub", [str(graphsub), "start", "--bind", f"127.0.0.1:{graph_tcp}",
                "--http", f"127.0.0.1:{graph_port}", "--persist", str(graph_dir / "data"),
                "--memory-max", "64MB", "--shard-count", "1"], graph_dir, child_environment())
            auto_process = start("autobot", [str(auto_python), str(bootstrap)], auto_dir, auto_env)
            router_process = start("router", [str(router_python), "-m", "uvicorn", "dsco_router.app:app",
                "--host", "127.0.0.1", "--port", str(router_port), "--log-level", "warning"], router_dir, router_env)
            for name, process, url, identity in (("graphsub", graph_process, graph_url, "graphsub"),
                    ("autobot", auto_process, auto_url, "autobot"),
                    ("router", router_process, router_url, "dsco-router")):
                health = wait_health(process, url, identity)
                report["services"][name] = {"url": url, "health": health}
            auto_status, _ = http(auto_url + "/api/v1/discover/search?query=uppercase&limit=1")
            router_status, _ = http(router_url + "/v1/route", {})
            if auto_status != 401 or router_status != 401:
                raise RuntimeError(f"Isolated auth is not enforced: Autobot={auto_status}, Router={router_status}")
            report["unauthenticated_status"] = {"autobot": auto_status, "router": router_status}
            report["graphsub_auth"] = "Native loopback-only engine; authenticated platform gateway is not started"

            child_env = child_environment()
            child_env.update({"LINGO_SYSTEMS_LIVE_AUTOBOT_TOKEN": auto_token,
                              "LINGO_SYSTEMS_LIVE_CHIMERA_TOKEN": router_token,
                              "PYTHONDONTWRITEBYTECODE": "1"})
            test_report = options.report_dir / "systems-services-tests.json"
            command = [sys.executable, str(ROOT / "tests" / "test_lingo_systems.py"), str(binary),
                "--report", str(test_report), "--live-graphsub-url", graph_url,
                "--live-autobot-url", auto_url, "--live-chimera-url", router_url,
                "--live-autobot-tool", "local:uppercase", "--live-autobot-query", "uppercase"]
            tested = subprocess.run(command, cwd=ROOT, env=child_env, capture_output=True,
                                    text=True, timeout=180)
            (options.report_dir / "systems-services-tests.log").write_text(redact(tested.stdout + tested.stderr))
            if test_report.exists():
                test_report.write_text(redact(test_report.read_text()))
                test_result = json.loads(test_report.read_text())
                report["tests"] = {"exit_code": tested.returncode, "status": test_result["status"],
                    "passed": len(test_result["passed"]), "failed": test_result["failed"],
                    "live": test_result["live"], "report": str(test_report)}
            if tested.returncode or not test_report.exists():
                raise RuntimeError("Cross-system verification failed; see systems-services-tests.json/log")
            if report["tests"]["live"].get("status") != "PASS":
                raise RuntimeError("Verification did not prove the actual service workflow")
            report["foundation"] = verify_foundation(binary, test_result, options.report_dir,
                                                     run_dir, private_values)
            counts = {}
            with sqlite3.connect(f"file:{router_dir / 'router.db'}?mode=ro", uri=True) as db:
                for table in ("requests", "provider_credentials", "credit_holds", "credit_ledger", "fabric_runs", "fabric_attempts"):
                    counts[table] = db.execute(f'SELECT count(*) FROM "{table}"').fetchone()[0]
            report["router_execution_rows"] = counts
            if any(counts.values()):
                raise RuntimeError("Decision-only proof unexpectedly created execution or billing records")
            artifact = options.report_dir / "cross-system-live.json"
            if artifact.exists():
                text = artifact.read_text()
                if any(value in text for value in private_values):
                    artifact.write_text(redact(text))
                    raise RuntimeError("Unexpected credential appeared in the artifact and was redacted")
            final_identity = evidence_identity(binary, graphsub, autobot, router)
            report["observed_artifact_inputs_unchanged"] = (
                execution_fingerprints(final_identity) == execution_fingerprints(report["artifact_identity"]))
            if not report["observed_artifact_inputs_unchanged"]:
                raise RuntimeError("Built artifacts or observed source inputs changed during verification")
            report["status"] = "PASS"
        except BaseException as exc:
            report["status"] = "FAIL"
            report["error"] = redact(str(exc))
        finally:
            cleanup_errors = []
            for name, process, log, log_path in reversed(owned):
                try:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM)
                        try:
                            process.wait(timeout=8)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait(timeout=5)
                except (OSError, subprocess.TimeoutExpired) as exc:
                    cleanup_errors.append(name + ": " + str(exc))
                finally:
                    log.close()
                    if log_path.exists():
                        (options.report_dir / (name + "-isolated.log")).write_text(redact(log_path.read_text(errors="replace")))
            report["processes_stopped"] = all(process.poll() is not None for _, process, _, _ in owned)
            if cleanup_errors:
                report["cleanup_errors"] = cleanup_errors
                report["status"] = "FAIL"
            report["finished_at"] = datetime.now(timezone.utc).isoformat()
            summary = options.report_dir / "services-verification.json"
            summary.write_text(redact(json.dumps(report, indent=2)) + "\n")
    print(json.dumps({"status": report["status"], "processes_stopped": report["processes_stopped"],
                      "report": str(options.report_dir / "services-verification.json")}))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
