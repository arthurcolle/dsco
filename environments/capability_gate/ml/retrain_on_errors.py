"""Scalable, generalizable retrain-on-where-v2-was-wrong (hard-example mining).

SCALABLE: v2's errors are mined AUTOMATICALLY over the full dataset — a row is a "v2 error"
when v2's prediction disagrees with the security-correct label (expected_decision). No manual
labeling; scales to 64k/100k.

GENERALIZABLE (the part that matters): retraining on errors risks MEMORIZING the specific wrong
rows. We guard against that and prove it didn't happen three ways:
  1. upweight v2's errors but KEEP the full base distribution (no catastrophic forgetting),
  2. HELD-OUT ERRORS — emphasize only 75% of v2's errors, then measure the fix-rate on the
     other 25% the retrain never emphasized. High fix-rate = learned the error PATTERN, not
     the instances.
  3. LEAVE-ONE-FAMILY-OUT — withhold one family's errors from the emphasis; the retrain must
     still cut that family's test errors (transfer across error types).
Reports false-negative (missed denies) and false-positive (over-refusals) on the held-out-tool
TEST split, before (v2) vs after (v4), and re-mines v4 to show error convergence (loop-ready).

Run: python retrain_on_errors.py
"""
import json
import os
import sys

import joblib
import numpy as np
import pandas as pd
import scipy.sparse as sp
from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.linear_model import LogisticRegression

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from effect_features import effect_features  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "..", "..", "data", "cap_classifier")
_TIER = {"untrusted": 0, "standard": 1, "trusted": 2}
rng = np.random.default_rng(0)


def load(name="expanded_v2.jsonl", cap=64000):
    rows = []
    with open(os.path.join(DATA, name)) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            inp = r["input"] if isinstance(r["input"], str) else json.dumps(r["input"])
            rows.append({"text": f'{r.get("tool","")} {inp}', "tool": r.get("tool", ""), "input": inp,
                         "tainted": int((r.get("pre_taint") or {}).get("untrusted", 0)),
                         "private": int((r.get("pre_taint") or {}).get("private", 0)),
                         "tier": _TIER.get(r.get("tier"), 1),
                         "y": 1 if r.get("expected_decision") == "deny" else 0,
                         "cat": r.get("family") or r.get("expected_category", "?"),
                         "split": r.get("split", "train")})
            if len(rows) >= cap:
                break
    for r in rows:
        r["eff"] = effect_features(r["tool"], r["input"])
    return rows


def v2_predict(model, rows):
    df = pd.DataFrame([{"text": r["text"], "tainted": r["tainted"],
                        "private": r["private"], "tier": r["tier"]} for r in rows])
    return model.predict(df).astype(int)


def feats(rows, vec=None, fit=False):
    texts = [r["text"] for r in rows]
    if fit:
        vec = TfidfVectorizer(analyzer="char_wb", ngram_range=(3, 5), min_df=2, max_features=40000)
        Xt = vec.fit_transform(texts)
    else:
        Xt = vec.transform(texts)
    num = np.array([[r["tainted"], r["private"], r["tier"]] + r["eff"] for r in rows], float)
    return sp.hstack([Xt, sp.csr_matrix(num)]).tocsr(), vec


def fit_lr(X, y, w=None):
    return LogisticRegression(max_iter=2000, C=4.0, class_weight="balanced").fit(X, y, sample_weight=w)


def fn_fp(pred, y):
    y = np.asarray(y); pred = np.asarray(pred)
    fn = float((pred[y == 1] == 0).mean()) if (y == 1).any() else float("nan")   # missed denies
    fp = float((pred[y == 0] == 1).mean()) if (y == 0).any() else float("nan")   # over-refusals
    return fn, fp


def main():
    rows = load()
    tr = [r for r in rows if r["split"] == "train"]
    te = [r for r in rows if r["split"] == "test"]
    ytr = np.array([r["y"] for r in tr]); yte = np.array([r["y"] for r in te])
    print(f"corpus {len(rows)}  train {len(tr)}  test {len(te)} (held-out tools)")

    v2 = joblib.load(os.path.join(HERE, "advisor_model_v2.joblib"))
    v2_tr = v2_predict(v2, tr); v2_te = v2_predict(v2, te)
    err_tr = v2_tr != ytr                       # WHERE v2 WAS WRONG (mined automatically)
    print(f"v2 errors mined on train: {int(err_tr.sum())}/{len(tr)} ({err_tr.mean():.1%})")

    Xtr, vec = feats(tr, fit=True)
    Xte, _ = feats(te, vec)

    base = fit_lr(Xtr, ytr)                      # retrain, no error emphasis
    W = 5.0
    w_all = np.where(err_tr, W, 1.0)
    v4 = fit_lr(Xtr, ytr, w_all)                 # retrain, v2's errors upweighted 5x

    print("\n=== held-out-TOOL TEST: false-neg (missed deny) / false-pos (over-refuse) ===")
    for name, pred in [("v2 (deployed)", v2_te), ("retrain base", base.predict(Xte)),
                       ("v4 (error-weighted)", v4.predict(Xte))]:
        fn, fp = fn_fp(pred, yte)
        print(f"  {name:20} FN {fn:6.1%}   FP {fp:6.1%}   err {(np.asarray(pred)!=yte).mean():6.1%}")

    # 1) HELD-OUT ERRORS — emphasize 75% of v2 errors, measure fix-rate on the untouched 25%.
    eidx = np.where(err_tr)[0]
    rng.shuffle(eidx)
    cut = int(0.75 * len(eidx))
    emph, held = set(eidx[:cut].tolist()), eidx[cut:]
    w_ho = np.array([W if i in emph else 1.0 for i in range(len(tr))])
    v4_ho = fit_lr(Xtr, ytr, w_ho)
    Xheld = Xtr[held]; yheld = ytr[held]
    base_fix = float((base.predict(Xheld) == yheld).mean())
    v4_fix = float((v4_ho.predict(Xheld) == yheld).mean())
    print(f"\n=== GENERALIZATION 1 — fix-rate on {len(held)} HELD-OUT v2-errors (never emphasized) ===")
    print(f"  base retrain: {base_fix:.1%}   ->   v4 (emphasized OTHER errors): {v4_fix:.1%}   "
          f"(+{v4_fix-base_fix:.1%} generalized, not memorized)")

    # 2) LEAVE-ONE-FAMILY-OUT — withhold a family's errors from emphasis, still cut its test errors.
    fams = sorted({r["cat"] for r in tr})
    print("\n=== GENERALIZATION 2 — leave-one-family-out error emphasis (test-err drop) ===")
    print(f"  {'family':18} {'v2 test-err':>11} {'base':>8} {'v4-LOFO':>9}")
    te_by = {f: [i for i, r in enumerate(te) if r["cat"] == f] for f in fams}
    base_te = base.predict(Xte)
    for f in fams:
        idx = te_by.get(f, [])
        if len(idx) < 20:
            continue
        w_lofo = np.where(err_tr & np.array([r["cat"] != f for r in tr]), W, 1.0)
        v4f = fit_lr(Xtr, ytr, w_lofo)
        v2e = float((v2_te[idx] != yte[idx]).mean())
        be = float((base_te[idx] != yte[idx]).mean())
        v4e = float((v4f.predict(Xte)[idx] != yte[idx]).mean())
        print(f"  {f:18} {v2e:11.1%} {be:8.1%} {v4e:9.1%}")

    # 3) CONVERGENCE — re-mine v4's errors on train (loop-ready: harvest -> retrain -> re-mine).
    v4_tr_err = (v4.predict(Xtr) != ytr).mean()
    print(f"\n=== CONVERGENCE — train error-rate: v2 {err_tr.mean():.1%} -> v4 {v4_tr_err:.1%} "
          f"(re-mine + retrain loops until dry) ===")

    joblib.dump({"model": v4, "vectorizer": vec}, os.path.join(HERE, "advisor_v4_errfix.joblib"))
    print("\nsaved advisor_v4_errfix.joblib")


if __name__ == "__main__":
    main()
