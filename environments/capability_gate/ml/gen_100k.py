"""Assemble the 100k capability-gate dataset — gen_v2 families + PCFG grammars + adversarial harvest.

Task-3 of the scale-up. Combines four deterministic sources into a single balanced 100,000-row
corpus, written to data/cap_classifier/expanded_100k.jsonl (+ .lock.json). Never touches
expanded_v2.jsonl. All randomness flows through varcache (blake2b-keyed, no global seed) and the
harvest is scored against a fixed model file, so the whole run is bit-reproducible: same run =>
identical row_keys_digest in the lock.

Sources (priority-filled to exactly 50k deny + 50k allow, so the label is balanced by
construction and the highest-value rows are never crowded out):
  1. HARVEST  — adversarial co-evolution hard cases (harvest.py), security-correct labels.
  2. GRAMMAR  — PCFG productions per family (grammars.py), combinatorial surface, pure-axis label.
  3. VARIETY  — extra minimal-pair + grant-context rows from gen_v2 (contrastive boundary).
  4. GEN_V2   — the base axis-factored families, scaled up to fill the remainder.

Every source emits the same row schema and keeps the tsplit() tool-binary holdout, so
train∩test tool-name leakage stays 0 across the union. Run: python gen_100k.py [N]
"""
import json
import os
import sys
from collections import Counter, defaultdict

# Widen the mined tool-binary pool BEFORE gen_v2/corpora_banks import (env is read at their
# import time). gen_v2 defaults to 40/kind — tuned so the tool-name shortcut probe reads ~0.5 at
# 12k rows. At 100k rows every tool still gets enough both-label samples, so we lift the cap to
# surface the full vendored LOLBAS/GTFOBins/MCP binary space (>>930 distinct cap-tools) while the
# hash-split holdout keeps train∩test tool leakage at exactly 0. Pinned for reproducibility.
os.environ["CORPORA_MAX_PER_KIND"] = os.environ.get("CORPORA_MAX_PER_KIND", "800")

import gen_v2 as gv
import grammars as gr
import harvest as hv
import varcache as vc

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "..", "data", "cap_classifier", "expanded_100k.jsonl")
LOCK = os.path.join(HERE, "..", "..", "..", "data", "cap_classifier", "expanded_100k.lock.json")

# gen_v2 families whose rows are minimal pairs or grant-context (extra contrastive variety)
VARIETY_FAMS = [gv.fam_grant_context, gv.fam_http_exfil, gv.fam_supply, gv.fam_destructive,
                gv.fam_cloud, gv.fam_sandbox, gv.fam_benign_lan]


def _row_key(r):
    """Stable content-addressed key per row (order-independent digest via write_lock)."""
    return vc.recipe_key({"tool": r["tool"], "input": r["input"],
                          "dec": r["expected_decision"], "fam": r["family"]})


def _genv2_rows(fam, i):
    """One gen_v2 family production at index i, tagged with its family name (as gv.build does)."""
    split = vc.split_of(f"{fam.__name__}:{i}", "rowsplit")
    rng = vc.axis_rng(fam.__name__, i)
    out = []
    for r in fam(rng, split, i):
        r["family"] = fam.__name__
        out.append(r)
    return out


def build(n=100000, model_path=hv.DEFAULT_MODEL, seed=0, n_grammar=40000, n_variety=8000):
    half = n // 2
    deny, allow = [], []
    src_counts = Counter()

    def push(r, src):
        bucket = deny if r["expected_decision"] == "deny" else allow
        if len(bucket) < half:
            bucket.append(r)
            src_counts[src] += 1
            return True
        return False

    # 1. HARVEST (highest value — never crowded out)
    harvested, hstats = hv.harvest(model_path=model_path, seed=seed)
    for r in harvested:
        push(r, "harvest")

    # 2. GRAMMAR productions
    for r in gr.grammar_rows(n_grammar):
        push(r, "grammar")

    # 3. VARIETY — extra minimal-pair + grant-context contrast (distinct idx namespace)
    vi = 0
    while src_counts["variety"] < n_variety and (len(deny) < half or len(allow) < half):
        fam = VARIETY_FAMS[vi % len(VARIETY_FAMS)]
        for r in _genv2_rows(fam, 10_000_000 + vi):   # namespace offset: no idx collision with fill
            push(r, "variety")
        vi += 1

    # 4. GEN_V2 base families — fill the remainder to exactly half/half
    i = 0
    while len(deny) < half or len(allow) < half:
        fam = gv.FAMILIES[i % len(gv.FAMILIES)]
        for r in _genv2_rows(fam, i):
            push(r, "genv2")
        i += 1

    rows = deny[:half] + allow[:half]
    keys = [_row_key(r) for r in rows]

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as fh:
        for r in rows:
            fh.write(json.dumps(r) + "\n")
    counts = Counter(r["expected_category"] for r in rows)
    lock = vc.write_lock(LOCK, vc.MASTER_SEED, counts, gv.ALL_BANKS, keys, extra={
        "dataset": "expanded_100k", "sources": dict(src_counts),
        "families": sorted({r["family"] for r in rows}),
        "grammar_families": gr.GFAMILIES,
        "harvest": {k: hstats[k] for k in ("model", "harvested", "flips_total", "flips_by_family",
                                           "proba_exfil_max", "proba_benign_min")},
    })
    _report(rows, src_counts, hstats, lock)
    return rows, lock


def _report(rows, src_counts, hstats, lock):
    dec = Counter(r["expected_decision"] for r in rows)
    tools = {r["cap_tool"] for r in rows}
    texts = [f'{r["tool"]} {r["input"]}' for r in rows]
    H = vc.ngram_entropy(texts)
    nd = vc.near_dup_rate(texts)
    tbs = defaultdict(set)
    for r in rows:
        tbs[r["split"]].add(r["cap_tool"])
    leak = tbs["train"] & tbs["test"]
    fam = Counter(r["family"] for r in rows)

    print(f"wrote {len(rows)} rows -> {os.path.relpath(OUT, HERE)}")
    print(f"decisions        : {dict(dec)}  (balanced)")
    print(f"sources          : {dict(src_counts)}")
    print(f"distinct cap-tools: {len(tools)}   (want >> 930)")
    print(f"char-3gram entropy: {H:.2f}   (want >= 7.31)")
    print(f"near-dup rate    : {nd:.4f}")
    print(f"train∩test tool leak: {len(leak)}   (MUST be 0)")
    print(f"distinct (tool,input): {len({(r['tool'], r['input']) for r in rows})}/{len(rows)}")
    print(f"row_keys_digest  : {lock['row_keys_digest']}")
    print("--- harvest (adversarial co-evolution) ---")
    print(f"  model {hstats['model']}  harvested {hstats['harvested']}  "
          f"wrong-decision flips {hstats['flips_total']}")
    print(f"  flips/family {hstats['flips_by_family']}")
    print(f"  exfil ALLOW-proba max {hstats['proba_exfil_max']:.4f}  "
          f"benign ALLOW-proba min {hstats['proba_benign_min']:.4f}")
    print("--- grammar coverage ---")
    print(f"  families {gr.GFAMILIES}")
    print("--- per-family counts ---")
    for k, v in sorted(fam.items(), key=lambda x: -x[1]):
        print(f"  {k:22} {v}")
    print("--- per-category counts ---")
    for k, v in sorted(Counter(r['expected_category'] for r in rows).items(), key=lambda x: -x[1]):
        print(f"  {k:22} {v}")


if __name__ == "__main__":
    build(int(sys.argv[1]) if len(sys.argv) > 1 else 100000)
