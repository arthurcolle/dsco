"""Train a neural capability-gate advisor on the expanded 10k dataset and plot the training
process — modern stack: polars (data), PyTorch (a real char-n-gram EmbeddingBag network with
a genuine gradient-descent loop), Plotly (interactive HTML + PNG).

Figures written to ml/plots/ (both .html interactive and .png):
  1. training_curve      — per-epoch train/val BCE loss (down) + accuracy & AUC (up): the
                           literal neural training process.
  2. learning_curve      — val accuracy / deny-recall vs training-set SIZE (data efficiency).
  3. gate_blindspots     — rule-gate vs learned-model accuracy per category: exposes the NEW
                           risk axes (destructive/cloud/sandbox/payment/...) the rules can't see.
  4. family_generalization — leave-one-category-out deny-recall (anti-memorization; the honest
                             limit: brand-new risk vocabularies don't generalize unseen).

Saves advisor_torch.pt. Run: python train_curves.py
"""
import gzip
import json
import os
import sys
import zlib

import numpy as np
import plotly.graph_objects as go
import polars as pl
import torch
import torch.nn as nn
from plotly.subplots import make_subplots

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from dsco_gate import Session, gate  # noqa: E402
from effect_features import NUM_EFFECT, effect_features  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "..", "..", "data", "cap_classifier")
PLOTS = os.path.join(HERE, "plots")
_TIER = {"untrusted": 0, "standard": 1, "trusted": 2}
V = 1 << 16                       # hashed char-n-gram vocabulary
torch.manual_seed(7)
rng = np.random.default_rng(7)


# ── feature extraction: deterministic hashed char n-grams (3..5) → EmbeddingBag ids ──
def ngram_ids(text: str):
    s = text.lower()
    out = []
    for n in (3, 4, 5):
        for i in range(len(s) - n + 1):
            out.append(zlib.crc32(s[i:i + n].encode("utf-8", "ignore")) % V)
    return out or [0]


def _as_str(x):
    return x if isinstance(x, str) else json.dumps(x)


def load_rows():
    """Load the high-variation v2 dataset. The 'split' field is the honest held-out-TOOL
    partition (disjoint tool binaries across train/val/test), so val/test measure real
    cross-tool-name generalization — trivial memorization can't win."""
    rows = []
    with open(os.path.join(DATA, "expanded_v2.jsonl")) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            inp = _as_str(r.get("input", ""))
            pt = r.get("pre_taint", {}) or {}
            rows.append({"tool": r.get("tool", ""), "input": inp,
                         "text": f'{r.get("tool","")} {inp}', "tier_s": r.get("tier", "trusted"),
                         "tier": _TIER.get(r.get("tier"), 1), "tainted": int(pt.get("untrusted", 0)),
                         "private": int(pt.get("private", 0)),
                         "y": 1 if r.get("expected_decision") == "deny" else 0,
                         "cat": r.get("expected_category", "?"), "axis": r.get("risk_axis", "?"),
                         "split": r.get("split", "train"), "cap_tool": r.get("cap_tool", "")})
    for r in rows:                       # precompute ids + effect features once
        r["ids"] = ngram_ids(r["text"])
        r["eff"] = effect_features(r["tool"], r["input"])
    return rows


def make_bag(subset, device):
    flat, offsets, o = [], [], 0
    for r in subset:
        offsets.append(o)
        flat.extend(r["ids"])
        o += len(r["ids"])
    flat_t = torch.tensor(flat, dtype=torch.long, device=device)
    off_t = torch.tensor(offsets, dtype=torch.long, device=device)
    num = torch.tensor([[r["tainted"], r["private"], r["tier"] / 2.0] + r["eff"] for r in subset],
                       dtype=torch.float32, device=device)
    y = torch.tensor([r["y"] for r in subset], dtype=torch.float32, device=device)
    return flat_t, off_t, num, y


class DscoAdvisor(nn.Module):
    """char-n-gram EmbeddingBag + numeric taint context → MLP → deny logit."""
    def __init__(self, vocab=V, emb=64, hidden=128):
        super().__init__()
        self.bag = nn.EmbeddingBag(vocab, emb, mode="mean")
        self.net = nn.Sequential(nn.Linear(emb + 3 + NUM_EFFECT, hidden), nn.ReLU(), nn.Dropout(0.2),
                                 nn.Linear(hidden, 64), nn.ReLU(), nn.Linear(64, 1))

    def forward(self, flat, offsets, num):
        e = self.bag(flat, offsets)
        return self.net(torch.cat([e, num], dim=1)).squeeze(1)


def _auc(y, p):
    y = np.asarray(y); p = np.asarray(p)
    pos, neg = p[y == 1], p[y == 0]
    if len(pos) == 0 or len(neg) == 0:
        return float("nan")
    # Mann–Whitney U (rank-based AUC), no sklearn dependency
    order = np.argsort(p)
    ranks = np.empty(len(p)); ranks[order] = np.arange(1, len(p) + 1)
    return float((ranks[y == 1].sum() - len(pos) * (len(pos) + 1) / 2) / (len(pos) * len(neg)))


def train_model(tr, va, device="cpu", epochs=40, bs=512, log=False):
    model = DscoAdvisor().to(device)
    opt = torch.optim.Adam(model.parameters(), lr=2e-3, weight_decay=1e-5)
    npos = sum(r["y"] for r in tr); nneg = len(tr) - npos
    pos_w = torch.tensor([nneg / max(1, npos)], device=device)
    lossf = nn.BCEWithLogitsLoss(pos_weight=pos_w)
    vflat, voff, vnum, vy = make_bag(va, device)
    hist = {"tr_loss": [], "va_loss": [], "tr_acc": [], "va_acc": [], "va_auc": []}
    idx = np.arange(len(tr))
    for ep in range(epochs):
        model.train()
        rng.shuffle(idx)
        for s in range(0, len(idx), bs):
            batch = [tr[i] for i in idx[s:s + bs]]
            flat, off, num, y = make_bag(batch, device)
            opt.zero_grad()
            loss = lossf(model(flat, off, num), y)
            loss.backward()
            opt.step()
        model.eval()
        with torch.no_grad():
            tflat, toff, tnum, ty = make_bag(tr, device)
            tp = torch.sigmoid(model(tflat, toff, tnum)).cpu().numpy()
            vp = torch.sigmoid(model(vflat, voff, vnum)).cpu().numpy()
            tl = lossf(model(tflat, toff, tnum), ty).item()
            vl = lossf(model(vflat, voff, vnum), vy).item()
        tyn, vyn = ty.cpu().numpy(), vy.cpu().numpy()
        hist["tr_loss"].append(tl); hist["va_loss"].append(vl)
        hist["tr_acc"].append(float(((tp >= .5) == tyn).mean()))
        hist["va_acc"].append(float(((vp >= .5) == vyn).mean()))
        hist["va_auc"].append(_auc(vyn, vp))
        if log and (ep % 5 == 0 or ep == epochs - 1):
            print(f"    epoch {ep:2d}  train_loss {tl:.3f}  val_loss {vl:.3f}  "
                  f"val_acc {hist['va_acc'][-1]:.3f}  val_auc {hist['va_auc'][-1]:.3f}")
    return model, hist


def predict(model, subset, device="cpu"):
    model.eval()
    with torch.no_grad():
        flat, off, num, _ = make_bag(subset, device)
        return torch.sigmoid(model(flat, off, num)).cpu().numpy()


def rule_decision(r):
    s = Session()
    if r["tainted"]:
        s.tainted_untrusted = True
    if r["private"]:
        s.accessed_private = True
    return gate(r["tool"], r["input"], r["tier_s"], s)["decision"]


def save(fig, name):
    fig.write_html(os.path.join(PLOTS, name + ".html"))
    try:
        fig.write_image(os.path.join(PLOTS, name + ".png"), width=1200, height=520, scale=2)
    except Exception as e:
        print(f"    (png export skipped: {e})")


def fig_training(hist):
    xs = list(range(1, len(hist["tr_loss"]) + 1))
    fig = make_subplots(rows=1, cols=2, subplot_titles=("BCE loss", "accuracy & AUC (val)"))
    fig.add_trace(go.Scatter(x=xs, y=hist["tr_loss"], name="train loss", line=dict(color="#1f77b4")), 1, 1)
    fig.add_trace(go.Scatter(x=xs, y=hist["va_loss"], name="val loss", line=dict(color="#d62728")), 1, 1)
    fig.add_trace(go.Scatter(x=xs, y=hist["va_acc"], name="val acc", line=dict(color="#2ca02c")), 1, 2)
    fig.add_trace(go.Scatter(x=xs, y=hist["va_auc"], name="val auc", line=dict(color="#9467bd")), 1, 2)
    fig.update_xaxes(title="epoch"); fig.update_layout(
        title="Training process — neural capability-gate advisor (char-ngram EmbeddingBag)",
        template="plotly_white", height=520)
    save(fig, "training_curve")


def fig_learning(allrows, va, device):
    n = len(allrows)
    order = np.arange(n); rng.shuffle(order)
    sizes = [0.1, 0.2, 0.35, 0.5, 0.7, 1.0]
    accs, recs, xs = [], [], []
    for frac in sizes:
        k = max(200, int(frac * n))
        sub = [allrows[i] for i in order[:k]]
        m, _ = train_model(sub, va, device, epochs=15)
        p = predict(m, va, device)
        y = np.array([r["y"] for r in va])
        accs.append(float(((p >= .5) == y).mean()))
        d = y == 1
        recs.append(float(((p[d] >= .5)).mean()) if d.any() else np.nan)
        xs.append(k)
    fig = go.Figure()
    fig.add_trace(go.Scatter(x=xs, y=accs, name="val accuracy", mode="lines+markers", line=dict(color="#2ca02c")))
    fig.add_trace(go.Scatter(x=xs, y=recs, name="val deny-recall", mode="lines+markers", line=dict(color="#9467bd")))
    fig.update_layout(title="Learning curve — performance vs training-set size",
                      xaxis_title="training examples", yaxis_title="score",
                      yaxis_range=[0.5, 1.02], template="plotly_white", height=520)
    save(fig, "learning_curve")


def fig_blindspots(expanded, model, device):
    cats = sorted({r["cat"] for r in expanded})
    p = predict(model, expanded, device)
    pred = (p >= .5).astype(int)
    rule_acc, model_acc, ns = {}, {}, {}
    for c in cats:
        idx = [i for i, r in enumerate(expanded) if r["cat"] == c]
        ns[c] = len(idx)
        rule_acc[c] = np.mean([rule_decision(expanded[i]) == ("deny" if expanded[i]["y"] else "allow") for i in idx])
        model_acc[c] = float((pred[idx] == np.array([expanded[i]["y"] for i in idx])).mean())
    order = sorted(cats, key=lambda c: rule_acc[c])
    labels = [f"{c} (n={ns[c]})" for c in order]
    fig = go.Figure()
    fig.add_trace(go.Bar(y=labels, x=[rule_acc[c] for c in order], name="rule gate",
                         orientation="h", marker_color="#d62728"))
    fig.add_trace(go.Bar(y=labels, x=[model_acc[c] for c in order], name="learned model",
                         orientation="h", marker_color="#1f77b4"))
    fig.update_layout(title="Gate blind spots — rule gate vs learned model per category",
                      barmode="group", xaxis_title="accuracy vs security-correct label",
                      xaxis_range=[0, 1.02], template="plotly_white", height=700)
    save(fig, "gate_blindspots")
    return rule_acc, model_acc, ns


def fig_generalization(expanded, focus, device):
    fams = [c for c in sorted({r["cat"] for r in expanded}) if c in focus]
    recs, ns = [], []
    for fam in fams:
        tr = [r for r in expanded if r["cat"] != fam]
        te = [r for r in expanded if r["cat"] == fam and r["y"] == 1]
        ns.append(len(te))
        if len(te) < 5:
            recs.append(np.nan); continue
        m, _ = train_model(tr, te, device, epochs=15)
        p = predict(m, te, device)
        recs.append(float((p >= .5).mean()))
    fig = go.Figure(go.Bar(x=fams, y=recs, marker_color="#ff7f0e",
                           text=[f"{r:.0%}<br>n={n}" if not np.isnan(r) else "" for r, n in zip(recs, ns)],
                           textposition="outside"))
    fig.update_layout(title="Leave-one-category-out generalization (family withheld from training)",
                      xaxis_title="held-out category", yaxis_title="deny-recall on unseen family",
                      yaxis_range=[0, 1.08], template="plotly_white", height=520)
    save(fig, "family_generalization")
    return dict(zip(fams, recs))


def main():
    os.makedirs(PLOTS, exist_ok=True)
    device = ("mps" if torch.backends.mps.is_available()          # Apple Metal GPU
              else "cuda" if torch.cuda.is_available() else "cpu")
    rows = load_rows()
    expanded = rows
    # HONEST split: disjoint tool binaries across train/val/test (held-out-tool generalization)
    tr = [r for r in rows if r["split"] == "train"]
    va = [r for r in rows if r["split"] == "val"]
    te = [r for r in rows if r["split"] == "test"]

    # polars summary of the corpus (modern dataframe)
    df = pl.DataFrame([{"split": r["split"], "axis": r["axis"],
                        "decision": "deny" if r["y"] else "allow"} for r in rows])
    print(f"corpus: {len(rows)} rows  deny-rate {df['decision'].eq('deny').mean():.1%}  device={device}")
    print(f"held-out-tool split — train {len(tr)}  val {len(va)}  test {len(te)}  "
          f"(train tools {len({r['cap_tool'] for r in tr})}, "
          f"test tools {len({r['cap_tool'] for r in te})}, "
          f"overlap {len({r['cap_tool'] for r in tr} & {r['cap_tool'] for r in te})})")

    print("\n[1/4] training-process curve (torch, 40 epochs; val = UNSEEN tool binaries)...")
    model, hist = train_model(tr, va, device, epochs=40, log=True)
    fig_training(hist)
    tp = predict(model, te, device)
    ty = np.array([r["y"] for r in te])
    dr = float((tp[ty == 1] >= .5).mean())
    print(f"      FINAL held-out-TEST (unseen tools): acc {((tp>=.5)==ty).mean():.3f}  "
          f"deny-recall {dr:.3f}  auc {_auc(ty, tp):.3f}")

    print("[2/4] learning curve (vs training size; eval on held-out-tool test)...")
    fig_learning(tr, te, device)

    print("[3/4] gate blind-spot comparison (rule vs learned)...")
    rule_acc, model_acc, ns = fig_blindspots(expanded, model, device)
    new_axes = ["destructive", "cloud-escalation", "sandbox-escape", "payment-abuse",
                "supply-chain-exec", "pivot-exfil", "exfil-external", "confused-deputy"]
    print("      rule-gate accuracy on NEW sub-areas (its blind spots) -> learned model:")
    for c in new_axes:
        if c in rule_acc:
            print(f"        {c:20} rule {rule_acc[c]:5.0%}  ->  model {model_acc[c]:5.0%}  (n={ns[c]})")

    print("[4/4] leave-one-category-out generalization...")
    gen = fig_generalization(expanded, set(new_axes), device)
    print("      held-out deny-recall:", {k: round(v, 2) for k, v in gen.items() if not np.isnan(v)})

    torch.save({"state_dict": model.state_dict(), "vocab": V}, os.path.join(HERE, "advisor_torch.pt"))
    print(f"\nsaved advisor_torch.pt + 4 interactive plots (.html + .png) -> {os.path.relpath(PLOTS, HERE)}/")


if __name__ == "__main__":
    main()
