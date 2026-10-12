#!/usr/bin/env python3
"""Probe installed agent harness contracts and optionally run a real model smoke."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from datetime import datetime, timezone


CONTRACTS = {
    "codex": {
        "version": ["--version"],
        "help": ["exec", "--help"],
        "tokens": ["--json", "--approve-for-me", "--ephemeral", "--skip-git-repo-check", "--model", "--cd"],
    },
    "claude-code": {
        "version": ["--version"],
        "help": ["--help"],
        "tokens": ["--print", "--output-format", "--permission-mode", "--no-session-persistence", "--verbose", "--model"],
    },
    "opencode": {
        "version": ["--version"],
        "help": ["run", "--help"],
        "tokens": ["--format", "--model", "--dir"],
        "acp_tokens": ["acp"],
    },
    "omp": {
        "version": ["--version"],
        "help": ["--help"],
        "tokens": ["--print", "--mode", "--no-session", "--approval-mode", "--model", "--cwd"],
        "acp_tokens": ["acp"],
    },
    "hermes": {
        "version": ["--version"],
        "help": ["chat", "--help"],
        "tokens": ["--query", "--model"],
    },
}


def run(argv: list[str], *, env: dict[str, str] | None = None, timeout: float = 30) -> tuple[int, str]:
    try:
        proc = subprocess.run(argv, capture_output=True, env=env, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        data = (exc.stdout or b"") + (exc.stderr or b"")
        if isinstance(data, str):
            return 124, data
        return 124, data.decode("utf-8", "replace")
    data = proc.stdout + proc.stderr
    return proc.returncode, data.decode("utf-8", "replace")


def first_line(text: str) -> str:
    return next((line.strip() for line in text.splitlines() if line.strip()), "")[:240]


def invocation_failure(output: str, stderr: str, receipt: dict) -> tuple[str, bool, str]:
    combined = f"{output}\n{stderr}".lower()
    if receipt.get("timed_out"):
        return "timeout", False, "harness invocation timed out"
    if "oauth session expired" in combined:
        return "authentication", True, "OAuth session expired and could not be refreshed"
    if "failed to authenticate" in combined or "authentication_failed" in combined:
        return "authentication", True, "harness authentication failed"
    if "not logged in" in combined or "login required" in combined:
        return "authentication", True, "harness login required"
    if "rate limit" in combined or "rate_limit" in combined:
        return "rate_limit", True, "provider rate limit reached"
    if "insufficient credit" in combined or "billing" in combined:
        return "billing", True, "provider billing unavailable"

    candidates: list[str] = []
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        for key in ("result", "error"):
            value = event.get(key)
            if isinstance(value, str) and value.strip():
                candidates.append(value.strip())
        message = event.get("message")
        if isinstance(message, dict):
            for part in message.get("content", []):
                if isinstance(part, dict) and isinstance(part.get("text"), str):
                    candidates.append(part["text"].strip())
    diagnostic = first_line(candidates[-1] if candidates else output or stderr)
    return "child_exit", False, diagnostic or "harness exited without the marker"


def file_sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def dsco_json(binary: str, *args: str) -> dict:
    proc = subprocess.run(
        [binary, *args], text=True, capture_output=True, timeout=30
    )
    if proc.returncode != 0:
        raise RuntimeError(first_line(proc.stderr) or f"exit {proc.returncode}")
    return json.loads(proc.stdout)


def probe_adapter(binary: str, row: dict, temp_home: Path) -> dict:
    adapter = row["id"]
    contract = CONTRACTS[adapter]
    result = {
        "adapter": adapter,
        "available": bool(row.get("available")),
        "path": row.get("path", ""),
        "contract_ok": False,
    }
    if not result["available"]:
        result["error"] = "executable_not_found"
        return result

    env = os.environ.copy()
    if adapter == "omp":
        env["HOME"] = str(temp_home)
        env["PI_CODING_AGENT_DIR"] = str(temp_home / "omp-agent")

    version_rc, version_output = run([result["path"], *contract["version"]], env=env)
    help_rc, help_output = run([result["path"], *contract["help"]], env=env)
    lowered = help_output.lower()
    missing = [token for token in contract["tokens"] if token.lower() not in lowered]
    result.update(
        {
            "version": first_line(version_output),
            "version_exit": version_rc,
            "help_exit": help_rc,
            "missing_contract_tokens": missing,
        }
    )

    acp_ok = True
    if "acp_tokens" in contract:
        try:
            command = dsco_json(binary, "interop", "acp-command", adapter)
            acp_rc, acp_output = run([*command, "--help"], env=env)
            acp_missing = [
                token for token in contract["acp_tokens"]
                if token.lower() not in acp_output.lower()
            ]
            result["acp_help_exit"] = acp_rc
            result["missing_acp_tokens"] = acp_missing
            acp_ok = acp_rc == 0 and not acp_missing
        except (RuntimeError, json.JSONDecodeError, OSError) as exc:
            result["acp_error"] = str(exc)[:240]
            acp_ok = False

    result["contract_ok"] = version_rc == 0 and help_rc == 0 and not missing and acp_ok
    return result


def invoke_adapter(binary: str, adapter: str, cwd: str, timeout_ms: int) -> dict:
    marker = "DSCO_INTEROP_SMOKE_OK"
    prompt = f"Return exactly {marker}. Do not call tools."
    argv = [
        binary, "interop", "run", adapter, "--prompt", prompt, "--cwd", cwd,
        "--timeout-ms", str(timeout_ms), "--max-output", str(1024 * 1024), "--json",
    ]
    try:
        proc = subprocess.run(
            argv, text=True, capture_output=True, timeout=timeout_ms / 1000 + 30
        )
    except subprocess.TimeoutExpired:
        return {
            "adapter": adapter,
            "ok": False,
            "error": "outer_timeout",
            "failure_class": "timeout",
            "external_blocker": False,
            "diagnostic": "DSCO invocation wrapper timed out",
        }
    try:
        receipt = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return {
            "adapter": adapter,
            "ok": False,
            "exit_code": proc.returncode,
            "error": "invalid_dsco_receipt",
            "failure_class": "dsco_receipt",
            "external_blocker": False,
            "diagnostic": first_line(proc.stderr or proc.stdout),
        }
    output = receipt.get("output", "")
    marker_found = marker in output
    ok = proc.returncode == 0 and receipt.get("ok") is True and marker_found
    failure_class, external_blocker, diagnostic = ("", False, "")
    if not ok:
        failure_class, external_blocker, diagnostic = invocation_failure(
            output, proc.stderr, receipt
        )
    return {
        "adapter": adapter,
        "ok": ok,
        "exit_code": proc.returncode,
        "child_exit_code": receipt.get("exit_code"),
        "timed_out": receipt.get("timed_out", False),
        "truncated": receipt.get("truncated", False),
        "marker_found": marker_found,
        "output_bytes": len(output.encode("utf-8", "replace")),
        "output_sha256": hashlib.sha256(output.encode("utf-8", "replace")).hexdigest(),
        "failure_class": failure_class,
        "external_blocker": external_blocker,
        "diagnostic": diagnostic,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--require-all", action="store_true")
    parser.add_argument("--invoke", action="store_true", help="run one real model smoke per available harness")
    parser.add_argument("--invoke-timeout-ms", type=int, default=180_000)
    parser.add_argument("--only", help="comma-separated adapter IDs")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1_000 <= args.invoke_timeout_ms <= 3_600_000:
        parser.error("--invoke-timeout-ms must be between 1000 and 3600000")

    binary = str(Path(args.binary).resolve())
    selected = set(args.only.split(",")) if args.only else set(CONTRACTS)
    unknown = selected - set(CONTRACTS)
    if unknown:
        parser.error("unknown adapters: " + ",".join(sorted(unknown)))

    status = dsco_json(binary, "interop", "status", "--json")
    rows = [row for row in status["adapters"] if row["id"] in selected]
    with tempfile.TemporaryDirectory(prefix="dsco-interop-probe-") as td:
        root = Path(td)
        contracts = [probe_adapter(binary, row, root / "home") for row in rows]
        invocations = []
        if args.invoke:
            workspace = root / "workspace"
            workspace.mkdir()
            invocations = [
                invoke_adapter(binary, row["id"], str(workspace), args.invoke_timeout_ms)
                for row in rows if row.get("available")
            ]

    all_available = all(row["available"] for row in contracts)
    contract_ok = all(row["contract_ok"] for row in contracts if row["available"])
    invoke_ok = not args.invoke or all(row["ok"] for row in invocations)
    ok = contract_ok and invoke_ok and (all_available or not args.require_all)
    report = {
        "schema": "dsco.agent.interop.conformance/v1",
        "observed_at": datetime.now(timezone.utc).isoformat(),
        "ok": ok,
        "dsco_binary": binary,
        "dsco_sha256": file_sha256(binary),
        "require_all": args.require_all,
        "real_invocation": args.invoke,
        "summary": {
            "adapters_selected": len(contracts),
            "adapters_available": sum(row["available"] for row in contracts),
            "contracts_passed": sum(row["contract_ok"] for row in contracts),
            "invocations_passed": sum(row["ok"] for row in invocations),
            "external_blockers": sum(
                bool(row.get("external_blocker")) for row in invocations
            ),
        },
        "contracts": contracts,
        "invocations": invocations,
    }
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        temp = args.output.with_name(args.output.name + ".tmp")
        temp.write_text(rendered)
        os.replace(temp, args.output)
    sys.stdout.write(rendered)
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
