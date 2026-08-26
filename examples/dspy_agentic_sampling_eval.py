#!/usr/bin/env python3
#
# /// script
# requires-python = ">=3.10"
# dependencies = [
#   "dspy",
#   "openai",
# ]
# ///
#
"""Batch eval runner for the DSPy agentic sampling harness."""

from __future__ import annotations

import argparse
from html import escape as html_escape
import json
import math
import multiprocessing as mp
import os
import sys
import time
from typing import Any

import dspy_agentic_sampling_harness as harness


COMPLEX_30: list[dict[str, str]] = [
    {
        "id": "c01_deadlock",
        "category": "concurrency",
        "prompt": "Diagnose a production deadlock in a C CLI where worker threads hold a provider mutex while emitting telemetry that also needs the same mutex. Give a minimal patch strategy, a repro test, and one rollback trigger.",
    },
    {
        "id": "c02_migration",
        "category": "migration",
        "prompt": "Plan a zero-downtime migration from a flat JSON session log to append-only SQLite tables while preserving replay compatibility for old sessions. Include schema boundaries, cutover phases, and validation queries.",
    },
    {
        "id": "c03_api_regression",
        "category": "compat",
        "prompt": "A local OpenAI-compatible gateway now returns HTTP 200 with empty assistant content for structured DSPy calls. Triage likely causes across adapter, gateway, and provider layers, then propose the cheapest verification ladder.",
    },
    {
        "id": "c04_threat_model",
        "category": "security",
        "prompt": "Threat-model an agent tool registry that loads external MCP metadata and can execute shell tools. Identify the three highest-impact abuse paths and the enforcement checks needed before execution.",
    },
    {
        "id": "c05_memory_leak",
        "category": "runtime",
        "prompt": "Find a plausible memory leak pattern in a streaming JSON parser that accumulates reasoning deltas and tool-call arguments. Describe instrumentation, a failing test shape, and a low-risk fix.",
    },
    {
        "id": "c06_model_routing",
        "category": "routing",
        "prompt": "Design a cost-aware model routing policy for root, review, orchestration, and leaf tasks where expensive models must prove marginal value. Include metrics and a stop condition.",
    },
    {
        "id": "c07_flaky_test",
        "category": "tests",
        "prompt": "A gateway integration test is flaky only on macOS under load. Build a debugging plan that separates port conflicts, process readiness, async timing, and provider nondeterminism.",
    },
    {
        "id": "c08_schema",
        "category": "data",
        "prompt": "Design an evaluation result schema for multi-agent samples, including raw sample records, Pareto frontier summaries, cost ledger fields, and enough provenance to reproduce a run.",
    },
    {
        "id": "c09_benchmark",
        "category": "performance",
        "prompt": "Interpret a benchmark where startup time improved 40 percent but first-token latency worsened 25 percent. Give likely causes, the next measurements, and a decision rule for accepting the change.",
    },
    {
        "id": "c10_observability",
        "category": "ops",
        "prompt": "Specify an observability plan for an agent CLI that must debug provider selection, tool execution, budget gates, and structured-output repair without logging private prompt text.",
    },
    {
        "id": "c11_retrieval_eval",
        "category": "eval",
        "prompt": "Create a retrieval evaluation for repo-grounded answers where correctness depends on exact file references. Include dataset construction, scoring, and anti-cheating controls.",
    },
    {
        "id": "c12_incident",
        "category": "incident",
        "prompt": "Write a concise incident analysis for a release that passed unit tests but broke the installed CLI due to a stale hotswap binary. Include root cause, missed signal, and prevention.",
    },
    {
        "id": "c13_auth",
        "category": "auth",
        "prompt": "Design token lifecycle handling for a local gateway that supports browser OAuth, static bearer tokens, and automated eval clients. Prioritize least surprise and debuggability.",
    },
    {
        "id": "c14_scheduler",
        "category": "agents",
        "prompt": "Propose a scheduler for multi-swarm agent work that balances breadth, depth, budget, and feedback reuse. Include how it decides not to expand a promising but costly branch.",
    },
    {
        "id": "c15_cache",
        "category": "cache",
        "prompt": "Analyze a cache invalidation bug where provider metadata changes but model capability checks use stale cached flags. Give a patch plan and two regression tests.",
    },
    {
        "id": "c16_selection",
        "category": "models",
        "prompt": "Compare two model-selection policies: cheapest-first with escalation versus expensive-root with cheap reviewers. Reason about quality, cost, tail latency, and failure isolation.",
    },
    {
        "id": "c17_rate_limit",
        "category": "distributed",
        "prompt": "Design distributed rate-limit handling for parallel agent workers that share one provider key. Include local admission control, retry budgets, and what gets surfaced to the user.",
    },
    {
        "id": "c18_ui_perf",
        "category": "frontend",
        "prompt": "A terminal UI flickers and loses cursor position during streamed tool events. Diagnose likely render-loop causes and propose a minimal fix that preserves keyboard responsiveness.",
    },
    {
        "id": "c19_db_locking",
        "category": "storage",
        "prompt": "Debug intermittent SQLite database locked errors in a local agent state store with concurrent readers and a background writer. Include pragmas, transaction boundaries, and test load.",
    },
    {
        "id": "c20_recovery",
        "category": "reliability",
        "prompt": "Plan backup and recovery for local agent sessions, tool ledgers, and config files. Include corruption detection, restore UX, and how to avoid overwriting user work.",
    },
    {
        "id": "c21_governance",
        "category": "safety",
        "prompt": "Define governance gates for mutating tools so every path routes through one execution function. Include metadata checks, user confirmation, and how to test bypass resistance.",
    },
    {
        "id": "c22_tool_schema",
        "category": "tools",
        "prompt": "A tool schema accepts malformed nested arrays that later crash an executor. Propose schema normalization, validation errors, and compatibility behavior for existing tool definitions.",
    },
    {
        "id": "c23_prompt_eval",
        "category": "eval",
        "prompt": "Design a prompt-eval rubric for coding agents where the answer must preserve user changes in a dirty git tree. Include pass/fail criteria and an adversarial case.",
    },
    {
        "id": "c24_load_shed",
        "category": "ops",
        "prompt": "Create a load-shedding policy for an agent gateway under high concurrency. Decide what to reject, queue, downgrade, or run locally, and define user-visible status messages.",
    },
    {
        "id": "c25_privacy",
        "category": "privacy",
        "prompt": "Design privacy controls for local activity logs that need enough metadata for debugging but must not store prompt contents by default. Include modes and retention policy.",
    },
    {
        "id": "c26_rollback",
        "category": "release",
        "prompt": "Build a release rollback plan for a CLI with repo-local and Homebrew-installed binaries. Include version checks, smoke tests, and how to avoid stale binary false positives.",
    },
    {
        "id": "c27_supply_chain",
        "category": "security",
        "prompt": "Assess supply-chain risk for a Python example that uses uv script dependencies. Include pinning tradeoffs, lockfile options, and a practical policy for examples.",
    },
    {
        "id": "c28_parser",
        "category": "parsing",
        "prompt": "A command parser mishandles flags after positional prompts when the prompt begins with dashes. Explain a robust parsing strategy and a compact test matrix.",
    },
    {
        "id": "c29_cost_ledger",
        "category": "finance",
        "prompt": "Audit a token cost ledger where projected spend and actual spend diverge across failed samples. Define accounting semantics and the fields needed to explain differences.",
    },
    {
        "id": "c30_readiness",
        "category": "release",
        "prompt": "Create a release-readiness checklist for this eval harness feature. Include runtime proof, artifact validation, user-facing docs, and one explicit no-go condition.",
    },
]


def plot_float(value: Any, default: float = 0.0) -> float:
    try:
        out = float(value)
    except (TypeError, ValueError):
        return default
    if math.isnan(out) or math.isinf(out):
        return default
    return out


def short(text: str, limit: int) -> str:
    text = " ".join(str(text or "").split())
    if len(text) <= limit:
        return text
    return text[: limit - 3] + "..."


def ensure_parent(path: str) -> None:
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)


def harness_args(eval_args: argparse.Namespace, problem: dict[str, str], remaining_seconds: float) -> argparse.Namespace:
    parser = harness.build_parser()
    per_problem_seconds = min(eval_args.problem_time_budget_seconds, max(1.0, remaining_seconds))
    argv = [
        "--spend",
        eval_args.spend,
        "--time-budget-seconds",
        f"{per_problem_seconds:.3f}",
        "--feedback-passes",
        str(eval_args.feedback_passes),
        "--swarms",
        str(eval_args.swarms),
        "--resolution",
        str(eval_args.resolution),
        "--branching",
        str(eval_args.branching),
        "--samples",
        str(eval_args.samples),
        "--max-depth",
        str(eval_args.max_depth),
        "--max-nodes",
        str(eval_args.max_nodes),
        "--beam-width",
        str(eval_args.beam_width),
        "--frontier-size",
        str(eval_args.frontier_size),
        "--budget-usd",
        str(eval_args.budget_usd_per_problem),
        "--reserve-fraction",
        str(eval_args.reserve_fraction),
        "--expected-output-tokens",
        str(eval_args.expected_output_tokens),
        "--max-output-tokens",
        str(eval_args.max_output_tokens),
        "--program",
        eval_args.program,
        "--signature-mode",
        eval_args.signature_mode,
        "--adapter",
        eval_args.adapter,
        "--base-url",
        eval_args.base_url,
        "--gateway-api-key",
        eval_args.gateway_api_key,
        "--preflight-timeout",
        str(eval_args.preflight_timeout),
    ]
    if eval_args.mode == "dry-run":
        argv.append("--dry-run")
    if eval_args.no_json_adapter_fallback:
        argv.append("--no-json-adapter-fallback")
    if not eval_args.feedback:
        argv.append("--no-feedback")
    argv.append(problem["prompt"])
    return harness.prepare_args(parser, parser.parse_args(argv), read_stdin=False)


def summarize_result(problem: dict[str, str], result: dict[str, Any], elapsed_seconds: float) -> dict[str, Any]:
    runtime = result.get("runtime") or {}
    ledger = result.get("ledger") or {}
    best = result.get("best") or None
    error = ""
    warning = ""
    for rec in result.get("samples") or []:
        if rec.get("error"):
            error = " ".join(str(rec.get("error") or "").split())
            break
    for rec in result.get("samples") or []:
        if rec.get("warning"):
            warning = " ".join(str(rec.get("warning") or "").split())
            break
    return {
        "id": problem["id"],
        "category": problem["category"],
        "prompt": problem["prompt"],
        "ok": bool(result.get("ok")),
        "mode": result.get("mode"),
        "elapsed_seconds": round(elapsed_seconds, 4),
        "stop_reason": runtime.get("stop_reason"),
        "successful_samples": runtime.get("successful_samples", 0),
        "sample_errors": runtime.get("sample_errors", 0),
        "sample_warnings": runtime.get("sample_warnings", 0),
        "frontier_count": len(result.get("pareto_frontier") or []),
        "best_id": best.get("id") if best else None,
        "best_layer": best.get("layer") if best else None,
        "best_model": best.get("model") if best else None,
        "best_utility": plot_float(best.get("utility") if best else None),
        "best_efficiency": plot_float(best.get("efficiency") if best else None),
        "spent_usd": plot_float(ledger.get("spent_usd")),
        "projected_usd": plot_float(ledger.get("projected_usd")),
        "input_tokens": int(ledger.get("input_tokens") or 0),
        "output_tokens": int(ledger.get("output_tokens") or 0),
        "error": error,
        "warning": warning,
    }


def failed_summary(
    problem: dict[str, str],
    mode: str,
    elapsed_seconds: float,
    stop_reason: str,
    error: str,
) -> dict[str, Any]:
    return {
        "id": problem["id"],
        "category": problem["category"],
        "prompt": problem["prompt"],
        "ok": False,
        "mode": mode,
        "elapsed_seconds": round(elapsed_seconds, 4),
        "stop_reason": stop_reason,
        "successful_samples": 0,
        "sample_errors": 1,
        "sample_warnings": 0,
        "frontier_count": 0,
        "best_id": None,
        "best_layer": None,
        "best_model": None,
        "best_utility": 0.0,
        "best_efficiency": 0.0,
        "spent_usd": 0.0,
        "projected_usd": 0.0,
        "input_tokens": 0,
        "output_tokens": 0,
        "error": error,
        "warning": "",
    }


def run_problem_child(eval_args: argparse.Namespace, problem: dict[str, str], remaining_seconds: float, queue: Any) -> None:
    try:
        run_args = harness_args(eval_args, problem, remaining_seconds)
        result = harness.run_harness(run_args)
        queue.put({"result": result})
    except BaseException as exc:
        queue.put({"error": f"{type(exc).__name__}: {exc}"})


def run_problem(eval_args: argparse.Namespace, problem: dict[str, str], remaining_seconds: float) -> dict[str, Any]:
    started = time.monotonic()
    if eval_args.hard_timeout:
        ctx = mp.get_context("spawn")
        queue = ctx.Queue()
        proc = ctx.Process(target=run_problem_child, args=(eval_args, problem, remaining_seconds, queue))
        proc.start()
        timeout = min(eval_args.problem_time_budget_seconds, max(1.0, remaining_seconds)) + eval_args.timeout_grace_seconds
        proc.join(timeout)
        if proc.is_alive():
            proc.terminate()
            proc.join(2.0)
            if proc.is_alive():
                proc.kill()
                proc.join(2.0)
            return failed_summary(
                problem,
                eval_args.mode,
                time.monotonic() - started,
                "problem_timeout",
                f"problem exceeded hard timeout of {timeout:.1f}s",
            )
        if queue.empty():
            return failed_summary(
                problem,
                eval_args.mode,
                time.monotonic() - started,
                "worker_exit",
                f"worker exited with code {proc.exitcode} without returning a result",
            )
        payload = queue.get()
        if payload.get("error"):
            return failed_summary(
                problem,
                eval_args.mode,
                time.monotonic() - started,
                "worker_error",
                str(payload["error"]),
            )
        return summarize_result(problem, payload["result"], time.monotonic() - started)

    run_args = harness_args(eval_args, problem, remaining_seconds)
    result = harness.run_harness(run_args)
    return summarize_result(problem, result, time.monotonic() - started)


def write_eval_svg(report: dict[str, Any], path: str) -> None:
    runs = list(report.get("runs") or [])
    width = 1400
    height = 900
    left = 92
    right = 176
    top = 112
    plot_h = 520
    plot_w = width - left - right
    bottom_y = top + plot_h
    max_cost = max([0.000001] + [plot_float(r.get("spent_usd")) for r in runs])
    count = max(1, len(runs))
    step = plot_w / count
    bar_h = 96
    bar_top = bottom_y + 62
    summary = report.get("summary") or {}
    spend_label = "simulated spend" if summary.get("mode") == "dry-run" else "estimated spend"
    clean_count = sum(1 for run in runs if run.get("ok") and not run.get("sample_warnings"))
    partial_count = sum(1 for run in runs if run.get("ok") and run.get("sample_warnings"))
    failed_count = sum(1 for run in runs if not run.get("ok"))

    def x_at(idx: int) -> float:
        return left + step * (idx + 0.5)

    def health_score(run: dict[str, Any]) -> float:
        if not run.get("ok"):
            return 0.0
        if run.get("sample_warnings"):
            return 0.55
        return 1.0

    def y_health(value: float) -> float:
        return bottom_y - max(0.0, min(1.0, value)) * plot_h

    def category_color(category: str) -> str:
        colors = {
            "security": "#dc2626",
            "safety": "#b91c1c",
            "eval": "#2563eb",
            "ops": "#7c3aed",
            "reliability": "#0891b2",
            "runtime": "#ea580c",
            "performance": "#16a34a",
            "release": "#0f766e",
            "routing": "#9333ea",
        }
        return colors.get(category, "#334155")

    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#f8fafc"/>',
        f'<text x="{left}" y="42" font-family="Arial, sans-serif" font-size="24" font-weight="700" fill="#0f172a">Complex30 Eval Results</text>',
        f'<text x="{left}" y="70" font-family="Arial, sans-serif" font-size="13" fill="#475569">mode={html_escape(str(summary.get("mode", "")))} signature={html_escape(str(summary.get("signature_mode", "")))} adapter={html_escape(str(summary.get("adapter", "")))} run={summary.get("problems_run", 0)}/{summary.get("problems_requested", 0)} ok={summary.get("ok_count", 0)} errors={summary.get("error_count", 0)} warnings={summary.get("warning_count", 0)} elapsed={plot_float(summary.get("elapsed_seconds")):.1f}s {spend_label}=${plot_float(summary.get("spent_usd")):.6f}</text>',
        f'<text x="{left}" y="92" font-family="Arial, sans-serif" font-size="13" fill="#475569">parse health: clean={clean_count} partial={partial_count} failed={failed_count}; partial means result was salvaged from a DSPy adapter parse error</text>',
        f'<rect x="{left}" y="{top}" width="{plot_w}" height="{plot_h}" fill="#ffffff" stroke="#cbd5e1"/>',
    ]

    for i in range(6):
        y = top + plot_h * i / 5
        val = 1.0 - i / 5
        lines.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}" stroke="#e2e8f0"/>')
        lines.append(f'<text x="{left - 12}" y="{y + 4:.2f}" text-anchor="end" font-family="Arial, sans-serif" font-size="11" fill="#64748b">{val:.2f}</text>')
    for label, value, color in (
        ("clean structured parse", 1.0, "#16a34a"),
        ("partial parse salvage", 0.55, "#d97706"),
        ("failed/no answer", 0.0, "#dc2626"),
    ):
        y = y_health(value)
        lines.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}" stroke="{color}" stroke-width="1.3" stroke-dasharray="5 5" opacity="0.45"/>')
        lines.append(f'<text x="{left + plot_w + 8}" y="{y + 4:.2f}" font-family="Arial, sans-serif" font-size="10" fill="{color}">{label}</text>')
    for i in range(count):
        x = x_at(i)
        if i % 5 == 0 or count <= 15:
            lines.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{bottom_y}" stroke="#eef2f7"/>')
            lines.append(f'<text x="{x:.2f}" y="{bottom_y + 22}" text-anchor="middle" font-family="Arial, sans-serif" font-size="10" fill="#64748b">{i + 1}</text>')

    for idx, run in enumerate(runs):
        x = x_at(idx)
        health = health_score(run)
        y = y_health(health)
        title = html_escape(
            f"{idx + 1}. {run.get('id')} {run.get('category')} ok={run.get('ok')} "
            f"health={health:.2f} utility={plot_float(run.get('best_utility')):.4f} cost=${plot_float(run.get('spent_usd')):.8f} "
            f"stop={run.get('stop_reason')} warning={short(str(run.get('warning') or ''), 80)} "
            f"error={short(str(run.get('error') or ''), 160)}"
        )
        radius = 5.0 + min(7.0, 40.0 * plot_float(run.get("spent_usd")) / max_cost)
        if run.get("ok") and run.get("sample_warnings"):
            size = radius + 2.0
            color = category_color(str(run.get("category")))
            points = f"{x:.2f},{y - size:.2f} {x + size:.2f},{y:.2f} {x:.2f},{y + size:.2f} {x - size:.2f},{y:.2f}"
            lines.append(f'<polygon points="{points}" fill="#f59e0b" stroke="{color}" stroke-width="1.5" opacity="0.9"><title>{title}</title></polygon>')
        elif run.get("ok"):
            lines.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{radius:.2f}" fill="#16a34a" stroke="{category_color(str(run.get("category")))}" stroke-width="1.5" opacity="0.9"><title>{title}</title></circle>')
        else:
            lines.append(f'<g stroke="#dc2626" stroke-width="2.2" opacity="0.9"><title>{title}</title>')
            lines.append(f'<line x1="{x - 6:.2f}" y1="{y - 6:.2f}" x2="{x + 6:.2f}" y2="{y + 6:.2f}"/>')
            lines.append(f'<line x1="{x - 6:.2f}" y1="{y + 6:.2f}" x2="{x + 6:.2f}" y2="{y - 6:.2f}"/>')
            lines.append("</g>")
        cost_height = max(1.0, (plot_float(run.get("spent_usd")) / max_cost) * bar_h)
        lines.append(f'<rect x="{x - step * 0.32:.2f}" y="{bar_top + bar_h - cost_height:.2f}" width="{max(2.0, step * 0.64):.2f}" height="{cost_height:.2f}" fill="#94a3b8" opacity="0.56"><title>{title}</title></rect>')

    lines.extend(
        [
            f'<text x="{left + plot_w / 2:.2f}" y="{height - 32}" text-anchor="middle" font-family="Arial, sans-serif" font-size="13" fill="#334155">problem index; green circle=clean parse, amber diamond=partial parse salvage, red X=failed, gray bars=spend</text>',
            f'<text x="28" y="{top + plot_h / 2:.2f}" text-anchor="middle" transform="rotate(-90 28 {top + plot_h / 2:.2f})" font-family="Arial, sans-serif" font-size="13" fill="#334155">parse health</text>',
            f'<text x="{left}" y="{bar_top - 16}" font-family="Arial, sans-serif" font-size="13" fill="#334155">{html_escape(spend_label)} per problem, max=${max_cost:.6f}</text>',
            '<g font-family="Arial, sans-serif" font-size="12" fill="#334155">',
            f'<circle cx="{left + plot_w - 210}" cy="{top + 24}" r="7" fill="#16a34a" opacity="0.9"/>',
            f'<text x="{left + plot_w - 196}" y="{top + 28}">clean parse</text>',
            f'<polygon points="{left + plot_w - 210},{top + 42} {left + plot_w - 202},{top + 50} {left + plot_w - 210},{top + 58} {left + plot_w - 218},{top + 50}" fill="#f59e0b" opacity="0.9"/>',
            f'<text x="{left + plot_w - 196}" y="{top + 54}">partial parse</text>',
            f'<g stroke="#dc2626" stroke-width="2.2"><line x1="{left + plot_w - 216}" y1="{top + 70}" x2="{left + plot_w - 204}" y2="{top + 82}"/><line x1="{left + plot_w - 216}" y1="{top + 82}" x2="{left + plot_w - 204}" y2="{top + 70}"/></g>',
            f'<text x="{left + plot_w - 196}" y="{top + 80}">failed/no answer</text>',
            f'<rect x="{left + plot_w - 216}" y="{top + 94}" width="14" height="14" fill="#94a3b8" opacity="0.56"/>',
            f'<text x="{left + plot_w - 196}" y="{top + 106}">spend</text>',
            "</g>",
            "</svg>",
        ]
    )
    ensure_parent(path)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the built-in complex30 eval set against the DSPy sampling harness.")
    parser.add_argument("--mode", choices=["dry-run", "live"], default="dry-run")
    parser.add_argument("--limit", type=int, default=30, help="number of built-in problems to run")
    parser.add_argument("--max-minutes", type=float, default=20.0, help="wall-clock cap for the batch")
    parser.add_argument("--problem-time-budget-seconds", type=float, default=35.0)
    parser.add_argument("--hard-timeout", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--timeout-grace-seconds", type=float, default=5.0)
    parser.add_argument("--output-json", default=".workspace/dspy_eval_complex30.json")
    parser.add_argument("--plot-results", default=".workspace/dspy_eval_complex30.svg")
    parser.add_argument("--canary-first", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--force-live", action="store_true", help="continue live batch even when the first problem has no successful sample")
    parser.add_argument("--spend", choices=sorted(harness.SPEND_PRESETS), default="quick")
    parser.add_argument("--feedback-passes", type=int, default=1)
    parser.add_argument("--swarms", type=int, default=1)
    parser.add_argument("--resolution", type=float, default=1.0)
    parser.add_argument("--branching", type=int, default=2)
    parser.add_argument("--samples", type=int, default=1)
    parser.add_argument("--max-depth", type=int, default=0)
    parser.add_argument("--max-nodes", type=int, default=8)
    parser.add_argument("--beam-width", type=int, default=1)
    parser.add_argument("--frontier-size", type=int, default=6)
    parser.add_argument("--budget-usd-per-problem", type=float, default=0.03)
    parser.add_argument("--reserve-fraction", type=float, default=0.12)
    parser.add_argument("--expected-output-tokens", type=int, default=300)
    parser.add_argument("--max-output-tokens", type=int, default=400)
    parser.add_argument("--program", choices=["predict", "cot"], default="predict")
    parser.add_argument("--signature-mode", choices=["compact", "rich"], default="compact")
    parser.add_argument("--adapter", choices=["chat", "json", "xml"], default="json")
    parser.add_argument("--no-json-adapter-fallback", action="store_true")
    parser.add_argument("--feedback", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--base-url", default=harness.DEFAULT_BASE_URL)
    parser.add_argument("--gateway-api-key", default=os.getenv("DSCO_WEB_TOKEN", harness.DEFAULT_GATEWAY_KEY))
    parser.add_argument("--preflight-timeout", type=float, default=2.0)
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    if args.limit <= 0:
        parser.error("--limit must be positive")
    if args.max_minutes <= 0:
        parser.error("--max-minutes must be positive")
    if args.problem_time_budget_seconds <= 0:
        parser.error("--problem-time-budget-seconds must be positive")
    problems = COMPLEX_30[: min(args.limit, len(COMPLEX_30))]
    started = time.monotonic()
    deadline = started + args.max_minutes * 60.0
    runs: list[dict[str, Any]] = []
    aborted_after_canary = False

    for idx, problem in enumerate(problems):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        summary = run_problem(args, problem, remaining)
        runs.append(summary)
        print(
            f"{idx + 1:02d}/{len(problems):02d} {summary['id']} "
            f"ok={str(summary['ok']).lower()} stop={summary['stop_reason']} "
            f"utility={summary['best_utility']:.4f} spent=${summary['spent_usd']:.8f}",
            flush=True,
        )
        if args.mode == "live" and args.canary_first and idx == 0 and not summary["ok"] and not args.force_live:
            aborted_after_canary = True
            break

    elapsed = time.monotonic() - started
    report = {
        "summary": {
            "mode": args.mode,
            "signature_mode": args.signature_mode,
            "adapter": args.adapter,
            "problems_requested": len(problems),
            "problems_run": len(runs),
            "ok_count": sum(1 for run in runs if run["ok"]),
            "error_count": sum(1 for run in runs if not run["ok"]),
            "warning_count": sum(1 for run in runs if run.get("sample_warnings")),
            "aborted_after_canary": aborted_after_canary,
            "elapsed_seconds": round(elapsed, 4),
            "spent_usd": round(sum(plot_float(run.get("spent_usd")) for run in runs), 8),
            "projected_usd": round(sum(plot_float(run.get("projected_usd")) for run in runs), 8),
            "input_tokens": sum(int(run.get("input_tokens") or 0) for run in runs),
            "output_tokens": sum(int(run.get("output_tokens") or 0) for run in runs),
            "spend_label": "simulated" if args.mode == "dry-run" else "estimated",
            "plot_results": args.plot_results,
            "output_json": args.output_json,
        },
        "runs": runs,
    }
    ensure_parent(args.output_json)
    with open(args.output_json, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
        f.write("\n")
    write_eval_svg(report, args.plot_results)

    summary = report["summary"]
    print(
        "summary "
        f"mode={summary['mode']} run={summary['problems_run']}/{summary['problems_requested']} "
        f"ok={summary['ok_count']} errors={summary['error_count']} warnings={summary['warning_count']} "
        f"{summary['spend_label']}_spent=${summary['spent_usd']:.8f} elapsed={summary['elapsed_seconds']:.2f}s "
        f"plot={args.plot_results} json={args.output_json}",
        flush=True,
    )
    if aborted_after_canary:
        print("aborted_after_canary=true", flush=True)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
