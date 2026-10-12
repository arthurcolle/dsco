#!/usr/bin/env python3
"""Crash-resumable, budgeted harness comparison runner.

The DSCO adapter always uses the native provider path. Other harnesses are
invoked through explicit argv adapters so this benchmark never needs a shell.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any


SCHEMA = "dsco.harness_matrix.v1"
RECEIPT_PREFIX = "DSCO_HEADLESS_RECEIPT "
SENSITIVE_ENV = re.compile(r"(?:KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL|COOKIE)", re.I)
BUDGET_FIELDS = {"timeout_seconds", "grader_timeout_seconds", "max_input_tokens",
                 "max_output_tokens", "max_turns", "max_cost_usd", "max_output_bytes"}
PLACEHOLDERS = {"prompt", "workspace", "provider", "model", "effort"} | BUDGET_FIELDS
EFFORTS = {"auto", "none", "minimal", "low", "medium", "high", "xhigh", "max"}


class ManifestError(ValueError):
    pass


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        json.dump(value, handle, indent=2, sort_keys=True)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def append_jsonl(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    try:
        os.write(fd, payload.encode("utf-8"))
        os.fsync(fd)
    finally:
        os.close(fd)


def strict_keys(value: dict[str, Any], allowed: set[str], where: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise ManifestError(f"{where}: unknown fields: {', '.join(unknown)}")


def require_id(value: Any, where: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}", value):
        raise ManifestError(f"{where}: expected a stable id with at most 96 characters")
    return value


def require_positive(value: Any, where: str, maximum: float) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value) or value <= 0 or value > maximum:
        raise ManifestError(f"{where}: expected a positive number no greater than {maximum}")
    return float(value)


def require_int(value: Any, where: str, minimum: int, maximum: int) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum or value > maximum:
        raise ManifestError(f"{where}: expected an integer in {minimum}..{maximum}")
    return value


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ManifestError(f"cannot read manifest: {exc}") from exc
    if not isinstance(value, dict):
        raise ManifestError("manifest must be a JSON object")
    strict_keys(value, {"schema", "name", "defaults", "fleet_mesh", "adapters", "tasks"}, "manifest")
    if value.get("schema") != SCHEMA:
        raise ManifestError(f"manifest.schema must be {SCHEMA!r}")
    require_id(value.get("name"), "manifest.name")
    defaults = value.get("defaults", {})
    if not isinstance(defaults, dict):
        raise ManifestError("defaults must be an object")
    strict_keys(defaults, {
        "timeout_seconds", "grader_timeout_seconds", "max_input_tokens",
        "max_output_tokens", "max_turns", "max_cost_usd", "max_output_bytes",
    }, "defaults")
    defaults["timeout_seconds"] = require_positive(defaults.get("timeout_seconds", 300), "defaults.timeout_seconds", 86400)
    defaults["grader_timeout_seconds"] = require_positive(defaults.get("grader_timeout_seconds", 60), "defaults.grader_timeout_seconds", 3600)
    defaults["max_input_tokens"] = require_int(defaults.get("max_input_tokens", 32768), "defaults.max_input_tokens", 512, 2_000_000)
    defaults["max_output_tokens"] = require_int(defaults.get("max_output_tokens", 4096), "defaults.max_output_tokens", 1, 100_000)
    defaults["max_turns"] = require_int(defaults.get("max_turns", 24), "defaults.max_turns", 1, 100_000)
    defaults["max_cost_usd"] = require_positive(defaults.get("max_cost_usd", 2.0), "defaults.max_cost_usd", 10_000)
    defaults["max_output_bytes"] = require_int(defaults.get("max_output_bytes", 16 * 1024 * 1024), "defaults.max_output_bytes", 1024, 1024 * 1024 * 1024)
    value["defaults"] = defaults

    fleet = value.get("fleet_mesh")
    if fleet is not None:
        if not isinstance(fleet, dict):
            raise ManifestError("fleet_mesh must be an object")
        strict_keys(fleet, {"python", "harness", "targets", "transport", "timeout_seconds", "min_ready_nodes"}, "fleet_mesh")
        for key in ("python", "harness"):
            if not isinstance(fleet.get(key), str) or not fleet[key]:
                raise ManifestError(f"fleet_mesh.{key} must be a nonempty string")
        targets = fleet.get("targets")
        if not isinstance(targets, list) or not targets or not all(isinstance(item, str) for item in targets):
            raise ManifestError("fleet_mesh.targets must be a nonempty string array")
        for index, target in enumerate(targets):
            require_id(target, f"fleet_mesh.targets[{index}]")
        if len(set(targets)) != len(targets):
            raise ManifestError("fleet_mesh.targets contains duplicates")
        transport = fleet.get("transport", "auto")
        if transport not in {"auto", "lan", "tailscale"}:
            raise ManifestError("fleet_mesh.transport must be auto, lan, or tailscale")
        fleet["transport"] = transport
        fleet["timeout_seconds"] = require_positive(
            fleet.get("timeout_seconds", 20), "fleet_mesh.timeout_seconds", 300)
        fleet["min_ready_nodes"] = require_int(
            fleet.get("min_ready_nodes", len(targets)), "fleet_mesh.min_ready_nodes", 1, len(targets))

    adapters = value.get("adapters")
    tasks = value.get("tasks")
    if not isinstance(adapters, list) or not adapters:
        raise ManifestError("adapters must be a nonempty array")
    if not isinstance(tasks, list) or not tasks:
        raise ManifestError("tasks must be a nonempty array")
    adapter_ids: set[str] = set()
    for index, adapter in enumerate(adapters):
        where = f"adapters[{index}]"
        if not isinstance(adapter, dict):
            raise ManifestError(f"{where} must be an object")
        strict_keys(adapter, {
            "id", "kind", "binary", "provider", "model", "effort", "argv",
            "prompt_transport", "env", "inherit_env", "enabled", "budgets",
        }, where)
        ident = require_id(adapter.get("id"), f"{where}.id")
        if ident in adapter_ids:
            raise ManifestError(f"duplicate adapter id: {ident}")
        adapter_ids.add(ident)
        kind = adapter.get("kind")
        if kind not in {"dsco_native", "argv"}:
            raise ManifestError(f"{where}.kind must be dsco_native or argv")
        if not isinstance(adapter.get("binary"), str) or not adapter["binary"]:
            raise ManifestError(f"{where}.binary must be a nonempty string")
        if not isinstance(adapter.get("enabled", True), bool):
            raise ManifestError(f"{where}.enabled must be boolean")
        if kind == "dsco_native":
            for key in ("provider", "model", "effort"):
                if not isinstance(adapter.get(key), str) or not adapter[key]:
                    raise ManifestError(f"{where}.{key} must be a nonempty string")
            if adapter["effort"] not in EFFORTS:
                raise ManifestError(f"{where}.effort is unsupported")
            if "argv" in adapter or "prompt_transport" in adapter:
                raise ManifestError(f"{where}: dsco_native constructs its own argv")
        else:
            argv = adapter.get("argv")
            if not isinstance(argv, list) or not argv or not all(isinstance(item, str) for item in argv):
                raise ManifestError(f"{where}.argv must be a nonempty string array")
            transport = adapter.get("prompt_transport", "argv")
            if transport not in {"argv", "stdin"}:
                raise ManifestError(f"{where}.prompt_transport must be argv or stdin")
            joined = "\0".join(argv)
            unknown = sorted(set(re.findall(r"\{([A-Za-z0-9_]+)\}", joined)) - PLACEHOLDERS)
            if unknown:
                raise ManifestError(f"{where}.argv has unknown placeholders: {', '.join(unknown)}")
            if transport == "argv" and "{prompt}" not in joined:
                raise ManifestError(f"{where}.argv transport requires a {{prompt}} placeholder")
            if transport == "stdin" and "{prompt}" in joined:
                raise ManifestError(f"{where}: stdin transport must not duplicate prompt in argv")
        env = adapter.get("env", {})
        inherit = adapter.get("inherit_env", [])
        if not isinstance(env, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in env.items()):
            raise ManifestError(f"{where}.env must be a string map")
        if not isinstance(inherit, list) or not all(isinstance(item, str) for item in inherit):
            raise ManifestError(f"{where}.inherit_env must be a string array")
        sensitive_literals = sorted(key for key in env if SENSITIVE_ENV.search(key))
        if sensitive_literals:
            raise ManifestError(f"{where}.env may not contain secret-like literals; use inherit_env: {', '.join(sensitive_literals)}")
        budgets = adapter.get("budgets", {})
        if not isinstance(budgets, dict):
            raise ManifestError(f"{where}.budgets must be an object")
        strict_keys(budgets, set(defaults), f"{where}.budgets")

    task_ids: set[str] = set()
    for index, task in enumerate(tasks):
        where = f"tasks[{index}]"
        if not isinstance(task, dict):
            raise ManifestError(f"{where} must be an object")
        strict_keys(task, {"id", "prompt", "fixture", "adapters", "graders", "budgets"}, where)
        ident = require_id(task.get("id"), f"{where}.id")
        if ident in task_ids:
            raise ManifestError(f"duplicate task id: {ident}")
        task_ids.add(ident)
        if not isinstance(task.get("prompt"), str) or not task["prompt"]:
            raise ManifestError(f"{where}.prompt must be a nonempty string")
        selected = task.get("adapters", [])
        if not isinstance(selected, list) or not all(isinstance(item, str) for item in selected):
            raise ManifestError(f"{where}.adapters must be a string array")
        missing = sorted(set(selected) - adapter_ids)
        if missing:
            raise ManifestError(f"{where}.adapters references unknown adapters: {', '.join(missing)}")
        graders = task.get("graders", [])
        if not isinstance(graders, list):
            raise ManifestError(f"{where}.graders must be an array")
        for grader_index, grader in enumerate(graders):
            validate_grader(grader, f"{where}.graders[{grader_index}]")
        budgets = task.get("budgets", {})
        if not isinstance(budgets, dict):
            raise ManifestError(f"{where}.budgets must be an object")
        strict_keys(budgets, set(defaults), f"{where}.budgets")
    return value


def validate_grader(grader: Any, where: str) -> None:
    if not isinstance(grader, dict):
        raise ManifestError(f"{where} must be an object")
    kind = grader.get("type")
    common = {"type", "id"}
    if kind == "command":
        strict_keys(grader, common | {"argv", "timeout_seconds", "expected_exit_code", "max_output_bytes"}, where)
        argv = grader.get("argv")
        if not isinstance(argv, list) or not argv or not all(isinstance(item, str) for item in argv):
            raise ManifestError(f"{where}.argv must be a nonempty string array")
    elif kind == "file_exists":
        strict_keys(grader, common | {"path", "expected"}, where)
    elif kind == "regex":
        strict_keys(grader, common | {"path", "pattern", "flags"}, where)
    elif kind == "sha256":
        strict_keys(grader, common | {"path", "expected"}, where)
    else:
        raise ManifestError(f"{where}.type must be command, file_exists, regex, or sha256")
    require_id(grader.get("id"), f"{where}.id")


def merged_budgets(defaults: dict[str, Any], adapter: dict[str, Any], task: dict[str, Any]) -> dict[str, Any]:
    merged = dict(defaults)
    merged.update(adapter.get("budgets", {}))
    merged.update(task.get("budgets", {}))
    for name, maximum in {"timeout_seconds": 86400, "grader_timeout_seconds": 3600,
                          "max_cost_usd": 10000}.items():
        merged[name] = require_positive(merged[name], "budgets." + name, maximum)
    for name, limits in {"max_input_tokens": (512, 2_000_000),
                         "max_output_tokens": (1, 100_000), "max_turns": (1, 100_000),
                         "max_output_bytes": (1024, 1024**3)}.items():
        merged[name] = require_int(merged[name], "budgets." + name, *limits)
    return merged


def token_estimate(text: str) -> int:
    return (len(text.encode("utf-8")) + 3) // 4


def resolve_binary(raw: str, manifest_dir: Path) -> Path:
    candidate = Path(raw).expanduser()
    if candidate.is_absolute() or "/" in raw:
        if not candidate.is_absolute():
            candidate = manifest_dir / candidate
        candidate = candidate.resolve()
    else:
        found = shutil.which(raw)
        if not found:
            raise FileNotFoundError(raw)
        candidate = Path(found).resolve()
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        raise FileNotFoundError(str(candidate))
    return candidate


def resolve_file(raw: str, manifest_dir: Path) -> Path:
    candidate = Path(raw).expanduser()
    if not candidate.is_absolute():
        candidate = manifest_dir / candidate
    candidate = candidate.resolve()
    if not candidate.is_file():
        raise FileNotFoundError(str(candidate))
    return candidate


def binary_identity(path: Path) -> dict[str, Any]:
    version: str | None = None
    try:
        completed = subprocess.run([str(path), "--version"], stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, timeout=10, check=False)
        version = completed.stdout.strip()[:4096] or None
    except (OSError, subprocess.SubprocessError):
        pass
    return {"path": str(path), "sha256": sha256_file(path), "version": version}


def fleet_readiness(config: dict[str, Any], manifest_dir: Path, out: Path) -> dict[str, Any]:
    """Freeze an address-free Fleet Mesh execution receipt before inference."""
    python = resolve_binary(config["python"], manifest_dir)
    harness = resolve_file(config["harness"], manifest_dir)
    request = {
        "action": "fleet/readiness",
        "targets": config["targets"],
        "transport": config["transport"],
        "timeout_seconds": config["timeout_seconds"],
    }
    started = time.time()
    try:
        completed = subprocess.run(
            [str(python), str(harness)], input=json.dumps(request), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=config["timeout_seconds"] + 30, check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise ManifestError("Fleet Mesh readiness timed out") from exc
    receipt: dict[str, Any] | None = None
    for line in reversed(completed.stdout.splitlines()):
        try:
            candidate = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(candidate, dict):
            receipt = candidate
            break
    if receipt is None or receipt.get("schema") != "dsco.fleet_mesh.readiness.v1":
        detail = (completed.stderr or completed.stdout or "no JSON receipt").strip()[:2000]
        raise ManifestError(f"Fleet Mesh readiness returned no valid receipt: {detail}")
    ready = receipt.get("ready_node_ids")
    blocked = receipt.get("blocked_node_ids")
    if (not isinstance(ready, list) or not isinstance(blocked, list) or
            set(ready) | set(blocked) != set(config["targets"]) or set(ready) & set(blocked)):
        raise ManifestError("Fleet Mesh readiness did not account for every requested node")
    receipt_digest = sha256_bytes(json.dumps(receipt, sort_keys=True, separators=(",", ":")).encode())
    record = {
        "schema": "dsco.harness_matrix.fleet_readiness.v1",
        "started_at_unix": started,
        "finished_at_unix": time.time(),
        "python": binary_identity(python),
        "harness": {"path": str(harness), "sha256": sha256_file(harness)},
        "request": request,
        "process": {"exit_code": completed.returncode,
                    "stderr_bytes": len(completed.stderr.encode()),
                    "stderr_sha256": sha256_bytes(completed.stderr.encode())},
        "receipt_sha256": receipt_digest,
        "receipt": receipt,
        "minimum_ready_nodes": config["min_ready_nodes"],
        "gate_passed": len(ready) >= config["min_ready_nodes"],
    }
    stamp = f"{time.time_ns()}"
    atomic_json(out / "fleet" / f"readiness-{stamp}.json", record)
    atomic_json(out / "fleet" / "readiness.latest.json", record)
    append_jsonl(out / "events.jsonl", {"event": "fleet.readiness", **record})
    if not record["gate_passed"]:
        raise ManifestError(
            f"Fleet Mesh has {len(ready)} ready nodes; {config['min_ready_nodes']} required; "
            f"blocked: {', '.join(blocked) or 'none'}")
    return record


def workspace_snapshot(root: Path) -> dict[str, Any]:
    files: list[dict[str, Any]] = []
    total_bytes = 0
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            files.append({"path": relative, "kind": "symlink", "target": os.readlink(path)})
        elif path.is_file():
            size = path.stat().st_size
            total_bytes += size
            files.append({"path": relative, "kind": "file", "bytes": size, "sha256": sha256_file(path)})
    identity = sha256_bytes(json.dumps(files, sort_keys=True, separators=(",", ":")).encode())
    return {"sha256": identity, "file_count": sum(row["kind"] == "file" for row in files),
            "total_bytes": total_bytes, "files": files}


def workspace_delta(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    left = {row["path"]: row for row in before["files"]}
    right = {row["path"]: row for row in after["files"]}
    added = sorted(set(right) - set(left))
    removed = sorted(set(left) - set(right))
    changed = sorted(path for path in set(left) & set(right) if left[path] != right[path])
    return {"before_sha256": before["sha256"], "after_sha256": after["sha256"],
            "added": added, "removed": removed, "changed": changed}


def materialize_workspace(task: dict[str, Any], manifest_dir: Path, destination: Path) -> None:
    fixture = task.get("fixture")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if fixture:
        source = (manifest_dir / fixture).resolve() if not Path(fixture).is_absolute() else Path(fixture).resolve()
        if not source.is_dir():
            raise ManifestError(f"task {task['id']}: fixture is not a directory: {source}")
        shutil.copytree(source, destination, symlinks=True)
    else:
        destination.mkdir()


def adapter_environment(adapter: dict[str, Any], budgets: dict[str, Any], evidence_dir: Path) -> dict[str, str]:
    env: dict[str, str] = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                           "HOME": os.environ.get("HOME", ""),
                           "TMPDIR": os.environ.get("TMPDIR", "/tmp")}
    for key in adapter.get("inherit_env", []):
        if key in os.environ:
            env[key] = os.environ[key]
    env.update(adapter.get("env", {}))
    if adapter["kind"] == "dsco_native":
        env.update({
            "DSCO_NO_AUTO_INTERACTIVE": "1",
            "DSCO_MCP_HEADLESS": "0",
            "DSCO_TOOLMGMT": "0",
            "DSCO_HEADLESS_RECEIPT": "1",
            "DSCO_EFFORT": adapter["effort"],
            "DSCO_MAX_INPUT_TOKENS": str(budgets["max_input_tokens"]),
            "DSCO_MAX_TOKENS": str(budgets["max_output_tokens"]),
            "DSCO_HARD_TURN_CEILING": str(budgets["max_turns"]),
            "DSCO_BUDGET": str(budgets["max_cost_usd"]),
            "DSCO_BASELINE_DB": str(evidence_dir / "baseline.sqlite"),
            "DSCO_CHRONICLE_DIR": str(evidence_dir / "chronicle"),
        })
    return env


def render_argv(adapter: dict[str, Any], binary: Path, prompt: str, workspace: Path,
                budgets: dict[str, Any]) -> tuple[list[str], str | None]:
    if adapter["kind"] == "dsco_native":
        return [str(binary), "--profile", "worker", "--provider", adapter["provider"],
                "--model", adapter["model"], "--effort", adapter["effort"],
                "--prompt", prompt], None
    values = {"prompt": prompt, "workspace": str(workspace),
              "provider": adapter.get("provider", ""), "model": adapter.get("model", ""),
              "effort": adapter.get("effort", ""),
              **{name: str(budgets[name]) for name in BUDGET_FIELDS}}
    argv = [str(binary)] + [item.format_map(values) for item in adapter["argv"]]
    stdin = prompt if adapter.get("prompt_transport", "argv") == "stdin" else None
    return argv, stdin


def terminate_group(process: subprocess.Popen[Any]) -> bool:
    process.poll()  # Reap an exited leader before addressing its process group.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        process.wait()
        return False
    except PermissionError:
        # macOS can report EPERM for a group whose leader has just exited.
        try:
            process.wait(timeout=0.5)
            return False
        except subprocess.TimeoutExpired:
            raise
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pass
    # A cooperative parent can exit while a descendant ignores SIGTERM.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()
    return True


def process_group_rss_kib(group_id: int) -> int | None:
    """Return current resident KiB for the full process group."""
    try:
        completed = subprocess.run(["/bin/ps", "-axo", "pgid=,rss="],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, text=True, timeout=2, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    total = 0
    found = False
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) != 2:
            continue
        try:
            pgid, rss = int(fields[0]), int(fields[1])
        except ValueError:
            continue
        if pgid == group_id:
            total += max(0, rss)
            found = True
    return total if found else None


def execute(argv: list[str], cwd: Path, env: dict[str, str], timeout: float,
            max_output_bytes: int, stdout_path: Path, stderr_path: Path,
            stdin_text: str | None = None) -> dict[str, Any]:
    started_monotonic = time.monotonic()
    started_wall = time.time()
    exceeded_output = False
    timed_out = False
    peak_rss_kib: int | None = None
    next_rss_sample = 0.0
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr, tempfile.TemporaryFile() as input_file:
        # A pipe write can itself block forever before the deadline loop starts.
        if stdin_text is not None:
            input_file.write(stdin_text.encode("utf-8"))
            input_file.seek(0)
        stdin = input_file if stdin_text is not None else subprocess.DEVNULL
        process = subprocess.Popen(argv, cwd=cwd, env=env, stdin=stdin, stdout=stdout, stderr=stderr,
                                   start_new_session=True)
        try:
            while process.poll() is None:
                elapsed = time.monotonic() - started_monotonic
                if elapsed >= timeout:
                    timed_out = True
                    break
                if elapsed >= next_rss_sample:
                    observed_rss = process_group_rss_kib(process.pid)
                    if observed_rss is not None:
                        peak_rss_kib = max(peak_rss_kib or 0, observed_rss)
                    next_rss_sample = elapsed + 0.25
                if stdout.tell() + stderr.tell() > max_output_bytes:
                    exceeded_output = True
                    break
                time.sleep(0.1)
        finally:
            # Includes exceptions, interrupts, and descendants of exited parents.
            terminated = terminate_group(process)
    ended_wall = time.time()
    ended_monotonic = time.monotonic()
    stdout_bytes = stdout_path.stat().st_size
    stderr_bytes = stderr_path.stat().st_size
    exceeded_output = exceeded_output or stdout_bytes + stderr_bytes > max_output_bytes
    return {
        "exit_code": process.returncode,
        "timed_out": timed_out,
        "output_limit_exceeded": exceeded_output,
        "termination_sent": terminated,
        "started_at_unix": started_wall,
        "finished_at_unix": ended_wall,
        "wall_seconds": round(ended_monotonic - started_monotonic, 6),
        "realtime_seconds": round(ended_wall - started_wall, 6),
        "stdout_bytes": stdout_bytes,
        "stderr_bytes": stderr_bytes,
        "stdout_sha256": sha256_file(stdout_path),
        "stderr_sha256": sha256_file(stderr_path),
        "peak_process_group_rss_kib": peak_rss_kib,
    }


def parse_dsco_receipts(stderr_path: Path) -> list[dict[str, Any]]:
    receipts: list[dict[str, Any]] = []
    with stderr_path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            position = line.find(RECEIPT_PREFIX)
            if position < 0:
                continue
            try:
                value = json.loads(line[position + len(RECEIPT_PREFIX):])
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict) and value.get("schema") == "dsco.headless.response.v1":
                receipts.append(value)
    return receipts


def safe_workspace_path(workspace: Path, raw: Any, where: str) -> Path:
    if not isinstance(raw, str) or not raw or Path(raw).is_absolute():
        raise ManifestError(f"{where}: path must be relative")
    result = (workspace / raw).resolve()
    try:
        result.relative_to(workspace.resolve())
    except ValueError as exc:
        raise ManifestError(f"{where}: path escapes workspace") from exc
    return result


def run_grader(grader: dict[str, Any], workspace: Path, evidence: Path,
               defaults: dict[str, Any]) -> dict[str, Any]:
    kind = grader["type"]
    result: dict[str, Any] = {"id": grader["id"], "type": kind, "passed": False}
    if kind == "file_exists":
        path = safe_workspace_path(workspace, grader.get("path"), f"grader {grader['id']}")
        expected = grader.get("expected", True)
        if not isinstance(expected, bool):
            raise ManifestError(f"grader {grader['id']}.expected must be boolean")
        result.update({"path": str(path.relative_to(workspace)), "expected": expected,
                       "observed": path.exists(), "passed": path.exists() is expected})
        return result
    if kind == "sha256":
        path = safe_workspace_path(workspace, grader.get("path"), f"grader {grader['id']}")
        expected = grader.get("expected")
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ManifestError(f"grader {grader['id']}.expected must be a lowercase SHA-256")
        observed = sha256_file(path) if path.is_file() else None
        result.update({"path": str(path.relative_to(workspace)), "expected": expected,
                       "observed": observed, "passed": observed == expected})
        return result
    if kind == "regex":
        path = safe_workspace_path(workspace, grader.get("path"), f"grader {grader['id']}")
        pattern = grader.get("pattern")
        if not isinstance(pattern, str) or len(pattern) > 4096:
            raise ManifestError(f"grader {grader['id']}.pattern must be a bounded string")
        flags = grader.get("flags", "")
        if not isinstance(flags, str) or set(flags) - set("ims"):
            raise ManifestError(f"grader {grader['id']}.flags may contain only i, m, s")
        flag_value = (re.I if "i" in flags else 0) | (re.M if "m" in flags else 0) | (re.S if "s" in flags else 0)
        text = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
        matched = re.search(pattern, text[:16 * 1024 * 1024], flag_value) is not None
        result.update({"path": str(path.relative_to(workspace)), "matched": matched, "passed": matched})
        return result
    argv = grader["argv"]
    timeout = float(grader.get("timeout_seconds", defaults["grader_timeout_seconds"]))
    max_bytes = int(grader.get("max_output_bytes", defaults["max_output_bytes"]))
    grader_dir = evidence / "graders" / grader["id"]
    grader_dir.mkdir(parents=True, exist_ok=True)
    process = execute(argv, workspace, {"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                                       "HOME": os.environ.get("HOME", ""),
                                       "TMPDIR": os.environ.get("TMPDIR", "/tmp")},
                      timeout, max_bytes, grader_dir / "stdout.txt", grader_dir / "stderr.txt")
    expected = grader.get("expected_exit_code", 0)
    if not isinstance(expected, int):
        raise ManifestError(f"grader {grader['id']}.expected_exit_code must be an integer")
    result.update({"argv": argv, "expected_exit_code": expected, "process": process,
                   "passed": process["exit_code"] == expected and not process["timed_out"]
                             and not process["output_limit_exceeded"]})
    atomic_json(grader_dir / "result.json", result)
    return result


def summarize(out: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    terminal: list[dict[str, Any]] = []
    for path in sorted((out / "state").glob("*.json")):
        value = json.loads(path.read_text(encoding="utf-8"))
        if value.get("status") != "started":
            terminal.append(value)
    by_adapter: dict[str, dict[str, Any]] = {}
    for adapter in manifest["adapters"]:
        rows = [row for row in terminal if row.get("adapter_id") == adapter["id"]]
        if not rows:
            continue
        completed = [row for row in rows if row.get("status") == "completed"]
        passed = [row for row in completed if row.get("verified") is True]
        receipts = [receipt for row in rows for receipt in row.get("native_receipts", [])]
        ttft = [float(receipt["ttft_ms"]) for receipt in receipts
                if isinstance(receipt.get("ttft_ms"), (int, float)) and receipt["ttft_ms"] > 0]
        throughput = [float(receipt["tokens_per_second"]) for receipt in receipts
                      if isinstance(receipt.get("tokens_per_second"), (int, float))
                      and receipt["tokens_per_second"] > 0]
        latency = [float(receipt["latency_ms"]) for receipt in receipts
                   if isinstance(receipt.get("latency_ms"), (int, float))
                   and receipt["latency_ms"] >= 0]
        rss = [int(row["process"]["peak_process_group_rss_kib"]) for row in rows
               if isinstance(row.get("process", {}).get("peak_process_group_rss_kib"), int)]
        reported_costs = [float(receipt["provider_reported_usd"]) for receipt in receipts
                          if isinstance(receipt.get("provider_reported_usd"), (int, float))]
        estimated_costs = [float(receipt["estimated_inference_usd"]) for receipt in receipts
                           if isinstance(receipt.get("estimated_inference_usd"), (int, float))]
        accounted = [float(r["budget_accounted_usd"]) for r in receipts
                     if type(r.get("budget_accounted_usd")) in (int, float)
                     and math.isfinite(r["budget_accounted_usd"]) and r["budget_accounted_usd"] >= 0]
        accounting_complete = (bool(receipts) and len(accounted) == len(receipts)
                               and all(row.get("native_receipts") for row in rows))
        by_adapter[adapter["id"]] = {
            "attempts": len(rows),
            "completed": len(completed),
            "verified": len(passed),
            "effect_unknown": sum(row.get("status") == "effect_unknown" for row in rows),
            "wall_seconds_total": round(sum(float(row.get("process", {}).get("wall_seconds", 0)) for row in rows), 6),
            "peak_process_group_rss_kib": max(rss) if rss else None,
            "provider_responses": len(receipts),
            "input_tokens": sum(int(receipt.get("input_tokens", 0)) for receipt in receipts),
            "output_tokens": sum(int(receipt.get("output_tokens", 0)) for receipt in receipts),
            "reasoning_tokens": sum(int(receipt.get("reasoning_tokens", 0)) for receipt in receipts),
            "cache_read_tokens": sum(int(receipt.get("cache_read_tokens", 0)) for receipt in receipts),
            "ttft_ms_median": percentile(ttft, 0.5),
            "ttft_ms_p95": percentile(ttft, 0.95),
            "latency_ms_median": percentile(latency, 0.5),
            "decode_tokens_per_second_median": percentile(throughput, 0.5),
            "subscription_responses": sum(receipt.get("subscription_included") is True
                                           for receipt in receipts),
            "provider_reported_usd": round(sum(reported_costs), 8) if reported_costs else None,
            "estimated_inference_usd": round(sum(estimated_costs), 8) if estimated_costs else None,
            "cost_accounting_complete": accounting_complete,
            "known_budget_accounted_usd": round(sum(accounted), 8),
            "budget_accounted_usd": round(sum(accounted), 8) if accounting_complete else None,
        }
    fleet_summary = None
    fleet_latest = out / "fleet" / "readiness.latest.json"
    if fleet_latest.is_file():
        fleet_record = json.loads(fleet_latest.read_text(encoding="utf-8"))
        fleet_receipt = fleet_record.get("receipt", {})
        fleet_summary = {
            "receipt_sha256": fleet_record.get("receipt_sha256"),
            "observed_at_unix": fleet_receipt.get("observed_at_unix"),
            "requested_nodes": fleet_receipt.get("requested_nodes"),
            "ready_nodes": fleet_receipt.get("ready_nodes"),
            "ready_node_ids": fleet_receipt.get("ready_node_ids", []),
            "blocked_node_ids": fleet_receipt.get("blocked_node_ids", []),
            "all_connected": fleet_receipt.get("all_connected"),
            "gate_passed": fleet_record.get("gate_passed"),
        }
    summary = {"schema": "dsco.harness_matrix.summary.v1", "suite": manifest["name"],
               "generated_at_unix": time.time(), "attempts": len(terminal),
               "fleet_mesh": fleet_summary, "adapters": by_adapter}
    atomic_json(out / "summary.json", summary)
    return summary


def percentile(values: list[float], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return round(ordered[0], 6)
    position = (len(ordered) - 1) * quantile
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = position - lower
    return round(ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction, 6)


def attempt_id(suite: str, task: str, adapter: str, repeat: int) -> str:
    payload = f"{suite}\0{task}\0{adapter}\0{repeat}".encode()
    return hashlib.sha256(payload).hexdigest()[:24]


def fleet_reference(record: dict[str, Any] | None) -> dict[str, Any] | None:
    if record is None:
        return None
    receipt = record.get("receipt", {})
    return {
        "receipt_sha256": record.get("receipt_sha256"),
        "observed_at_unix": receipt.get("observed_at_unix"),
        "ready_node_ids": receipt.get("ready_node_ids", []),
        "blocked_node_ids": receipt.get("blocked_node_ids", []),
        "all_connected": receipt.get("all_connected"),
    }


def run_attempt(manifest: dict[str, Any], manifest_path: Path, out: Path,
                task: dict[str, Any], adapter: dict[str, Any], repeat: int,
                retry_unknown: bool, fleet_record: dict[str, Any] | None) -> dict[str, Any]:
    ident = attempt_id(manifest["name"], task["id"], adapter["id"], repeat)
    state_path = out / "state" / f"{ident}.json"
    evidence = out / "attempts" / ident
    workspace = out / "workspaces" / ident
    if state_path.exists():
        previous = json.loads(state_path.read_text(encoding="utf-8"))
        if previous.get("status") == "started":
            previous.update({"status": "effect_unknown", "recovered_at_unix": time.time(),
                             "reason": "runner stopped while child outcome was uncommitted"})
            atomic_json(state_path, previous)
            append_jsonl(out / "results.jsonl", previous)
        if previous.get("status") != "effect_unknown" or not retry_unknown:
            return previous
        ident = f"{ident}-retry-{uuid.uuid4().hex[:8]}"
        state_path = out / "state" / f"{ident}.json"
        evidence = out / "attempts" / ident
        workspace = out / "workspaces" / ident

    budgets = merged_budgets(manifest["defaults"], adapter, task)
    estimated = token_estimate(task["prompt"])
    if estimated > budgets["max_input_tokens"]:
        blocked = {"schema": "dsco.harness_matrix.attempt.v1", "attempt_id": ident,
                   "suite": manifest["name"], "task_id": task["id"], "adapter_id": adapter["id"],
                   "repeat": repeat, "status": "blocked", "reason": "prompt_budget_exceeded",
                   "prompt_sha256": sha256_bytes(task["prompt"].encode()),
                   "estimated_prompt_tokens": estimated, "budgets": budgets,
                   "fleet_mesh": fleet_reference(fleet_record)}
        atomic_json(state_path, blocked)
        append_jsonl(out / "results.jsonl", blocked)
        return blocked

    materialize_workspace(task, manifest_path.parent, workspace)
    evidence.mkdir(parents=True)
    before = workspace_snapshot(workspace)
    binary = resolve_binary(adapter["binary"], manifest_path.parent)
    identity = binary_identity(binary)
    argv, stdin_text = render_argv(adapter, binary, task["prompt"], workspace, budgets)
    argv_template = (["<binary>", "--profile", "worker", "--provider", adapter.get("provider"),
                      "--model", adapter.get("model"), "--effort", adapter.get("effort"),
                      "--prompt", "{prompt}"] if adapter["kind"] == "dsco_native"
                     else ["<binary>"] + adapter["argv"])
    template_text = "\0".join(value or "" for value in argv_template)
    child_budget_fields = (sorted(BUDGET_FIELDS - {"grader_timeout_seconds", "max_output_bytes"})
                           if adapter["kind"] == "dsco_native" else
                           sorted(name for name in BUDGET_FIELDS
                                  if "{" + name + "}" in template_text))
    invocation = {
        "schema": "dsco.harness_matrix.invocation.v1", "attempt_id": ident,
        "adapter_kind": adapter["kind"], "binary": identity,
        "argv_template": argv_template,
        "prompt_transport": "argv" if stdin_text is None else "stdin",
        "prompt_sha256": sha256_bytes(task["prompt"].encode()),
        "estimated_prompt_tokens": estimated, "budgets": budgets,
        "budget_enforcement": {
            "runner": ["timeout_seconds", "max_output_bytes", "max_input_tokens_prompt_preflight"],
            "child": child_budget_fields,
        },
        "provider": adapter.get("provider"), "model": adapter.get("model"),
        "effort": adapter.get("effort"), "inherited_env_names": adapter.get("inherit_env", []),
        "fleet_mesh": fleet_reference(fleet_record),
    }
    atomic_json(evidence / "invocation.json", invocation)
    started = {"schema": "dsco.harness_matrix.attempt.v1", "attempt_id": ident,
               "suite": manifest["name"], "task_id": task["id"], "adapter_id": adapter["id"],
               "repeat": repeat, "status": "started", "started_at_unix": time.time(),
               "prompt_sha256": invocation["prompt_sha256"], "binary": identity,
               "workspace_before_sha256": before["sha256"], "budgets": budgets,
               "fleet_mesh": fleet_reference(fleet_record)}
    atomic_json(state_path, started)
    append_jsonl(out / "events.jsonl", {"event": "attempt.started", **started})

    env = adapter_environment(adapter, budgets, evidence)
    process = execute(argv, workspace, env, budgets["timeout_seconds"], budgets["max_output_bytes"],
                      evidence / "stdout.txt", evidence / "stderr.txt", stdin_text)
    after = workspace_snapshot(workspace)
    delta = workspace_delta(before, after)
    atomic_json(evidence / "workspace.before.json", before)
    atomic_json(evidence / "workspace.after.json", after)
    atomic_json(evidence / "workspace.delta.json", delta)
    graders = [run_grader(grader, workspace, evidence, manifest["defaults"])
               for grader in task.get("graders", [])]
    receipts = parse_dsco_receipts(evidence / "stderr.txt") if adapter["kind"] == "dsco_native" else []
    process_ok = process["exit_code"] == 0 and not process["timed_out"] and not process["output_limit_exceeded"]
    verified = process_ok and bool(graders) and all(grader["passed"] for grader in graders)
    status = "completed"
    effect_state = "confirmed" if process_ok else ("unknown" if process["termination_sent"] else "failed")
    result = {**started, "status": status, "finished_at_unix": time.time(), "process": process,
              "effect_state": effect_state, "workspace": delta, "graders": graders,
              "verified": verified, "verification_coverage": "graded" if graders else "unverified",
              "native_receipts": receipts}
    atomic_json(evidence / "result.json", result)
    atomic_json(state_path, result)
    append_jsonl(out / "results.jsonl", result)
    append_jsonl(out / "events.jsonl", {"event": "attempt.finished", **result})
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--adapter", action="append", dest="adapters")
    parser.add_argument("--task", action="append", dest="tasks")
    parser.add_argument("--retry-unknown", action="store_true",
                        help="create a new attempt for an effect-unknown interrupted run")
    parser.add_argument("--list", action="store_true", help="validate and print the selected matrix")
    args = parser.parse_args()
    if args.rounds < 1 or args.rounds > 100:
        parser.error("--rounds must be in 1..100")
    try:
        manifest_path = args.manifest.expanduser().resolve()
        manifest = load_manifest(manifest_path)
        selected_adapters = [adapter for adapter in manifest["adapters"]
                             if ((args.adapters and adapter["id"] in args.adapters) or
                                 (not args.adapters and adapter.get("enabled", True)))]
        selected_tasks = [task for task in manifest["tasks"]
                          if (not args.tasks or task["id"] in args.tasks)]
        if args.adapters and set(args.adapters) - {adapter["id"] for adapter in manifest["adapters"]}:
            raise ManifestError("--adapter contains an unknown adapter id")
        if args.tasks and set(args.tasks) - {task["id"] for task in manifest["tasks"]}:
            raise ManifestError("--task contains an unknown task id")
        if not selected_adapters or not selected_tasks:
            raise ManifestError("selection produced an empty matrix")
        matrix = {"schema": "dsco.harness_matrix.selection.v1", "suite": manifest["name"],
                  "adapters": [adapter["id"] for adapter in selected_adapters],
                  "tasks": [task["id"] for task in selected_tasks], "rounds": args.rounds,
                  "attempts": len(selected_adapters) * len(selected_tasks) * args.rounds,
                  "fleet_mesh": ({"targets": manifest["fleet_mesh"]["targets"],
                                  "min_ready_nodes": manifest["fleet_mesh"]["min_ready_nodes"]}
                                 if manifest.get("fleet_mesh") else None)}
        if args.list:
            print(json.dumps(matrix, indent=2))
            return 0
        out = args.output.expanduser().resolve()
        out.mkdir(parents=True, exist_ok=True)
        manifest_hash = sha256_file(manifest_path)
        frozen_path = out / "manifest.json"
        if frozen_path.exists() and sha256_file(frozen_path) != manifest_hash:
            raise ManifestError("output contains a different frozen manifest")
        if not frozen_path.exists():
            shutil.copyfile(manifest_path, frozen_path)
        atomic_json(out / "selection.json", matrix)
        mesh_record = (fleet_readiness(manifest["fleet_mesh"], manifest_path.parent, out)
                       if manifest.get("fleet_mesh") else None)
        for repeat in range(1, args.rounds + 1):
            for task in selected_tasks:
                allowed = set(task.get("adapters", []))
                for adapter in selected_adapters:
                    if allowed and adapter["id"] not in allowed:
                        continue
                    result = run_attempt(manifest, manifest_path, out, task, adapter, repeat,
                                         args.retry_unknown, mesh_record)
                    print(json.dumps({"attempt_id": result["attempt_id"],
                                      "task": result["task_id"], "adapter": result["adapter_id"],
                                      "status": result["status"], "verified": result.get("verified")}),
                          flush=True)
        print(json.dumps(summarize(out, manifest), indent=2))
        return 0
    except (ManifestError, OSError, subprocess.SubprocessError) as exc:
        print(json.dumps({"error": type(exc).__name__, "detail": str(exc)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
