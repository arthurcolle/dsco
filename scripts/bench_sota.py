#!/usr/bin/env python3
"""Offline SOTA profile benchmark harness for dsco.

Emits JSONL only. This target is intentionally local-only so it can run in CI
without provider credentials or network access.
"""

from __future__ import annotations

import json
import os
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_THRESHOLDS = {
    "dsco-lite-tool-exec-cwd:p50_ms": 5.0,
    "dsco-lite-models-json:p50_ms": 15.0,
    "dsco-lite-tools-json:p50_ms": 15.0,
    "dsco-profile-lite-tool-exec-cwd:p50_ms": 15.0,
    "dsco-profile-worker-tool-exec-cwd:p50_ms": 20.0,
    "dsco-profile-lite-tool-exec:total_ms": 1.0,
    "dsco-profile-worker-tool-exec:total_ms": 1.0,
    "dsco-lite:bytes": 600 * 1024,
}


def emit(obj: dict) -> None:
    print(json.dumps(obj, sort_keys=True, separators=(",", ":")), flush=True)


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = int(round((len(ordered) - 1) * pct))
    if idx < 0:
        idx = 0
    if idx >= len(ordered):
        idx = len(ordered) - 1
    return ordered[idx]


def threshold_for(key: str) -> float | None:
    env_key = "DSCO_GATE_" + "".join(
        ch if ch.isalnum() else "_" for ch in key.upper()
    )
    if env_key in os.environ:
        return float(os.environ[env_key])
    return DEFAULT_THRESHOLDS.get(key)


def gate(case: str, metric: str, value: float) -> bool:
    key = f"{case}:{metric}"
    threshold = threshold_for(key)
    if threshold is None:
        return True
    ok = value <= threshold
    emit({
        "bench": "sota",
        "type": "gate",
        "case": case,
        "metric": metric,
        "value": round(value, 3) if isinstance(value, float) else value,
        "threshold": threshold,
        "ok": ok,
    })
    return ok


def run_once(argv: list[str], env_extra: dict[str, str] | None = None,
             capture_stderr: bool = False) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env.setdefault("DSCO_NO_AUTO_INTERACTIVE", "1")
    if env_extra:
        env.update(env_extra)
    return subprocess.run(
        argv,
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE if capture_stderr else subprocess.DEVNULL,
        env=env,
        check=False,
        text=True,
    )


def bench_case(name: str, argv: list[str], samples: int, warmup: int) -> bool:
    for _ in range(warmup):
        cp = run_once(argv)
        if cp.returncode != 0:
            emit({"bench": "sota", "type": "error", "case": name,
                  "returncode": cp.returncode})
            return False

    values: list[float] = []
    for _ in range(samples):
        t0 = time.perf_counter()
        cp = run_once(argv)
        elapsed_ms = (time.perf_counter() - t0) * 1000.0
        if cp.returncode != 0:
            emit({"bench": "sota", "type": "error", "case": name,
                  "returncode": cp.returncode})
            return False
        values.append(elapsed_ms)

    stats = {
        "bench": "sota",
        "type": "timing",
        "case": name,
        "samples": samples,
        "p50_ms": round(statistics.median(values), 3),
        "p95_ms": round(percentile(values, 0.95), 3),
        "min_ms": round(min(values), 3),
        "max_ms": round(max(values), 3),
        "mean_ms": round(statistics.fmean(values), 3),
    }
    emit(stats)
    return gate(name, "p50_ms", float(stats["p50_ms"]))


def perf_internal(profile: str) -> bool:
    cp = run_once(
        ["./dsco", "--profile", profile, "--tool-exec", "cwd", "{}"],
        {"DSCO_PERF": "json"},
        capture_stderr=True,
    )
    if cp.returncode != 0:
        emit({"bench": "sota", "type": "error",
              "case": f"dsco-profile-{profile}-internal",
              "returncode": cp.returncode})
        return False

    last = None
    for line in cp.stderr.splitlines():
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if obj.get("type") == "startup" and obj.get("event") == "mark":
            last = obj
    if last:
        total_ms = float(last.get("total_ms", 0.0))
        case = f"dsco-profile-{profile}-tool-exec"
        emit({
            "bench": "sota",
            "type": "startup_internal",
            "case": case,
            "profile": profile,
            "caps": last.get("caps", ""),
            "total_ms": total_ms,
            "label": last.get("label", ""),
        })
        return gate(case, "total_ms", total_ms)
    return True


def emit_size(binary: str) -> bool:
    path = ROOT / binary
    if not path.exists():
        emit({"bench": "sota", "type": "error", "case": f"{binary}-size",
              "reason": "missing binary"})
        return False
    bytes_len = path.stat().st_size
    emit({
        "bench": "sota",
        "type": "size",
        "binary": binary,
        "bytes": bytes_len,
    })
    return gate(binary, "bytes", float(bytes_len))


def main() -> int:
    samples = int(os.environ.get("DSCO_BENCH_SAMPLES", "25"))
    warmup = int(os.environ.get("DSCO_BENCH_WARMUP", "3"))
    if samples <= 0 or samples > 1000:
        raise SystemExit("DSCO_BENCH_SAMPLES must be 1..1000")
    if warmup < 0 or warmup > 100:
        raise SystemExit("DSCO_BENCH_WARMUP must be 0..100")

    emit({
        "bench": "sota",
        "type": "begin",
        "samples": samples,
        "warmup": warmup,
        "system": platform.system(),
        "machine": platform.machine(),
        "gates": DEFAULT_THRESHOLDS,
    })

    ok = True
    cases = [
        ("dsco-lite-tool-exec-cwd", ["./dsco-lite", "--tool-exec", "cwd", "{}"]),
        ("dsco-lite-models-json", ["./dsco-lite", "--models-json"]),
        ("dsco-lite-tools-json", ["./dsco-lite", "--tools-json"]),
        ("dsco-profile-lite-tool-exec-cwd",
         ["./dsco", "--profile", "lite", "--tool-exec", "cwd", "{}"]),
        ("dsco-profile-worker-tool-exec-cwd",
         ["./dsco", "--profile", "worker", "--tool-exec", "cwd", "{}"]),
    ]
    for name, argv in cases:
        ok = bench_case(name, argv, samples, warmup) and ok

    ok = perf_internal("lite") and ok
    ok = perf_internal("worker") and ok
    ok = emit_size("dsco") and ok
    ok = emit_size("dsco-lite") and ok
    emit({"bench": "sota", "type": "end", "ok": ok})
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
