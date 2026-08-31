#!/usr/bin/env python3
"""flywheel_label.py — read-only projector over DSCO per-run WAL journals.

Reconstructs episodes from length-prefixed JSON frames
(~/.dsco/runs/<run_id>/journal.wal), computes persistence components P1-P5
(DATA_FLYWHEEL_ARCHITECTURE.md §1), and emits candidate training-example
labels as JSONL. Never mutates run history. Writes only to --out.

Frame format (from src/chronicle.c:chronicle_journal_append, 2026-08-31):
  8-byte header: uint32 LE payload_len + uint32 LE crc32(payload)
  payload: JSON line  {"v":1,"type":<RECORD_TYPE>,"run_id":...,"seq":N,"wall_ms":N,"payload":{...}}
Record types observed in source: run.started, TOOL_CALL, TOOL_RESULT (canonical,
fsync-forced), rl.trajectory.event, plus free-form. Payloads nest under "payload".

Usage:
  python3 scripts/flywheel_label.py --runs ~/.dsco/runs --out out/episodes.jsonl \
      [--max-runs N] [--report report.json]
"""

import argparse
import json
import os
import struct
import sys
import time
import zlib
from collections import defaultdict

SCHEMA = "dsco.flywheel.episode.v1"
VERIFIER_ID = "flywheel_label.intrinsic"
VERIFIER_VERSION = "0.1.0"

# Error/repair classification for P2 (strategy-cluster diversity over tool+error signatures)
FAILURE_CLASSES = {
    "timeout": ["timeout", "timed out", "deadline"],
    "denied": ["denied", "not authorized", "permission", "approval"],
    "not_found": ["no such file", "not found", "enoent"],
    "parse": ["json", "parse", "syntax", "malformed"],
    "network": ["econn", "socket", "tls", "http 5", "dns"],
    "exit_nonzero": ["exit code", "exit_status", "non-zero", "nonzero"],
}


MAX_FRAME = 32 * 1024 * 1024  # hard limit enforced by the writer


def read_frames(path):
    """Yield (offset, frame_dict) from 8-byte-header CRC-framed journal records.
    Verifies CRC32; stops at first torn/corrupt frame (crash-only WAL semantics)."""
    try:
        with open(path, "rb") as f:
            offset = 0
            while True:
                hdr = f.read(8)
                if len(hdr) < 8:
                    return
                n, crc = struct.unpack("<II", hdr)
                if n == 0 or n > MAX_FRAME:
                    return
                payload = f.read(n)
                if len(payload) < n:
                    return  # torn write at crash — expected on crash-only WALs
                if zlib.crc32(payload) != crc:
                    return  # corrupt frame; conservative stop (writer never rewrites)
                try:
                    fr = json.loads(payload)
                except (json.JSONDecodeError, UnicodeDecodeError):
                    yield offset, {"type": "__malformed__", "offset": offset}
                    offset += 8 + n
                    continue
                # Unwrap the envelope: hoist payload fields for projection convenience
                body = fr.get("payload") if isinstance(fr.get("payload"), dict) else {}
                fr["_p"] = body
                yield offset, fr
                offset += 8 + n
    except OSError:
        return


def classify_failure(text):
    t = (text or "").lower()
    for cls, needles in FAILURE_CLASSES.items():
        if any(nd in t for nd in needles):
            return cls
    return "other"


def project_run(run_id, frames):
    """Reduce one run's frames to an episode record with P1-P5 signals."""
    ep = {
        "schema": SCHEMA,
        "run_id": run_id,
        "verifier": {"id": VERIFIER_ID, "version": VERIFIER_VERSION, "tier": "intrinsic"},
        "started_at": None,
        "n_frames": 0,
        "n_tool_calls": 0,
        "n_tool_failures": 0,
        "n_tool_timeouts": 0,
        "n_llm_responses": 0,
        "input_tokens": 0,
        "output_tokens": 0,
        "cost_usd": 0.0,
        "repair_clusters": set(),
        "failure_events": 0,
        "semantic_step_max": 0,
        "semantic_horizon_max": 0,
        "terminal_success": None,   # None = undetermined from journal alone
        "blocked_with_evidence": None,
        "malformed_frames": 0,
        "has_run_end": False,
        "environment_id": None,
        "branch_ids": set(),
        "checkpoint_ids": set(),
        "tool_names": defaultdict(int),
    }

    for _, fr in frames:
        ep["n_frames"] += 1
        ftype = fr.get("type") or fr.get("event_name") or ""
        p = fr.get("_p") or {}
        if ftype == "__malformed__":
            ep["malformed_frames"] += 1
            continue
        if fr.get("wall_ms") and ep["started_at"] is None:
            ep["started_at"] = fr["wall_ms"]
        if ftype == "run.started":
            ep["environment_id"] = p.get("environment_id") or fr.get("environment_id") or ep["environment_id"]
        elif ftype in ("run.ended", "run.completed", "run.failed", "RUN_END", "RUN_ENDED"):
            ep["has_run_end"] = True
            # variant A (agent_event.v1): status in payload.status ("ok"/"error")
            # variant B (legacy): payload.status = "completed"/"failed"
            raw = p.get("status") or fr.get("status") or ""
            if ftype in ("RUN_END", "RUN_ENDED"):
                status = raw or "failed"
            else:
                status = raw or ("success" if "completed" in ftype else "failed")
            ep["terminal_success"] = status in ("success", "ok", "completed")
        elif ftype in ("TOOL_CALL", "tool.call"):
            ep["n_tool_calls"] += 1
            inner = p.get("payload") if isinstance(p.get("payload"), dict) else p
            name = inner.get("tool") or p.get("tool") or "unknown"
            ep["tool_names"][name] += 1
            if p.get("status") not in (None, "ok", "success"):
                ep["n_tool_failures"] += 1
                ep["failure_events"] += 1
                ep["repair_clusters"].add(f"{name}:{classify_failure(str(p.get('status','')))}")
        elif ftype in ("TOOL_RESULT", "tool.result"):
            inner = p.get("payload") if isinstance(p.get("payload"), dict) else p
            ok = inner.get("ok", p.get("status", "ok") in ("ok", "success"))
            timed_out = bool(inner.get("timed_out") or inner.get("timeout"))
            if not ok or timed_out:
                ep["n_tool_failures"] += 1
                ep["failure_events"] += 1
                if timed_out:
                    ep["n_tool_timeouts"] += 1
                res = inner.get("result") if isinstance(inner.get("result"), str) else ""
                tool = inner.get("tool") or "?"
                ep["repair_clusters"].add(f"{tool}:{classify_failure(res)}")
        elif ftype.lower().startswith("tool."):
            # spans-based path (chronicle span events mirrored to journal)
            if ftype.endswith(("start", "begin", "call")):
                ep["n_tool_calls"] += 1
                ep["tool_names"][p.get("tool_name") or p.get("name") or "unknown"] += 1
            elif ftype.endswith(("end", "result")):
                if not p.get("ok", True) or p.get("timeout"):
                    ep["n_tool_failures"] += 1
                    ep["failure_events"] += 1
                    sig = classify_failure(p.get("result_text") or p.get("error") or "")
                    ep["repair_clusters"].add(f"{p.get('tool_name','?')}:{sig}")
        elif ftype == "llm.response.completed" or (ftype.startswith("llm.") and ftype.endswith(("completed", "response"))):
            ep["n_llm_responses"] += 1
            cost = p.get("cost") if isinstance(p.get("cost"), dict) else {}
            ep["input_tokens"] += int(p.get("input_tokens") or cost.get("input_tokens") or 0)
            ep["output_tokens"] += int(p.get("output_tokens") or cost.get("output_tokens") or 0)
            ep["cost_usd"] += float(p.get("cost_usd") or cost.get("usd_delta") or 0.0)
        elif ftype == "rl.trajectory.event":
            rl = p.get("rl_context") if isinstance(p.get("rl_context"), dict) else {}
            cost = p.get("cost") if isinstance(p.get("cost"), dict) else {}
            ep["cost_usd"] += float(cost.get("usd_delta") or 0.0)
            ep["input_tokens"] += int(cost.get("input_tokens") or 0)
            ep["output_tokens"] += int(cost.get("output_tokens") or 0)
            ss = rl.get("semantic_step") or p.get("semantic_step") or 0
            sh = rl.get("semantic_horizon") or p.get("semantic_horizon") or 0
            ep["semantic_step_max"] = max(ep["semantic_step_max"], int(ss))
            ep["semantic_horizon_max"] = max(ep["semantic_horizon_max"], int(sh))
            for key, dest in (("branch_id", "branch_ids"), ("checkpoint_id", "checkpoint_ids")):
                v = rl.get(key) or p.get(key)
                if v:
                    ep[dest].add(v)
            if rl.get("environment_id") and not ep["environment_id"]:
                ep["environment_id"] = rl["environment_id"]

    # Derived persistence components (normalized later, across corpus)
    ep["p2_repair_diversity_raw"] = len(ep["repair_clusters"]) if ep["failure_events"] else 0
    ep["p3_horizon_utilization"] = (
        round(ep["semantic_step_max"] / ep["semantic_horizon_max"], 4)
        if ep["semantic_horizon_max"] > 0 else None
    )
    # P4: ended blocked w/ named evidence vs gave up — requires terminal semantics;
    # leave None unless frames carry it. has_run_end at least distinguishes crash vs exit.
    ep["repair_clusters"] = sorted(ep["repair_clusters"])
    ep["branch_ids"] = sorted(ep["branch_ids"])
    ep["checkpoint_ids"] = sorted(ep["checkpoint_ids"])
    ep["tool_names"] = dict(ep["tool_names"])
    ep["cost_usd"] = round(ep["cost_usd"], 6)
    return ep


def label_examples(ep):
    """Emit training-example candidates from an episode (SFT-filtered upstream).

    Only episodes with terminal_success=True and zero malformed frames become
    positive SFT candidates; failures become preference-pair 'rejected' material
    when a sibling success exists (paired upstream by task_sha)."""
    labels = []
    if ep["terminal_success"] is True and ep["malformed_frames"] == 0:
        labels.append({
            "kind": "sft_candidate",
            "quality_score": 1.0,  # placeholder — re-scored by judge tiers upstream
            "consent_state": "not-consented",  # inherited default; session policy overrides
        })
    elif ep["terminal_success"] is False and ep["has_run_end"]:
        labels.append({
            "kind": "preference_rejected_candidate",
            "quality_score": 0.0,
            "consent_state": "not-consented",
        })
    return labels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", default=os.path.expanduser("~/.dsco/runs"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-runs", type=int, default=0)
    ap.add_argument("--report", default="")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    t0 = time.time()
    n_runs = n_episodes = n_labels = 0
    successes = failures = unknown = 0

    with open(args.out, "w") as out:
        for entry in sorted(os.listdir(args.runs)):
            wal = os.path.join(args.runs, entry, "journal.wal")
            if not os.path.isfile(wal) or os.path.getsize(wal) < 16:
                continue
            n_runs += 1
            if args.max_runs and n_runs > args.max_runs:
                break
            ep = project_run(entry, read_frames(wal))
            if ep["n_frames"] == 0:
                continue
            ep["labels"] = label_examples(ep)
            n_labels += len(ep["labels"])
            n_episodes += 1
            if ep["terminal_success"] is True:
                successes += 1
            elif ep["terminal_success"] is False:
                failures += 1
            else:
                unknown += 1
            out.write(json.dumps(ep) + "\n")

    report = {
        "schema": "dsco.flywheel.label_report.v1",
        "verifier": {"id": VERIFIER_ID, "version": VERIFIER_VERSION},
        "runs_scanned": n_runs,
        "episodes": n_episodes,
        "labels_emitted": n_labels,
        "terminal_success": successes,
        "terminal_failure": failures,
        "terminal_unknown": unknown,
        "elapsed_sec": round(time.time() - t0, 2),
    }
    if args.report:
        with open(args.report, "w") as f:
            json.dump(report, f, indent=2)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
