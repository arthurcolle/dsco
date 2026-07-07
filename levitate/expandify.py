#!/usr/bin/env python3
"""expandify.py — prior-weighted, cost-normalized expansion policy.

Policy:
    score = ((Q + c * P * sqrt(N_parent)/(1+n)) * info * fertility * reversibility) / cost

Use it to choose which tool call / hypothesis / task / feature to expand next.
Stdlib only. Reads JSON array of candidates; prints ranked candidates.

Candidate fields:
  name: str
  q: known value estimate [-inf,+inf], default 0
  prior: policy prior P in [0,1], default 0.5
  visits: child visit count n, default 0
  cost: positive cost, default 1
  info: cross-branch information multiplier, default 1
  fertility: downstream branching/compounding multiplier, default 1
  reversible: bool or [0,1], default true
  irreversible: bool optional alias; if true and evidence < min_evidence => gated
  evidence: confidence/evidence mass [0,inf], default visits

Example:
  python3 levitate/expandify.py <<EOF
  [{"name":"read schema","prior":.9,"cost":1,"info":3},
   {"name":"write patch","q":4,"prior":.6,"cost":8,"irreversible":true,"evidence":0},
   {"name":"grep codebase","prior":.8,"cost":1,"info":2}]
  EOF
"""
from __future__ import annotations
import argparse, json, math, sys


def clamp(x, lo, hi):
    return max(lo, min(hi, x))


def reversibility_weight(c):
    if "reversible" in c:
        v = c["reversible"]
        if isinstance(v, bool):
            return 1.0 if v else 0.25
        return clamp(float(v), 0.0, 1.0)
    if c.get("irreversible"):
        return 0.25
    return 1.0


def score(c, parent_visits=1, exploration=1.4, min_evidence=1.0):
    name = c.get("name", "<unnamed>")
    q = float(c.get("q", 0.0))
    p = clamp(float(c.get("prior", 0.5)), 0.0, 1.0)
    n = max(0.0, float(c.get("visits", 0.0)))
    cost = max(1e-9, float(c.get("cost", 1.0)))
    info = max(0.0, float(c.get("info", c.get("cross_branch_info", 1.0))))
    fertility = max(0.0, float(c.get("fertility", 1.0)))
    rev = reversibility_weight(c)
    evidence = float(c.get("evidence", n))

    gated = False
    gate_reason = None
    if c.get("irreversible") and evidence < min_evidence:
        gated = True
        gate_reason = f"irreversible expansion needs evidence >= {min_evidence}; got {evidence}"

    u = exploration * p * math.sqrt(max(1.0, parent_visits)) / (1.0 + n)
    raw = (q + u) * info * fertility * rev
    s = raw / cost
    if gated:
        s = -math.inf
    return {
        "name": name,
        "score": s,
        "q": q,
        "u": u,
        "cost": cost,
        "info": info,
        "fertility": fertility,
        "reversibility": rev,
        "visits": n,
        "evidence": evidence,
        "gated": gated,
        "gate_reason": gate_reason,
    }


def rank(candidates, parent_visits=None, exploration=1.4, min_evidence=1.0):
    if parent_visits is None:
        parent_visits = max(1.0, sum(float(c.get("visits", 0)) for c in candidates) + 1.0)
    rows = [score(c, parent_visits, exploration, min_evidence) for c in candidates]
    rows.sort(key=lambda r: r["score"], reverse=True)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parent-visits", type=float)
    ap.add_argument("--exploration", type=float, default=1.4)
    ap.add_argument("--min-evidence", type=float, default=1.0)
    ap.add_argument("--json", action="store_true", help="emit JSON instead of table")
    args = ap.parse_args()
    candidates = json.load(sys.stdin)
    rows = rank(candidates, args.parent_visits, args.exploration, args.min_evidence)
    if args.json:
        print(json.dumps(rows, indent=2))
        return
    print(f"{'rank':>4}  {'score':>10}  {'Q':>7}  {'U':>7}  {'cost':>7}  {'info':>6}  {'fert':>6}  {'rev':>5}  name")
    for i, r in enumerate(rows, 1):
        s = "-inf" if r["score"] == -math.inf else f"{r['score']:.4f}"
        suffix = f"  [GATED: {r['gate_reason']}]" if r["gated"] else ""
        print(f"{i:>4}  {s:>10}  {r['q']:>7.2f}  {r['u']:>7.2f}  {r['cost']:>7.2f}  {r['info']:>6.2f}  {r['fertility']:>6.2f}  {r['reversibility']:>5.2f}  {r['name']}{suffix}")


if __name__ == "__main__":
    main()
