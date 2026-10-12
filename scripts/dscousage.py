#!/usr/bin/env python3
"""Unified agent-CLI usage: ccusage (all detected agents) + the dsco inference
cost ledger, deduped by provider response id against Codex session rollouts.

The dsco lane hits the ChatGPT backend directly and never writes
~/.codex/sessions, so its usage is invisible to ccusage. Rows whose request_id
appears in a Codex rollout ARE counted by ccusage's codex loader and get
dropped here; rows with an empty request_id predate id capture and are kept.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, date, timedelta

LEDGER = os.path.expanduser("~/.dsco/inference-costs.jsonl")
RUNS_DIR = os.path.expanduser("~/.dsco/runs")
CODEX_SESSIONS = os.path.expanduser("~/.codex/sessions")
RESP_ID_RE = re.compile(rb"resp_[0-9a-fA-F]{20,}")
COST_MARK = '"schema":"dsco.inference_cost.v1"'


def parse_day(s):
    return datetime.strptime(s, "%Y-%m-%d").date()


def run_ccusage(since, until):
    cmd = ["ccusage", "daily", "--json"]
    if since:
        cmd += ["--since", since.strftime("%Y-%m-%d")]
    if until:
        cmd += ["--until", until.strftime("%Y-%m-%d")]
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=120, check=True)
    except FileNotFoundError:
        print("dscousage: ccusage not found on PATH", file=sys.stderr)
        return {}
    except subprocess.CalledProcessError as e:
        print(f"dscousage: ccusage failed: {e.stderr.decode()[:200]}", file=sys.stderr)
        return {}
    days = {}
    for d in json.loads(out.stdout).get("daily", []):
        days[d["period"]] = {
            "input": d.get("inputTokens", 0),
            "output": d.get("outputTokens", 0),
            "cache_read": d.get("cacheReadTokens", 0),
            "cost": d.get("totalCost", 0.0),
            "agents": (d.get("metadata") or {}).get("agents", []),
        }
    return days


def codex_resp_ids(since, until):
    """Response ids from Codex rollout files, bounded by the session dir's
    YYYY/MM/DD layout (padded a day each way for timezone skew)."""
    ids = set()
    if not os.path.isdir(CODEX_SESSIONS):
        return ids
    lo = (since - timedelta(days=1)) if since else date.min
    hi = (until + timedelta(days=1)) if until else date.max
    for root, _dirs, files in os.walk(CODEX_SESSIONS):
        rel = os.path.relpath(root, CODEX_SESSIONS)
        parts = rel.split(os.sep)
        if len(parts) == 3:
            try:
                d = date(int(parts[0]), int(parts[1]), int(parts[2]))
            except ValueError:
                continue
            if not (lo <= d <= hi):
                continue
        elif rel != "." and len(parts) < 3:
            continue
        for f in files:
            if not f.endswith(".jsonl"):
                continue
            try:
                with open(os.path.join(root, f), "rb") as fh:
                    ids.update(m.group().decode() for m in RESP_ID_RE.finditer(fh.read()))
            except OSError:
                continue
    return ids


def iter_fallback_rows():
    if not os.path.exists(LEDGER):
        return
    with open(LEDGER) as fh:
        for line in fh:
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue


def iter_journal_rows(since, until):
    """Cost records from run journals (~/.dsco/runs/*/journal.wal) — the
    primary sink when the Chronicle journal is enabled; the flat jsonl is
    only its fallback. WAL frames are length-prefixed binary around JSON, so
    decode leniently and raw_decode from each schema marker."""
    if not os.path.isdir(RUNS_DIR):
        return
    lo = None
    if since:
        lo = datetime(since.year, since.month, since.day).timestamp() - 86400
    decoder = json.JSONDecoder()
    for entry in os.scandir(RUNS_DIR):
        wal = os.path.join(entry.path, "journal.wal")
        try:
            st = os.stat(wal)
        except OSError:
            continue
        if lo and st.st_mtime < lo:
            continue
        try:
            with open(wal, "rb") as fh:
                text = fh.read().decode("utf-8", errors="replace")
        except OSError:
            continue
        idx = 0
        while True:
            idx = text.find(COST_MARK, idx)
            if idx == -1:
                break
            start = text.rfind("{", 0, idx)
            try:
                obj, _end = decoder.raw_decode(text, start)
                yield obj
            except (json.JSONDecodeError, ValueError):
                pass
            idx += len(COST_MARK)


def load_dsco(since, until, seen_ids):
    days = defaultdict(lambda: {"n": 0, "input": 0, "output": 0, "cache_read": 0,
                                "notional": 0.0, "billable": 0.0, "deduped": 0,
                                "lanes": set()})
    dsco_seen = set()
    for r in list(iter_fallback_rows()) + list(iter_journal_rows(since, until)):
        day = datetime.fromtimestamp(r.get("timestamp_unix", 0)).date()
        if (since and day < since) or (until and day > until):
            continue
        key = day.strftime("%Y-%m-%d")
        rid = r.get("request_id") or ""
        if rid and (rid in seen_ids or rid in dsco_seen):
            days[key]["deduped"] += 1
            continue
        if rid:
            dsco_seen.add(rid)
        usd = r.get("provider_reported_usd")
        if usd is None:
            usd = r.get("estimated_inference_usd") or 0.0
        d = days[key]
        d["n"] += 1
        d["input"] += r.get("input_tokens", 0)
        d["output"] += r.get("output_tokens", 0)
        d["cache_read"] += r.get("cache_read_tokens", 0)
        d["notional"] += usd
        if not r.get("subscription_included"):
            d["billable"] += usd
        d["lanes"].add(r.get("billing_lane") or "unknown")
    return days


def main():
    ap = argparse.ArgumentParser(description="ccusage + dsco ledger, deduped")
    ap.add_argument("-s", "--since", type=parse_day)
    ap.add_argument("-u", "--until", type=parse_day)
    ap.add_argument("-j", "--json", action="store_true", dest="as_json")
    args = ap.parse_args()

    cc = run_ccusage(args.since, args.until)
    seen = codex_resp_ids(args.since, args.until)
    dsco = load_dsco(args.since, args.until, seen)

    all_days = sorted(set(cc) | set(dsco))
    rows, totals = [], defaultdict(float)
    for day in all_days:
        c = cc.get(day, {})
        d = dsco.get(day, {})
        row = {
            "day": day,
            "cc_input": c.get("input", 0), "cc_output": c.get("output", 0),
            "cc_cost": c.get("cost", 0.0), "cc_agents": c.get("agents", []),
            "dsco_requests": d.get("n", 0),
            "dsco_input": d.get("input", 0), "dsco_output": d.get("output", 0),
            "dsco_notional": d.get("notional", 0.0),
            "dsco_billable": d.get("billable", 0.0),
            "dsco_deduped": d.get("deduped", 0),
            "dsco_lanes": sorted(d.get("lanes", ())),
            "billable_total": c.get("cost", 0.0) + d.get("billable", 0.0),
        }
        rows.append(row)
        for k in ("cc_cost", "dsco_notional", "dsco_billable", "billable_total"):
            totals[k] += row[k]
        for k in ("cc_input", "cc_output", "dsco_input", "dsco_output",
                  "dsco_requests", "dsco_deduped"):
            totals[k] += row[k]

    if args.as_json:
        json.dump({"daily": rows, "totals": dict(totals)}, sys.stdout, indent=2)
        print()
        return

    hdr = (f"{'Day':<11}{'ccusage in/out':>22}{'cc $':>9}"
           f"{'dsco in/out':>22}{'notional $':>11}{'billable $':>11}{'dedup':>6}")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        print(f"{r['day']:<11}"
              f"{r['cc_input']:>13,}/{r['cc_output']:<8,}"
              f"{r['cc_cost']:>9.2f}"
              f"{r['dsco_input']:>13,}/{r['dsco_output']:<8,}"
              f"{r['dsco_notional']:>11.2f}{r['dsco_billable']:>11.2f}"
              f"{r['dsco_deduped']:>6}")
    print("-" * len(hdr))
    print(f"{'TOTAL':<11}"
          f"{int(totals['cc_input']):>13,}/{int(totals['cc_output']):<8,}"
          f"{totals['cc_cost']:>9.2f}"
          f"{int(totals['dsco_input']):>13,}/{int(totals['dsco_output']):<8,}"
          f"{totals['dsco_notional']:>11.2f}{totals['dsco_billable']:>11.2f}"
          f"{int(totals['dsco_deduped']):>6}")
    print(f"\nBillable total (ccusage cost + dsco billable): "
          f"${totals['billable_total']:.2f}   "
          f"dsco notional absorbed by subscriptions: "
          f"${totals['dsco_notional'] - totals['dsco_billable']:.2f}")


if __name__ == "__main__":
    main()
