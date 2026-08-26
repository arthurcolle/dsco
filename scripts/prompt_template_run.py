#!/usr/bin/env python3
"""Execute and grade a versioned DSCO prompt-template suite."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import re
import subprocess
import time
from typing import Any

TOKEN = re.compile(r"{{\s*([A-Za-z_][A-Za-z0-9_]*)\s*}}")
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def render(template: str, variables: dict[str, Any]) -> str:
    referenced = set(TOKEN.findall(template))
    missing = sorted(referenced - variables.keys())
    if missing:
        raise ValueError("missing template variables: " + ", ".join(missing))
    extra = sorted(variables.keys() - referenced)
    if extra:
        raise ValueError("unused template variables: " + ", ".join(extra))
    for key in referenced:
        if not isinstance(variables[key], (str, int, float, bool)):
            raise ValueError(f"variable {key!r} must be scalar")
    return TOKEN.sub(lambda m: str(variables[m.group(1)]), template)


def grade(response: str, process_exit: int, graders: list[dict[str, Any]]) -> tuple[bool, list[dict[str, Any]]]:
    clean = ANSI.sub("", response)
    rows = []
    for g in graders:
        kind = g.get("type")
        if kind == "exit_code":
            actual: Any = process_exit
            passed = actual == int(g["equals"])
        elif kind == "not_contains":
            actual = g["value"] in clean
            passed = not actual
        elif kind == "contains":
            actual = g["value"] in clean
            passed = actual
        elif kind == "regex":
            actual = bool(re.search(g["pattern"], clean, re.I | re.S))
            passed = actual
        elif kind == "max_bytes":
            actual = len(response.encode())
            passed = actual <= int(g["value"])
        elif kind == "no_tool_calls":
            actual = bool(re.search(r"(?:tool_call|\[tool\]|►\s+\w+)", clean, re.I))
            passed = not actual
        else:
            raise ValueError(f"unknown grader type: {kind}")
        rows.append({"type": kind, "passed": passed, "actual": actual})
    return all(x["passed"] for x in rows), rows


def validate_suite(suite: dict[str, Any]) -> None:
    if suite.get("schema") != "dsco.prompt-template-suite.v1":
        raise ValueError("schema must be dsco.prompt-template-suite.v1")
    if not isinstance(suite.get("templates"), list) or not suite["templates"]:
        raise ValueError("templates must be a non-empty array")
    target_ids = set()
    for target in suite.get("targets", []):
        if not isinstance(target, dict) or not target.get("id") or not target.get("provider") or not target.get("model"):
            raise ValueError("each target requires id, provider, and model")
        if target["id"] in target_ids:
            raise ValueError(f"duplicate target id: {target['id']}")
        target_ids.add(target["id"])
    ids = set()
    for item in suite["templates"]:
        for key in ("id", "version", "system", "user", "variables", "graders"):
            if key not in item:
                raise ValueError(f"template missing {key}")
        if item["id"] in ids:
            raise ValueError(f"duplicate template id: {item['id']}")
        ids.add(item["id"])
        if not isinstance(item["variables"], dict) or not isinstance(item["graders"], list):
            raise ValueError(f"invalid variables/graders for {item['id']}")
        # Variables are shared across role templates; require every declared
        # value to be consumed by at least one role and every token to resolve.
        system_refs = set(TOKEN.findall(item["system"]))
        user_refs = set(TOKEN.findall(item["user"]))
        refs = system_refs | user_refs
        missing = sorted(refs - item["variables"].keys())
        extra = sorted(item["variables"].keys() - refs)
        if missing:
            raise ValueError(f"missing variables for {item['id']}: {', '.join(missing)}")
        if extra:
            raise ValueError(f"unused variables for {item['id']}: {', '.join(extra)}")
        render(item["system"], {k: item["variables"][k] for k in system_refs})
        render(item["user"], {k: item["variables"][k] for k in user_refs})


def execute(root: pathlib.Path, suite_defaults: dict[str, Any], item: dict[str, Any], run_dir: pathlib.Path,
            target: dict[str, Any] | None = None) -> dict[str, Any]:
    started = time.time()
    variables = item["variables"]
    system_refs = set(TOKEN.findall(item["system"]))
    user_refs = set(TOKEN.findall(item["user"]))
    system = render(item["system"], {k: variables[k] for k in system_refs})
    user = render(item["user"], {k: variables[k] for k in user_refs})
    cfg = {**suite_defaults, **(target or {}), **item.get("execution", {})}
    model = cfg.get("model", "openai-codex/gpt-5.6-sol")
    provider = cfg.get("provider", "openai-codex")
    timeout = int(cfg.get("timeout", 300))
    tools = bool(cfg.get("tools", False))
    target_id = (target or {}).get("id", "default")
    case_dir = run_dir / target_id / item["id"]
    case_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["DSCO_SYSTEM_PROMPT"] = system
    if not tools:
        env["DSCO_DISABLE_ALL_TOOLS"] = "1"
    command = [str(root / "dsco"), "--profile", "worker", "--sandboxed", "--provider", provider,
               "--model", model, "--effort", str(cfg.get("effort", "low")), "-p", user]
    try:
        proc = subprocess.run(command, cwd=root, env=env, text=True, capture_output=True, timeout=timeout)
        exit_code, stdout, stderr, timed_out = proc.returncode, proc.stdout, proc.stderr, False
    except subprocess.TimeoutExpired as exc:
        exit_code, timed_out = 124, True
        stdout = exc.stdout.decode(errors="replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        stderr = exc.stderr.decode(errors="replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
    passed, grades = grade(stdout, exit_code, item["graders"])
    (case_dir / "response.txt").write_text(stdout)
    (case_dir / "stderr.txt").write_text(stderr)
    # Rendered prompts stay local to the run for reproducibility. Reports expose only digests.
    (case_dir / "system.txt").write_text(system)
    (case_dir / "user.txt").write_text(user)
    result = {"id": item["id"], "version": item["version"], "target_id": target_id,
              "passed": passed and not timed_out, "provider": provider, "model": model,
              "process_exit": exit_code, "timed_out": timed_out,
              "elapsed_ms": round((time.time() - started) * 1000, 1), "tools_enabled": tools,
              "system_sha256": digest(system.encode()), "user_sha256": digest(user.encode()),
              "response_sha256": digest(stdout.encode()), "response_bytes": len(stdout.encode()), "graders": grades}
    (case_dir / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("suite", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--parallel", type=int, default=1)
    ap.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1])
    args = ap.parse_args()
    suite_bytes = args.suite.read_bytes()
    suite = json.loads(suite_bytes)
    validate_suite(suite)
    root, run_dir = args.root.resolve(), args.out.resolve()
    run_dir.mkdir(parents=True, exist_ok=True)
    targets = suite.get("targets") or [{"id": "default"}]
    jobs = [(item, target) for target in targets for item in suite["templates"]]
    def worker(job: tuple[dict[str, Any], dict[str, Any]]) -> dict[str, Any]:
        item, target = job
        return execute(root, suite.get("defaults", {}), item, run_dir, target)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(args.parallel, 16))) as pool:
        results = list(pool.map(worker, jobs))
    report = {"schema": "dsco.prompt-template-report.v1", "suite": suite.get("name", args.suite.stem),
              "suite_sha256": digest(suite_bytes), "passed": all(x["passed"] for x in results),
              "summary": {"total": len(results), "passed": sum(x["passed"] for x in results),
                          "failed": sum(not x["passed"] for x in results)}, "results": results}
    (run_dir / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report["summary"], sort_keys=True))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
