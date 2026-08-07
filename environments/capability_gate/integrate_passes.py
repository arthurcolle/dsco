"""Integrate the swarm passes: replay every agent-produced labeled case through the gate
to (a) find new gate bugs (gate decision != agent's expected), and (b) emit merged
adversarial/hard splits. Defensive to per-file schema drift.

Case files (JSONL) come in two shapes:
  * standalone : has `pre_taint` {untrusted,private} + optional `env` {VAR:val}
  * session    : has `session` + `turn` (+ optional `variant`); replay in turn order
Both carry `expected_decision` (and usually `expected_category`, `difficulty`).

Usage: integrate_passes.py PASSES_DIR OUT_DIR
"""
import glob
import json
import os
import sys
from collections import defaultdict

from dsco_gate import Session, gate


def _norm(row):
    inp = row.get("input", "{}")
    if not isinstance(inp, str):
        inp = json.dumps(inp)
    return {
        "tool": row.get("tool"),
        "input": inp,
        "tier": row.get("tier", "trusted"),
        "expected": row.get("expected_decision") or row.get("expected"),
        "expected_category": row.get("expected_category"),
        "difficulty": row.get("difficulty", "unlabeled"),
        "env": row.get("env") or {},
        "session": row.get("session"),
        "turn": row.get("turn"),
        "variant": row.get("variant"),
        "src": row.get("_src"),
        "note": row.get("rationale") or row.get("note"),
    }


def _apply_env(env):
    old = {}
    for k, v in (env or {}).items():
        old[k] = os.environ.get(k)
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = str(v)
    return old


def _restore_env(old):
    for k, v in old.items():
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = v


def run(passes_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    rows = []
    for path in sorted(glob.glob(os.path.join(passes_dir, "*.jsonl"))):
        src = os.path.basename(path)
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    r = json.loads(line)
                    r["_src"] = src
                    rows.append(_norm(r))
                except Exception:
                    pass

    standalone = [r for r in rows if r["session"] is None and r["tool"]]
    sess_rows = [r for r in rows if r["session"] is not None and r["tool"]]

    merged, disagree = [], []
    agree = total = 0
    by_diff = defaultdict(lambda: [0, 0])   # difficulty -> [agree, total]
    by_src = defaultdict(lambda: [0, 0])

    def record(r, got):
        nonlocal agree, total
        total += 1
        ok = (r["expected"] is None) or (got["decision"] == r["expected"])
        agree += ok
        by_diff[r["difficulty"]][1] += 1; by_diff[r["difficulty"]][0] += ok
        by_src[r["src"]][1] += 1; by_src[r["src"]][0] += ok
        out = {"tool": r["tool"], "input": r["input"], "tier": r["tier"],
               "tainted": got["tainted"], "private": got["private"],
               "egress": got["egress"], "risk": got["risk"],
               "gate_decision": got["decision"], "gate_category": got["category"],
               "expected_decision": r["expected"], "difficulty": r["difficulty"],
               "src": r["src"], "session": r["session"], "turn": r["turn"], "note": r["note"]}
        merged.append(out)
        if not ok:
            disagree.append(out)

    # standalone cases — re-read raw for pre_taint fidelity (it isn't kept in _norm)
    for path in sorted(glob.glob(os.path.join(passes_dir, "*.jsonl"))):
        src = os.path.basename(path)
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    raw = json.loads(line)
                except Exception:
                    continue
                if raw.get("session") is not None or not raw.get("tool"):
                    continue
                r = _norm({**raw, "_src": src})
                old = _apply_env(r["env"])
                s = Session()
                pt = raw.get("pre_taint")
                if not isinstance(pt, dict):
                    pt = {}
                if pt.get("untrusted"):
                    s.tainted_untrusted = True
                if pt.get("private"):
                    s.accessed_private = True
                got = gate(r["tool"], r["input"], r["tier"], s)
                record(r, got)
                _restore_env(old)

    # session cases: replay each session in turn order (fresh Session accumulates)
    groups = defaultdict(list)
    for r in sess_rows:
        groups[(r["src"], r["session"], r["variant"])].append(r)
    for key, turns in groups.items():
        turns.sort(key=lambda x: (x["turn"] if x["turn"] is not None else 0))
        s = Session()
        for r in turns:
            old = _apply_env(r["env"])
            got = gate(r["tool"], r["input"], r["tier"], s)
            record(r, got)
            _restore_env(old)

    with open(os.path.join(out_dir, "adversarial.jsonl"), "w") as f:
        for m in merged:
            f.write(json.dumps(m) + "\n")
    with open(os.path.join(out_dir, "gate_disagreements.jsonl"), "w") as f:
        for d in disagree:
            f.write(json.dumps(d) + "\n")

    print(f"cases replayed: {total}   gate==expected: {agree} ({agree/total:.1%})   "
          f"disagreements (candidate bugs): {len(disagree)}")
    print("\nby difficulty (agreement):")
    for k, (a, t) in sorted(by_diff.items()):
        print(f"  {k:12} {a}/{t}  {a/t:.0%}")
    print("\nby source pass:")
    for k, (a, t) in sorted(by_src.items()):
        print(f"  {k:34} {a}/{t}  {a/t:.0%}")
    print(f"\nwrote {out_dir}/adversarial.jsonl ({len(merged)}), gate_disagreements.jsonl ({len(disagree)})")
    return len(disagree)


if __name__ == "__main__":
    pd = sys.argv[1] if len(sys.argv) > 1 else "../../data/cap_classifier/passes"
    od = sys.argv[2] if len(sys.argv) > 2 else "../../data/cap_classifier/adversarial"
    sys.exit(0 if run(pd, od) == 0 else 0)
