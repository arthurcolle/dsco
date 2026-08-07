"""Fix cross-novel-concept generalization: withhold an ENTIRE risk family and measure
deny-recall on it, across four representations —

  ng   : char-ngram tf-idf (the surface baseline that fails — vocabulary is unseen OOD)
  eff  : 8 cross-cutting effect features (shared abstraction; transfers when the effect
         appears in a family that stays in training)
  emb  : pretrained MiniLM sentence embedding (TRANSFER learning — the embedding already
         encodes that `rm -rf /`, `iam create-access-key`, `--privileged` are dangerous
         from web-scale pretraining, independent of our training vocabulary)
  emb+eff : embeddings + effect features (both levers)

Plot -> ml/plots/concept_generalization.png (+ .html). Run: python concept_generalization.py
"""
import json
import os
import sys

import numpy as np
import plotly.graph_objects as go
from sklearn.calibration import CalibratedClassifierCV
from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.linear_model import LogisticRegression

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from effect_features import effect_features  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "..", "..", "data", "cap_classifier")
PLOTS = os.path.join(HERE, "plots")


def load():
    rows = []
    with open(os.path.join(DATA, "expanded_v2.jsonl")) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            inp = r["input"] if isinstance(r["input"], str) else json.dumps(r["input"])
            # withhold by FAMILY (surface-distinct concept), not category — so a sibling that
            # shares the same abstract effect (e.g. db-destruct for fs-destruct) stays in train.
            rows.append({"text": f'{r.get("tool","")} {inp}', "tool": r.get("tool", ""),
                         "input": inp, "y": 1 if r["expected_decision"] == "deny" else 0,
                         "cat": r.get("family") or r.get("expected_category", "?"),
                         "eff": effect_features(r.get("tool", ""), inp)})
    return rows


def embed(rows):
    from sentence_transformers import SentenceTransformer
    import torch
    dev = "mps" if torch.backends.mps.is_available() else "cpu"
    m = SentenceTransformer("all-MiniLM-L6-v2", device=dev)
    E = m.encode([r["text"] for r in rows], batch_size=256, normalize_embeddings=True,
                 show_progress_bar=False)
    for r, e in zip(rows, E):
        r["emb"] = e.astype(np.float32)
    return rows


def deny_proba(rows, cat, feat):
    """Deny-probabilities on the WITHHELD family's DENY rows and its ALLOW rows, so we can
    score both leave-one-family-out deny-recall AND over-refusal on the family's benign
    variants. Returns (proba_deny, proba_allow) or None if either side is too small."""
    import scipy.sparse as sp
    tr = [r for r in rows if r["cat"] != cat]
    te_d = [r for r in rows if r["cat"] == cat and r["y"] == 1]
    te_a = [r for r in rows if r["cat"] == cat and r["y"] == 0]
    if len(te_d) < 5 or len(te_a) < 5:
        return None
    ytr = np.array([r["y"] for r in tr])

    def X(subset, fit_vec=None):
        parts = []
        if feat in ("ng", "ng+eff", "all"):
            if fit_vec is None:
                fit_vec = TfidfVectorizer(analyzer="char_wb", ngram_range=(3, 5), min_df=2,
                                          max_features=40000)
                parts.append(fit_vec.fit_transform([r["text"] for r in subset]))
            else:
                parts.append(fit_vec.transform([r["text"] for r in subset]))
        if feat in ("emb", "emb+eff", "all"):
            parts.append(sp.csr_matrix(np.vstack([r["emb"] for r in subset])))
        if feat in ("eff", "ng+eff", "emb+eff", "all"):
            parts.append(sp.csr_matrix(np.array([r["eff"] for r in subset], float)))
        return sp.hstack(parts).tocsr(), fit_vec

    Xtr, vec = X(tr)
    base = LogisticRegression(max_iter=2000, C=4.0, class_weight="balanced")
    # ISOTONIC-calibrate each expert to true frequencies before pooling, so the max over hops
    # is over comparable probabilities (uncalibrated LR is over-confident -> max over-refuses).
    clf = CalibratedClassifierCV(base, method="isotonic", cv=3).fit(Xtr, ytr)
    pd_ = clf.predict_proba(X(te_d, vec)[0])[:, 1]
    pa_ = clf.predict_proba(X(te_a, vec)[0])[:, 1]
    return pd_, pa_


def main():
    os.makedirs(PLOTS, exist_ok=True)
    rows = load()
    if len(rows) > 24000:            # the per-family analysis doesn't need the full 64k
        idx = np.random.default_rng(0).permutation(len(rows))[:24000]
        rows = [rows[i] for i in idx]
    print(f"embedding {len(rows)} rows with MiniLM (transfer)...")
    rows = embed(rows)
    cats = sorted({r["cat"] for r in rows if r["y"] == 1 and
                   sum(1 for x in rows if x["cat"] == r["cat"] and x["y"] == 1) >= 5})
    # base representations + two combiners: "all" = concat-into-one-LR, "max" = MAX-POOL of the
    # per-representation calibrated deny-probs (routes each family to its most confident expert,
    # so the embedding's ignorance can't dilute the effect signal — recovers payment).
    # max-pool over experts is BIAS-RECOVERY: each regularized LR shrinks toward 0.5 and under-
    # detects confident denies; taking the max over hops recovers the signal regularization threw
    # away. The question is whether that recovered mass is TRUE denies (good) or false positives on
    # the withheld family's benign variants (over-refusal) — so we score BOTH deny-recall AND
    # over-refusal (deny-rate on the withheld family's ALLOW rows).
    feats = ["ng", "eff", "emb", "all", "max", "stack"]
    colors = {"ng": "#d62728", "eff": "#ff7f0e", "emb": "#1f77b4", "all": "#2ca02c",
              "max": "#9467bd", "stack": "#111111"}
    rec = {f: {} for f in feats}   # deny-recall on withheld denies (want high)
    over = {f: {} for f in feats}  # over-refusal on withheld allows (want low)
    sweep = {"max_d": [], "max_a": [], "all_d": [], "all_a": []}  # pooled probs for the tau-sweep
    E_d, E_a = {}, {}              # per-family [n x 3] expert-prob matrices for the learned stacker
    print(f"\nleave-one-CONCEPT-out  deny-recall / over-refusal  ({len(cats)} families):")
    print(f"  {'family':18} " + " ".join(f"{f:>11}" for f in feats))
    for c in cats:
        pr = {f: deny_proba(rows, c, f) for f in ("ng", "eff", "emb", "all")}
        if any(pr[f] is None for f in ("ng", "eff", "emb", "all")):
            continue
        for f in ("ng", "eff", "emb", "all"):
            pd_, pa_ = pr[f]
            rec[f][c] = float((pd_ >= .5).mean())
            over[f][c] = float((pa_ >= .5).mean())
        pdm = np.maximum.reduce([pr[k][0] for k in ("ng", "eff", "emb")])  # max-pool deny rows
        pam = np.maximum.reduce([pr[k][1] for k in ("ng", "eff", "emb")])  # max-pool allow rows
        rec["max"][c] = float((pdm >= .5).mean())
        over["max"][c] = float((pam >= .5).mean())
        sweep["max_d"].append(pdm); sweep["max_a"].append(pam)
        sweep["all_d"].append(pr["all"][0]); sweep["all_a"].append(pr["all"][1])
        E_d[c] = np.column_stack([pr[k][0] for k in ("ng", "eff", "emb")])
        E_a[c] = np.column_stack([pr[k][1] for k in ("ng", "eff", "emb")])
        print(f"  {c:18} " + " ".join(f"{rec[f][c]:4.0%}/{over[f][c]:3.0%}"
                                       for f in ("ng", "eff", "emb", "all", "max")))

    # LEARNED STACKER: a meta-LR over the 3 calibrated experts' probs, trained leave-one-family-out
    # at the meta level (reuses the per-family expert probs already computed — cheap). It learns
    # WHICH expert to trust, so it keeps max's recall while cutting max's over-refusal.
    stack_d, stack_a = {}, {}
    for c in E_d:
        others = [o for o in E_d if o != c]
        Xtr = np.vstack([np.vstack([E_d[o], E_a[o]]) for o in others])
        ytr = np.concatenate([np.r_[np.ones(len(E_d[o])), np.zeros(len(E_a[o]))] for o in others])
        meta = LogisticRegression(max_iter=1000, class_weight="balanced").fit(Xtr, ytr)
        stack_d[c] = meta.predict_proba(E_d[c])[:, 1]
        stack_a[c] = meta.predict_proba(E_a[c])[:, 1]
        rec["stack"][c] = float((stack_d[c] >= .5).mean())
        over["stack"][c] = float((stack_a[c] >= .5).mean())
    sweep["stack_d"] = list(stack_d.values())
    sweep["stack_a"] = list(stack_a.values())

    mr = {f: np.nanmean(list(rec[f].values())) for f in feats}
    mo = {f: np.nanmean(list(over[f].values())) for f in feats}
    print("  " + "-" * 74)
    print(f"  {'MEAN':18} " + " ".join(f"{mr[f]:4.0%}/{mo[f]:3.0%}" for f in feats))

    # ── tau-sweep: the recall/over-refusal Pareto frontier for the max-pool score (calibrated) ──
    d_max = np.concatenate(sweep["max_d"]); a_max = np.concatenate(sweep["max_a"])
    d_all = np.concatenate(sweep["all_d"]); a_all = np.concatenate(sweep["all_a"])
    taus = np.round(np.arange(0.50, 0.96, 0.05), 2)
    pareto = {"max": [], "all": []}
    for t in taus:
        pareto["max"].append((float((d_max >= t).mean()), float((a_max >= t).mean())))
        pareto["all"].append((float((d_all >= t).mean()), float((a_all >= t).mean())))
    # knee for max: highest recall subject to over-refusal <= 0.15
    knee = None
    for t, (r, o) in zip(taus, pareto["max"]):
        if o <= 0.15 and (knee is None or r > knee[1]):
            knee = (t, r, o)
    print("\ntau-sweep (max-pool pooled over all families):")
    print(f"  {'tau':>5} {'recall':>8} {'over-refuse':>12}")
    for t, (r, o) in zip(taus, pareto["max"]):
        mark = "  <- knee (over<=15%)" if knee and t == knee[0] else ""
        print(f"  {t:5.2f} {r:8.1%} {o:12.1%}{mark}")
    if knee:
        print(f"  chosen operating point: tau={knee[0]:.2f}  recall={knee[1]:.1%}  over-refusal={knee[2]:.1%}")

    figp = go.Figure()
    for name, col in (("max", "#9467bd"), ("all", "#2ca02c")):
        xs = [o for (_r, o) in pareto[name]]
        ys = [r for (r, _o) in pareto[name]]
        figp.add_trace(go.Scatter(x=xs, y=ys, mode="lines+markers", name=f"{name} (tau 0.5..0.95)",
                                  line=dict(color=col), text=[f"tau={t}" for t in taus]))
    if knee:
        figp.add_trace(go.Scatter(x=[knee[2]], y=[knee[1]], mode="markers+text",
                                  marker=dict(color="black", size=12, symbol="star"),
                                  text=[f"knee tau={knee[0]}"], textposition="top center", name="chosen"))
    figp.add_vline(x=0.15, line_dash="dash", line_color="gray")
    figp.update_layout(title="Operating-point curve: deny-recall vs over-refusal as tau sweeps "
                       "(calibrated max-pool vs concat)", xaxis_title="over-refusal (withheld benign)",
                       yaxis_title="deny-recall (withheld denies)", template="plotly_white", height=520)
    figp.write_html(os.path.join(PLOTS, "concept_pareto.html"))
    try:
        figp.write_image(os.path.join(PLOTS, "concept_pareto.png"), width=900, height=520, scale=2)
    except Exception as e:
        print(f"  (pareto png skipped: {e})")

    from plotly.subplots import make_subplots as _msub
    plotcats = [c for c in cats if c in rec["ng"]]
    fig = _msub(rows=2, cols=1, subplot_titles=(
        "deny-recall on WITHHELD family (higher=better)",
        "over-refusal on WITHHELD family's benign variants (lower=better)"))
    for f in feats:
        fig.add_trace(go.Bar(name=f, x=plotcats, y=[rec[f][c] for c in plotcats],
                             marker_color=colors[f], legendgroup=f), 1, 1)
        fig.add_trace(go.Bar(name=f, x=plotcats, y=[over[f][c] for c in plotcats],
                             marker_color=colors[f], legendgroup=f, showlegend=False), 2, 1)
    fig.update_yaxes(range=[0, 1.05])
    fig.update_layout(
        title=f"Max-pool as bias-recovery: deny-recall vs over-refusal (leave-one-family-out)  "
              f"— recall/over  ng {mr['ng']:.0%}/{mo['ng']:.0%} · all {mr['all']:.0%}/{mo['all']:.0%} · "
              f"max {mr['max']:.0%}/{mo['max']:.0%}",
        barmode="group", template="plotly_white", height=780, legend_title="features")
    fig.write_html(os.path.join(PLOTS, "concept_generalization.html"))
    try:
        fig.write_image(os.path.join(PLOTS, "concept_generalization.png"), width=1300, height=520, scale=2)
    except Exception as e:
        print(f"  (png skipped: {e})")
    print(f"\nsaved concept_generalization.(html|png) -> {os.path.relpath(PLOTS, HERE)}/")


if __name__ == "__main__":
    main()
