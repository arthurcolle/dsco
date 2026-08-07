"""Prove the overfitting is fixed — the honest anti-shortcut dashboard (swarm methodology).

Three measurements, v1 (templated expanded_10k) vs v2 (high-variation expanded_v2):

  1. TOOL-NAME SHORTCUT PROBE — train a classifier on the tool BINARY alone (one-hot),
     random split. If tool-name predicts the label, that's the leak that inflated AUC.
     Want ~0.50 (chance). v2 uses every tool for BOTH labels, so it should collapse to chance;
     v1's fixed per-family tool names should score high.
  2. HELD-OUT-TOOL GENERALIZATION — train a char-ngram model on the train-split tool binaries,
     test on DISJOINT test-split binaries (unseen at train). This is real cross-tool-name
     generalization; a memorizer collapses, a concept-learner holds.
  3. MINIMAL-PAIR CONTRAST-CONSISTENCY — for byte-identical allow/deny pairs differing only in
     the discriminating field, credit only if BOTH are right. The honesty metric.

Plots -> ml/plots/overfit_*.png (+ .html). Run: python eval_overfit.py
"""
import json
import os

import numpy as np
import plotly.graph_objects as go
from plotly.subplots import make_subplots
from sklearn.feature_extraction.text import CountVectorizer, TfidfVectorizer
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import roc_auc_score

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "..", "..", "data", "cap_classifier")
PLOTS = os.path.join(HERE, "plots")
rng = np.random.default_rng(0)
_TIER = {"untrusted": 0, "standard": 1, "trusted": 2}


def _as_str(x):
    return x if isinstance(x, str) else json.dumps(x)


def derive_cap_tool(r):
    if r.get("cap_tool"):
        return r["cap_tool"]
    try:
        d = json.loads(r["input"]) if isinstance(r["input"], str) else r["input"]
    except Exception:
        d = {}
    for k in ("command", "cmd", "script", "run", "code", "c", "exec"):
        if isinstance(d, dict) and d.get(k):
            return str(d[k]).split()[0]
    return r.get("tool", "?")


def load(name):
    rows = []
    with open(os.path.join(DATA, name)) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            inp = _as_str(r.get("input", ""))
            pt = r.get("pre_taint", {}) or {}
            rows.append({"tool": r.get("tool", ""), "input": inp,
                         "text": f'{r.get("tool","")} {inp}', "cap_tool": derive_cap_tool(r),
                         "tainted": int(pt.get("untrusted", r.get("tainted", 0))),
                         "private": int(pt.get("private", r.get("private", 0))),
                         "tier": _TIER.get(r.get("tier"), 1),
                         "y": 1 if r.get("expected_decision", r.get("decision")) == "deny" else 0,
                         "cat": r.get("expected_category", "?"), "split": r.get("split"),
                         "pair_id": r.get("pair_id")})
    return rows


def _num(rows):
    return np.array([[r["tainted"], r["private"], r["tier"]] for r in rows], float)


def tool_probe_auc(rows):
    """Can the tool BINARY alone predict the label? (random split). Want ~0.5."""
    idx = np.arange(len(rows)); rng.shuffle(idx)
    cut = int(0.8 * len(rows))
    tr = [rows[i] for i in idx[:cut]]; te = [rows[i] for i in idx[cut:]]
    vec = CountVectorizer(analyzer=lambda s: [s])          # one-hot of the tool binary
    Xtr = vec.fit_transform([r["cap_tool"] for r in tr]); Xte = vec.transform([r["cap_tool"] for r in te])
    ytr = np.array([r["y"] for r in tr]); yte = np.array([r["y"] for r in te])
    if len(set(ytr)) < 2 or len(set(yte)) < 2:
        return float("nan")
    clf = LogisticRegression(max_iter=1000, class_weight="balanced").fit(Xtr, ytr)
    return roc_auc_score(yte, clf.predict_proba(Xte)[:, 1])


def _text_model(tr):
    vec = TfidfVectorizer(analyzer="char_wb", ngram_range=(3, 5), min_df=2, max_features=40000)
    import scipy.sparse as sp
    Xt = vec.fit_transform([r["text"] for r in tr])
    X = sp.hstack([Xt, sp.csr_matrix(_num(tr))]).tocsr()
    clf = LogisticRegression(max_iter=2000, C=4.0, class_weight="balanced").fit(X, [r["y"] for r in tr])
    return vec, clf


def _text_pred(vec, clf, rows):
    import scipy.sparse as sp
    Xt = vec.transform([r["text"] for r in rows])
    X = sp.hstack([Xt, sp.csr_matrix(_num(rows))]).tocsr()
    return clf.predict(X), clf.predict_proba(X)[:, 1]


def heldout_tool_generalization(rows):
    """Train on train-split tool binaries, test on DISJOINT test-split binaries."""
    if rows[0]["split"]:
        tr = [r for r in rows if r["split"] == "train"]
        te = [r for r in rows if r["split"] == "test"]
    else:  # v1 has no split field: construct a held-out-tool split by hashing the binary
        import hashlib
        def hs(t): return int(hashlib.blake2b(t.encode(), digest_size=4).hexdigest(), 16) % 5
        tr = [r for r in rows if hs(r["cap_tool"]) >= 1]
        te = [r for r in rows if hs(r["cap_tool"]) == 0]
    tr_tools = {r["cap_tool"] for r in tr}
    te = [r for r in te if r["cap_tool"] not in tr_tools]  # enforce disjoint
    vec, clf = _text_model(tr)
    pred, proba = _text_pred(vec, clf, te)
    y = np.array([r["y"] for r in te])
    acc = float((pred == y).mean())
    d = y == 1
    recall = float((pred[d] == 1).mean()) if d.any() else float("nan")
    auc = roc_auc_score(y, proba) if len(set(y)) == 2 else float("nan")
    return {"acc": acc, "deny_recall": recall, "auc": auc, "n_test": len(te),
            "n_test_tools": len({r["cap_tool"] for r in te})}


def contrast_consistency(rows):
    """Minimal pairs: model (trained on train split) must get BOTH members right."""
    tr = [r for r in rows if r["split"] == "train"]
    vec, clf = _text_model(tr)
    from collections import defaultdict
    groups = defaultdict(list)
    for r in rows:
        if r.get("pair_id") and r["split"] in ("val", "test"):
            groups[r["pair_id"]].append(r)
    pairs = [g for g in groups.values() if len(g) >= 2]
    if not pairs:
        return float("nan"), 0
    ok = 0
    for g in pairs:
        pred, _ = _text_pred(vec, clf, g)
        if all(int(p) == r["y"] for p, r in zip(pred, g)):
            ok += 1
    return ok / len(pairs), len(pairs)


def main():
    os.makedirs(PLOTS, exist_ok=True)
    v1 = load("expanded_10k.jsonl")
    v2 = load("expanded_v2.jsonl")

    print("=== 1. TOOL-NAME SHORTCUT PROBE (tool binary alone -> label; want ~0.50) ===")
    a1, a2 = tool_probe_auc(v1), tool_probe_auc(v2)
    print(f"  v1 (templated)      tool-only AUC: {a1:.3f}   {'<- LEAK' if a1 > 0.65 else ''}")
    print(f"  v2 (high-variation) tool-only AUC: {a2:.3f}   {'(shortcut removed)' if a2 < 0.6 else ''}")

    print("\n=== 2. HELD-OUT-TOOL GENERALIZATION (unseen tool binaries at test) ===")
    g1, g2 = heldout_tool_generalization(v1), heldout_tool_generalization(v2)
    print(f"  v1: acc {g1['acc']:.3f}  deny-recall {g1['deny_recall']:.3f}  auc {g1['auc']:.3f}  "
          f"(test n={g1['n_test']}, {g1['n_test_tools']} unseen tools)")
    print(f"  v2: acc {g2['acc']:.3f}  deny-recall {g2['deny_recall']:.3f}  auc {g2['auc']:.3f}  "
          f"(test n={g2['n_test']}, {g2['n_test_tools']} unseen tools)")

    print("\n=== 3. MINIMAL-PAIR CONTRAST-CONSISTENCY (v2; both members right) ===")
    cc, npairs = contrast_consistency(v2)
    print(f"  v2 contrast-consistency: {cc:.3f}  over {npairs} held-out minimal pairs")

    # plot: v1 vs v2 across the three honest metrics
    fig = make_subplots(rows=1, cols=3, subplot_titles=(
        "Tool-name shortcut AUC<br>(lower=better, 0.5=no leak)",
        "Held-out-tool deny-recall<br>(higher=better)",
        "Minimal-pair contrast-consistency<br>(v2)"))
    fig.add_trace(go.Bar(x=["v1 templated", "v2 varied"], y=[a1, a2],
                         marker_color=["#d62728", "#2ca02c"], text=[f"{a1:.2f}", f"{a2:.2f}"],
                         textposition="outside"), 1, 1)
    fig.add_hline(y=0.5, line_dash="dash", line_color="gray", row=1, col=1)
    fig.add_trace(go.Bar(x=["v1 templated", "v2 varied"], y=[g1["deny_recall"], g2["deny_recall"]],
                         marker_color=["#d62728", "#2ca02c"],
                         text=[f"{g1['deny_recall']:.2f}", f"{g2['deny_recall']:.2f}"],
                         textposition="outside"), 1, 2)
    fig.add_trace(go.Bar(x=["v2 varied"], y=[cc], marker_color=["#1f77b4"],
                         text=[f"{cc:.2f}"], textposition="outside"), 1, 3)
    fig.update_yaxes(range=[0, 1.05])
    fig.update_layout(title="Overfitting audit: templated (v1) vs high-variation (v2)",
                      template="plotly_white", height=460, showlegend=False)
    fig.write_html(os.path.join(PLOTS, "overfit_audit.html"))
    try:
        fig.write_image(os.path.join(PLOTS, "overfit_audit.png"), width=1200, height=460, scale=2)
    except Exception as e:
        print(f"  (png export skipped: {e})")
    print(f"\nsaved overfit_audit.(html|png) -> {os.path.relpath(PLOTS, HERE)}/")


if __name__ == "__main__":
    main()
