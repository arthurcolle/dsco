#!/usr/bin/env python3
"""Run deterministic model/provider resolution simulations against dsco."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


@dataclass(frozen=True)
class SimulationCase:
    name: str
    args: List[str]
    expect: Dict[str, str]
    env: Dict[str, str] = field(default_factory=dict)


CASES: List[SimulationCase] = [
    SimulationCase(
        name="bare-opus-no-key",
        args=["--route-explain", "opus"],
        expect={
            "alias_resolved_model": "claude-opus-4-8",
            "model_family": "anthropic",
            "detected_provider": "anthropic",
            "route_provider": "anthropic",
            "fallback_active": "no",
            "auth_mode": "missing",
            "credential_present": "no",
            "credential_usable": "no",
        },
    ),
    SimulationCase(
        name="openrouter-fallback-for-bare-claude",
        args=["--route-explain", "claude-sonnet-4-6"],
        env={"OPENROUTER_API_KEY": "sk-or-router"},
        expect={
            "alias_resolved_model": "claude-sonnet-4-6",
            "model_family": "anthropic",
            "detected_provider": "anthropic",
            "route_provider": "openrouter",
            "fallback_active": "yes",
            "auth_mode": "openrouter-api-key",
            "credential_present": "yes",
            "credential_usable": "yes",
            "api_url": "https://openrouter.ai/api/v1/chat/completions",
        },
    ),
    SimulationCase(
        name="native-anthropic-namespace-refuses-openrouter-fallback",
        args=["--route-explain", "anthropic/claude-sonnet-4-6"],
        env={"OPENROUTER_API_KEY": "sk-or-router"},
        expect={
            "model_family": "anthropic",
            "detected_provider": "anthropic",
            "route_provider": "anthropic",
            "fallback_active": "no",
            "auth_mode": "missing",
            "credential_present": "no",
            "credential_usable": "no",
        },
    ),
    SimulationCase(
        name="explicit-openrouter-anthropic-wrapper",
        args=["--route-explain", "openrouter/anthropic/claude-sonnet-4-6"],
        env={"OPENROUTER_API_KEY": "sk-or-router"},
        expect={
            "model_family": "anthropic",
            "detected_provider": "openrouter",
            "route_provider": "openrouter",
            "fallback_active": "no",
            "auth_mode": "openrouter-api-key",
            "credential_present": "yes",
            "credential_usable": "yes",
        },
    ),
    SimulationCase(
        name="kimi-native-alias",
        args=["--route-explain", "kimi"],
        env={"KIMI_API_KEY": "kimi-native"},
        expect={
            "alias_resolved_model": "moonshotai/kimi-k2.7-code",
            "model_family": "moonshot",
            "detected_provider": "moonshot",
            "route_provider": "moonshot",
            "fallback_active": "no",
            "auth_mode": "api-key",
            "credential_present": "yes",
            "credential_usable": "yes",
            "api_url": "https://api.moonshot.ai/v1/chat/completions",
        },
    ),
    SimulationCase(
        name="explicit-openrouter-moonshot-wrapper",
        args=["--route-explain", "openrouter/moonshotai/kimi-k2.7-code"],
        env={"OPENROUTER_API_KEY": "sk-or-router"},
        expect={
            "model_family": "moonshot",
            "detected_provider": "openrouter",
            "route_provider": "openrouter",
            "fallback_active": "no",
            "auth_mode": "openrouter-api-key",
            "credential_present": "yes",
            "credential_usable": "yes",
        },
    ),
    SimulationCase(
        name="glm-native-alias",
        args=["--route-explain", "glm52"],
        env={"GLM_API_KEY": "glm-native"},
        expect={
            "alias_resolved_model": "zai/glm-5.2",
            "model_family": "zai",
            "detected_provider": "zai",
            "route_provider": "zai",
            "fallback_active": "no",
            "auth_mode": "api-key",
            "credential_present": "yes",
            "credential_usable": "yes",
            "api_url": "https://api.z.ai/api/coding/paas/v4/chat/completions",
        },
    ),
    SimulationCase(
        name="glm-openrouter-alias",
        args=["--route-explain", "or-glm52"],
        env={"OPENROUTER_API_KEY": "sk-or-router"},
        expect={
            "alias_resolved_model": "openrouter/z-ai/glm-5.2",
            "model_family": "zai",
            "detected_provider": "openrouter",
            "route_provider": "openrouter",
            "fallback_active": "no",
            "auth_mode": "openrouter-api-key",
            "credential_present": "yes",
            "credential_usable": "yes",
        },
    ),
    SimulationCase(
        name="z-ai-openrouter-catalog-namespace",
        args=["--route-explain", "z-ai/glm-5.2"],
        env={"OPENROUTER_API_KEY": "sk-or-router"},
        expect={
            "alias_resolved_model": "z-ai/glm-5.2",
            "model_family": "zai",
            "detected_provider": "openrouter",
            "route_provider": "openrouter",
            "fallback_active": "no",
            "auth_mode": "openrouter-api-key",
            "credential_present": "yes",
            "credential_usable": "yes",
        },
    ),
    SimulationCase(
        name="local-ollama-no-key",
        args=["--route-explain", "ollama/gpt-oss:20b"],
        expect={
            "model_family": "ollama",
            "detected_provider": "ollama",
            "route_provider": "ollama",
            "fallback_active": "no",
            "endpoint_class": "local",
            "auth_mode": "local",
            "credential_present": "no",
            "credential_usable": "yes",
        },
    ),
    SimulationCase(
        name="openai-direct-key",
        args=["--route-explain", "openai/gpt-5.5"],
        env={"OPENAI_API_KEY": "sk-openai-sim"},
        expect={
            "model_family": "openai",
            "detected_provider": "openai",
            "route_provider": "openai",
            "fallback_active": "no",
            "auth_mode": "openai-api-key",
            "credential_present": "yes",
            "credential_usable": "yes",
            "api_url": "https://api.openai.com/v1/chat/completions",
        },
    ),
    SimulationCase(
        name="override-mismatched-session-key-refused",
        args=["--provider", "openrouter", "-k", "sk-ant-session", "--route-explain", "claude-sonnet-4-6"],
        expect={
            "model_family": "anthropic",
            "detected_provider": "anthropic",
            "route_provider": "openrouter",
            "provider_override": "openrouter",
            "fallback_active": "yes",
            "auth_mode": "missing",
            "credential_present": "no",
            "credential_usable": "no",
        },
    ),
    SimulationCase(
        name="unknown-model-fallback-key-infers-openrouter",
        args=["-k", "sk-or-session", "--route-explain", "future-model-2030"],
        expect={
            "alias_resolved_model": "future-model-2030",
            "model_family": "other",
            "detected_provider": "openrouter",
            "route_provider": "openrouter",
            "fallback_active": "no",
            "auth_mode": "openrouter-api-key",
            "credential_present": "yes",
            "credential_usable": "yes",
        },
    ),
]


def parse_route_explain(output: str) -> Dict[str, str]:
    parsed: Dict[str, str] = {}
    for line in output.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        parsed[key.strip()] = value.strip()
    return parsed


def base_env(home: Path) -> Dict[str, str]:
    return {
        "HOME": str(home),
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "TERM": "dumb",
        "LC_ALL": "C",
        "DSCO_DISABLE_CODEX_OAUTH_DISCOVERY": "1",
        "DSCO_DISABLE_CLAUDE_CODE_OAUTH_DISCOVERY": "1",
        "DSCO_DISABLE_CHATGPT_NATIVE": "1",
    }


def run_case(
    dsco: Path, case: SimulationCase, env: Dict[str, str], timeout: float
) -> Tuple[bool, Dict[str, object]]:
    run_env = dict(env)
    run_env.update(case.env)
    cmd = [str(dsco)] + case.args
    proc = subprocess.run(
        cmd,
        env=run_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        check=False,
    )
    parsed = parse_route_explain(proc.stdout)
    failures = []
    if proc.returncode != 0:
        failures.append(f"exit {proc.returncode}")
    for key, expected in case.expect.items():
        actual = parsed.get(key)
        if actual != expected:
            failures.append(f"{key}: expected {expected!r}, got {actual!r}")
    return not failures, {
        "name": case.name,
        "cmd": cmd,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "parsed": parsed,
        "failures": failures,
    }


def select_cases(names: Optional[Iterable[str]]) -> List[SimulationCase]:
    if not names:
        return CASES
    requested = set(names)
    selected = [case for case in CASES if case.name in requested]
    missing = requested - {case.name for case in selected}
    if missing:
        raise SystemExit(f"unknown simulation case(s): {', '.join(sorted(missing))}")
    return selected


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dsco", default="./dsco", help="path to the dsco binary")
    parser.add_argument("--case", action="append", dest="cases", help="run one named case")
    parser.add_argument("--json", action="store_true", help="emit machine-readable results")
    parser.add_argument("--timeout", type=float, default=10.0, help="per-case timeout in seconds")
    args = parser.parse_args()

    dsco = Path(args.dsco).expanduser().resolve()
    if not dsco.exists():
        parser.error(f"dsco binary not found: {dsco}")

    cases = select_cases(args.cases)
    results = []
    with tempfile.TemporaryDirectory(prefix="dsco_model_resolution_") as tmp:
        home = Path(tmp) / "home"
        home.mkdir()
        env = base_env(home)
        for case in cases:
            ok, result = run_case(dsco, case, env, args.timeout)
            results.append(result)
            if not args.json:
                parsed = result["parsed"]
                status = "ok" if ok else "FAIL"
                print(
                    "{:<4} {:<52} {} -> {} (fallback={}, auth={})".format(
                        status,
                        case.name,
                        parsed.get("alias_resolved_model", "(missing)"),
                        parsed.get("route_provider", "(missing)"),
                        parsed.get("fallback_active", "(missing)"),
                        parsed.get("auth_mode", "(missing)"),
                    )
                )
                for failure in result["failures"]:
                    print(f"     {failure}")

    failed = [result for result in results if result["failures"]]
    if args.json:
        print(json.dumps({"ok": not failed, "results": results}, indent=2, sort_keys=True))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
