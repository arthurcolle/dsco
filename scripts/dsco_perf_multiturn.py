#!/usr/bin/env python3
"""Generate an instrumented multi-turn rollout plan for DSCO performance work.

This is intentionally model/provider-agnostic. It emits a JSONL control plane
that can be used by dsco, Claude, Codex, or local workers to execute bounded
turns, collect evidence, and advance gates.
"""
from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path

PHASES = [
    {
        "id": "P1",
        "name": "Measurement substrate",
        "turns": [
            "Wire DSCO_PERF=json spans into startup, registry, provider, tool dispatch, and worker launch.",
            "Add perf_bench CI/local runner with p50/p95/RSS/schema-byte regression gates.",
            "Add report generator that converts JSONL spans into Markdown/SVG summaries.",
        ],
        "gate": "Every critical path emits stable JSON spans and benchmark script exits non-zero on regression.",
    },
    {
        "id": "P2",
        "name": "Schema diet and tool packs",
        "turns": [
            "Create task-class to tool-pack resolver with minimal default active schema set.",
            "Generate compact schemas for active model context while preserving full discovery schemas.",
            "Add active_schema_tokens and active_tool_count metrics to every model turn.",
        ],
        "gate": "Default non-specialized model turn exposes <=40 active tools and schema tokens drop >=50%.",
    },
    {
        "id": "P3",
        "name": "Tool result reducers",
        "turns": [
            "Introduce result handle protocol for outputs over threshold.",
            "Implement reducers for grep, bash/build, git diff/status, web fetch, and market scans.",
            "Add context_result_bytes and offloaded_result_bytes metrics.",
        ],
        "gate": "Large tool outputs no longer enter context raw unless explicitly recalled.",
    },
    {
        "id": "P4",
        "name": "Prompt cache architecture",
        "turns": [
            "Partition static prefix, semi-static project summary, and dynamic turn tail.",
            "Add provider-specific cache-control validation and cache-hit telemetry.",
            "Stabilize prompt_cache_key derivation by repo, model, tool-pack, and identity version.",
        ],
        "gate": "Supported providers report cache reuse and repeated turn cost/latency materially decreases.",
    },
    {
        "id": "P5",
        "name": "Capability-bit startup",
        "turns": [
            "Parse argv into command grammar and capability mask before subsystem initialization.",
            "Gate memory, MCP, TUI, market, browser, provider, and swarm init behind capability bits.",
            "Add startup capability plan to route explain and perf spans.",
        ],
        "gate": "Info/local-tool paths prove no unnecessary subsystem spans occurred.",
    },
    {
        "id": "P6",
        "name": "Worker profiles and swarm QoS",
        "turns": [
            "Define minimal worker profiles: local_tool, model_only, research, mcp, market, compile.",
            "Add load-aware launch scheduler with CPU/RSS/provider-rate budgets.",
            "Require structured bounded worker returns and deterministic reducers.",
        ],
        "gate": "Worker heartbeat latency improves and high system load causes queueing instead of thrash.",
    },
    {
        "id": "P7",
        "name": "Runtime memory efficiency",
        "turns": [
            "Introduce per-turn arenas in parser, provider request build, tool arg parse, and result assembly.",
            "Intern repeated model/tool/provider strings and compact hot structs.",
            "Replace live scans with generated incremental indexes for skills/doctrine/tools/models.",
        ],
        "gate": "Allocator count/RSS/binary-size budgets improve or remain bounded with no correctness loss.",
    },
]


def now():
    return datetime.now(timezone.utc).isoformat()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="reports/perf_push_limit/multiturn_rollout.jsonl")
    args = ap.parse_args()
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    seq = 0
    with out.open("w", encoding="utf-8") as f:
        for phase in PHASES:
            for turn_no, task in enumerate(phase["turns"], 1):
                seq += 1
                rec = {
                    "event": "rollout_turn",
                    "seq": seq,
                    "created_at": now(),
                    "phase_id": phase["id"],
                    "phase_name": phase["name"],
                    "turn_no": turn_no,
                    "task": task,
                    "instrumentation_required": [
                        "perf_json_spans",
                        "before_after_benchmark",
                        "artifact_manifest",
                        "rollback_note",
                    ],
                    "evidence": [],
                    "status": "planned",
                    "gate": phase["gate"],
                }
                f.write(json.dumps(rec) + "\n")
    print(str(out))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
