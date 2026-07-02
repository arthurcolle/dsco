#!/usr/bin/env python3
"""DSCO performance rollout harness.

Runs repeatable critical-path benchmarks, records JSONL events, and generates
Markdown/SVG graphs. Designed to be cheap enough for local iteration while
capturing enough detail to drive multi-turn performance rollouts.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Sequence


@dataclass
class Case:
    name: str
    command: List[str]
    timeout_s: float = 20.0
    max_output: int = 500_000


DEFAULT_CASES: List[Case] = [
    Case("version", ["./dsco", "--version"]),
    Case("help", ["./dsco", "--help"]),
    Case("tools_json", ["./dsco", "--tools-json"]),
    Case("models_json", ["./dsco", "--models-json"]),
    Case("tool_cwd", ["./dsco", "--tool-exec", "cwd", "{}"]),
    Case("route_explain_sonnet", ["./dsco", "--route-explain", "claude-sonnet-4-5"], timeout_s=30.0),
]


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def percentile(xs: Sequence[float], p: float) -> float:
    if not xs:
        return 0.0
    ordered = sorted(xs)
    if len(ordered) == 1:
        return ordered[0]
    k = (len(ordered) - 1) * p
    f = int(k)
    c = min(f + 1, len(ordered) - 1)
    if f == c:
        return ordered[f]
    return ordered[f] * (c - k) + ordered[c] * (k - f)


def run_case(case: Case, cwd: Path, env: Dict[str, str]) -> Dict:
    start_ns = time.perf_counter_ns()
    start_wall = now_iso()
    try:
        proc = subprocess.run(
            case.command,
            cwd=str(cwd),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=case.timeout_s,
            check=False,
        )
        timed_out = False
    except subprocess.TimeoutExpired as e:
        proc = e
        timed_out = True
    end_ns = time.perf_counter_ns()
    dur_ms = (end_ns - start_ns) / 1_000_000.0

    stdout = getattr(proc, "stdout", b"") or b""
    stderr = getattr(proc, "stderr", b"") or b""
    if isinstance(stdout, str):
        stdout_b = stdout.encode()
    else:
        stdout_b = stdout
    if isinstance(stderr, str):
        stderr_b = stderr.encode()
    else:
        stderr_b = stderr

    return {
        "event": "benchmark_run",
        "case": case.name,
        "command": case.command,
        "start": start_wall,
        "duration_ms": dur_ms,
        "returncode": None if timed_out else getattr(proc, "returncode", None),
        "timed_out": timed_out,
        "stdout_bytes": len(stdout_b),
        "stderr_bytes": len(stderr_b),
        "stdout_head": stdout_b[:400].decode("utf-8", "replace"),
        "stderr_head": stderr_b[:400].decode("utf-8", "replace"),
    }


def summarize(events: List[Dict]) -> Dict:
    by_case: Dict[str, List[Dict]] = {}
    for ev in events:
        by_case.setdefault(ev["case"], []).append(ev)
    cases = []
    for name, rows in by_case.items():
        durs = [r["duration_ms"] for r in rows if not r.get("timed_out")]
        ok_rows = [r for r in rows if (not r.get("timed_out") and r.get("returncode") == 0)]
        cases.append({
            "case": name,
            "runs": len(rows),
            "ok": len(ok_rows),
            "failures": len(rows) - len(ok_rows),
            "min_ms": min(durs) if durs else None,
            "p50_ms": percentile(durs, 0.50) if durs else None,
            "p95_ms": percentile(durs, 0.95) if durs else None,
            "max_ms": max(durs) if durs else None,
            "mean_ms": statistics.mean(durs) if durs else None,
            "stdout_bytes_median": int(statistics.median([r["stdout_bytes"] for r in rows])) if rows else 0,
        })
    cases.sort(key=lambda x: x["case"])
    return {
        "event": "benchmark_summary",
        "generated_at": now_iso(),
        "host": platform.node(),
        "platform": platform.platform(),
        "python": sys.version.split()[0],
        "cases": cases,
    }


def bar_svg(summary: Dict, path: Path, field: str = "p50_ms") -> None:
    cases = summary["cases"]
    width = 980
    row_h = 34
    margin_l = 210
    margin_r = 120
    margin_t = 36
    height = margin_t + row_h * len(cases) + 42
    vals = [c[field] or 0 for c in cases]
    max_val = max(vals) if vals else 1
    max_val = max(max_val, 1)
    scale_w = width - margin_l - margin_r
    colors = ["#4f46e5", "#0891b2", "#16a34a", "#ca8a04", "#dc2626", "#9333ea"]
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#0b1020"/>',
        '<text x="24" y="24" fill="#f8fafc" font-family="Menlo, monospace" font-size="18">DSCO Critical Path Latency — baseline</text>',
    ]
    for i, c in enumerate(cases):
        y = margin_t + i * row_h
        val = c[field] or 0
        bw = (val / max_val) * scale_w
        color = colors[i % len(colors)]
        parts.append(f'<text x="24" y="{y+22}" fill="#cbd5e1" font-family="Menlo, monospace" font-size="13">{c["case"]}</text>')
        parts.append(f'<rect x="{margin_l}" y="{y+7}" width="{bw:.1f}" height="20" rx="4" fill="{color}"/>')
        parts.append(f'<text x="{margin_l + bw + 8:.1f}" y="{y+22}" fill="#f8fafc" font-family="Menlo, monospace" font-size="12">{val:.2f} ms</text>')
    parts.append('</svg>')
    path.write_text("\n".join(parts), encoding="utf-8")


def projection_svg(path: Path) -> None:
    # A deliberately conservative improvement model for rollout visualization.
    phases = [
        ("P0 baseline", 100, 100, 100),
        ("P1 instrument", 98, 98, 100),
        ("P2 schema diet", 70, 62, 82),
        ("P3 result reducers", 56, 48, 76),
        ("P4 cache prefix", 38, 31, 72),
        ("P5 capability init", 30, 28, 58),
        ("P6 worker profiles", 24, 24, 48),
        ("P7 scheduler", 22, 22, 44),
    ]
    width, height = 1100, 520
    ml, mt, mr, mb = 95, 55, 40, 90
    plot_w, plot_h = width - ml - mr, height - mt - mb
    def x(i): return ml + i * plot_w / (len(phases) - 1)
    def y(v): return mt + (100 - v) * plot_h / 100
    series = [
        ("turn_latency_index", 1, "#38bdf8"),
        ("token_cost_index", 2, "#a3e635"),
        ("rss_index", 3, "#f472b6"),
    ]
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#080b14"/>',
        '<text x="24" y="32" fill="#f8fafc" font-family="Menlo, monospace" font-size="20">Performance Rollout Projection Index</text>',
        '<text x="24" y="52" fill="#94a3b8" font-family="Menlo, monospace" font-size="12">Lower is better. Baseline = 100.</text>',
    ]
    for tick in [0, 25, 50, 75, 100]:
        yy = y(tick)
        parts.append(f'<line x1="{ml}" y1="{yy:.1f}" x2="{width-mr}" y2="{yy:.1f}" stroke="#1e293b"/>')
        parts.append(f'<text x="42" y="{yy+4:.1f}" fill="#64748b" font-family="Menlo, monospace" font-size="11">{tick}</text>')
    for name, idx, color in series:
        pts = " ".join(f'{x(i):.1f},{y(p[idx]):.1f}' for i, p in enumerate(phases))
        parts.append(f'<polyline points="{pts}" fill="none" stroke="{color}" stroke-width="4"/>')
        for i, p in enumerate(phases):
            parts.append(f'<circle cx="{x(i):.1f}" cy="{y(p[idx]):.1f}" r="5" fill="{color}"/>')
        parts.append(f'<text x="{width-280}" y="{80 + 24*idx}" fill="{color}" font-family="Menlo, monospace" font-size="13">{name}</text>')
    for i, p in enumerate(phases):
        xx = x(i)
        parts.append(f'<line x1="{xx:.1f}" y1="{height-mb}" x2="{xx:.1f}" y2="{height-mb+6}" stroke="#475569"/>')
        parts.append(f'<text transform="translate({xx-8:.1f},{height-mb+18}) rotate(45)" fill="#cbd5e1" font-family="Menlo, monospace" font-size="11">{p[0]}</text>')
    parts.append('</svg>')
    path.write_text("\n".join(parts), encoding="utf-8")


def markdown(summary: Dict, out_dir: Path) -> str:
    lines = [
        "# DSCO Performance Rollout Baseline", "",
        f"Generated: `{summary['generated_at']}`", "",
        "## Host", "",
        f"- Host: `{summary['host']}`",
        f"- Platform: `{summary['platform']}`", "",
        "## Baseline critical paths", "",
        "![Baseline latency](baseline_latency.svg)", "",
        "| Case | Runs | OK | Failures | p50 ms | p95 ms | max ms | median stdout bytes |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for c in summary["cases"]:
        lines.append(
            f"| {c['case']} | {c['runs']} | {c['ok']} | {c['failures']} | "
            f"{(c['p50_ms'] or 0):.2f} | {(c['p95_ms'] or 0):.2f} | {(c['max_ms'] or 0):.2f} | {c['stdout_bytes_median']} |"
        )
    lines += ["", "## Rollout projection", "", "![Rollout projection](rollout_projection.svg)", ""]
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=".")
    ap.add_argument("--out", default="reports/perf_push_limit")
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--sleep", type=float, default=0.05)
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env.setdefault("DSCO_PERF", "json")
    env.setdefault("DSCO_NO_APPROVAL_PROMPTS", "1")

    events: List[Dict] = []
    for case in DEFAULT_CASES:
        for i in range(args.runs):
            ev = run_case(case, repo, env)
            ev["iteration"] = i + 1
            events.append(ev)
            print(json.dumps(ev), flush=True)
            time.sleep(args.sleep)

    jsonl = out / "baseline_runs.jsonl"
    with jsonl.open("w", encoding="utf-8") as f:
        for ev in events:
            f.write(json.dumps(ev) + "\n")

    summary = summarize(events)
    (out / "baseline_summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    bar_svg(summary, out / "baseline_latency.svg")
    projection_svg(out / "rollout_projection.svg")
    (out / "baseline.md").write_text(markdown(summary, out), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
