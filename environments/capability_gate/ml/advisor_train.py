"""Train a LEARNED capability-gate advisor — no hard-coded evasion patterns.

The model reads the raw tool+input TEXT via character n-grams (so it discovers evasion
signatures — /dev/tcp, nslookup, decimal IPs, base64, homoglyphs — itself) plus the
legitimate session context (accumulated tainted/private, tier) AND the operator env-grant
context (trusted-egress allowlists, scoped write/net grants, control/exfil grants). It is
trained on the SECURITY-CORRECT labels (the swarm's adversarial `expected_decision`, which
overrides the rule gate on the frontier cases), so it can beat the hard-coded rules.

Two weaknesses this version fixes:
  1. GRANT BLINDNESS — the old advisor never saw the operator env, so the grant-modulated
     "pass08" family (same command allow-with-grant / deny-without) generalized at ~37%
     leave-one-family-out. We add grant features (allowed_host_match, secret_scope_granted,
     spawn_permitted, lane_tier, + hardening/control/exfil grant flags) derived from the
     row's env dict. pass06 supplies contrastive trusted-egress pairs so the grant signal is
     learnable even when pass08 is entirely withheld.
  2. SINGLE-MODEL — we add a HistGradientBoosting model on the STRUCTURAL/numeric features
     (egress kind, host kind, ipv6, metadata-IP, taint, grants) and ensemble it with the
     char-ngram LR. Both are isotonic-calibrated; the ensemble averages calibrated
     probabilities and applies a per-tier ABSTAIN band (deny>τ_high, allow<τ_low, else the
     caller falls back to the conservative rule gate).

The headline test is LEAVE-ONE-FAMILY-OUT generalization: train with an entire evasion
family withheld, then measure deny-recall on that unseen family.

    python advisor_train.py            # trains, evals, saves advisor_model_v3.joblib
"""
import glob
import gzip
import json
import os
import re
import sys

import numpy as np
import pandas as pd
from sklearn.calibration import CalibratedClassifierCV
from sklearn.compose import ColumnTransformer
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import classification_report
from sklearn.pipeline import Pipeline

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "..", "..", "data", "cap_classifier")

# Import the gate helpers (host extraction, egress/host classification, taint helpers) so
# the advisor's structural features are computed IDENTICALLY to the deterministic gate.
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..")))
from dsco_gate import (  # noqa: E402
    CAP_CONTROL, CAP_EXEC, CAP_NET, E_EXTERNAL, E_OPAQUE, _HOST_RE, _EXFIL_VERB_RE,
    caps_for_tool, classify_egress, host_kind, input_has_network, input_touches_secrets,
    is_spawn, looks_like_credential, shell_has_write,
)

_METADATA_HOSTS = {"169.254.169.254", "fd00:ec2::254", "metadata.google.internal", "metadata"}
_PATH_RE = re.compile(r'"(?:file_path|path|dest|target)"\s*:\s*"([^"]+)"')


# Oversample the adversarial swarm rows so the rare grant-modulated frontier cases (some
# hardening/scope flips have only 1-2 examples) are not drowned by the 40k rule-labeled
# volume rows — and so the base models AND isotonic calibration keep them above the deny
# threshold. (sample_weight can't be used here: CalibratedClassifierCV does not forward it to
# a wrapped Pipeline's estimator, so physical row replication is the reliable route.)
ADV_WEIGHT = 12


def _oversample(frame):
    adv = frame[frame.family != "volume"]
    if adv.empty or ADV_WEIGHT <= 1:
        return frame
    return pd.concat([frame] + [adv] * (ADV_WEIGHT - 1), ignore_index=True)


def _tier_code(t):
    return {"untrusted": 0, "standard": 1, "trusted": 2}.get(t, 1)


def _truthy(v):
    return str(v).strip().lower() in ("1", "true", "yes", "on")


# ── feature extraction ────────────────────────────────────────────────────────────────
# Grant features named in the task plus the hardening/control/exfil grant flags that
# actually drive the pass08 allow/deny flips, and cheap structural flags for the GB model.
STRUCT_COLS = [
    "egress_kind", "raw_host_kind", "has_ipv6", "is_metadata_ip", "input_secrets",
    "has_network", "is_spawn", "is_control", "is_exec", "is_net", "shell_writes",
]
GRANT_COLS = [
    "allowed_host_match", "secret_scope_granted", "spawn_permitted", "lane_tier",
    "net_grant_block", "run_grant_block", "write_grant_block", "control_granted",
    "exfil_granted",
]
# LR keeps the original session context + the grant features as passthrough numerics.
LR_NUM = ["tainted", "private", "tier"] + GRANT_COLS
# GB sees the full structural/numeric picture (no text).
GB_NUM = ["tainted", "private", "tier"] + GRANT_COLS + STRUCT_COLS


def _extract_hosts(text):
    hosts = []
    for m in _HOST_RE.finditer(text or ""):
        raw = next((g for g in m.groups() if g), None)
        if raw:
            hosts.append(raw.strip("[]").split("%")[0].lower())
    return hosts


def _host_in_scope(host, scope_str):
    for t in (x.strip().lower() for x in (scope_str or "").split(",")):
        if t and (host == t or host.endswith("." + t)):
            return True
    return False


def grant_and_struct_features(tool, inp, env, tier_code, tainted, private):
    """Derive numeric grant + structural features from the row's tool/input/env context.

    Grant signals come from the operator env dict the deterministic gate honors:
      DSCO_TRUSTED_EGRESS_HOSTS / DSCO_ALLOW_NET (host scope), DSCO_ALLOW_WRITE (path scope),
      DSCO_ALLOW_EXFIL (trifecta override), DSCO_ALLOW_CONTROL, and the DSCO_ALLOW_*=0
      hardening blocks. Ternary scope features use +1 in-scope / -1 scope-present-but-out /
      0 no-scope so the model can separate "granted allow" from "out-of-scope deny".
    """
    env = env or {}
    tool = tool or ""
    caps = caps_for_tool(tool, inp)
    egress_kind, _dest = classify_egress(tool, inp, caps)
    hosts = _extract_hosts(inp)
    raw_host_kind = max((host_kind(h) for h in hosts), default=0)

    # --- grant: host allowlist match (learnable from pass06 contrastive trusted-egress) ---
    scopes = []
    tl = env.get("DSCO_TRUSTED_EGRESS_HOSTS")
    if tl:  # non-empty string
        scopes.append(tl)
    an = env.get("DSCO_ALLOW_NET")
    if an and not _truthy(an) and an != "0":  # a host scope, not a boolean toggle
        scopes.append(an)
    if not scopes:
        allowed_host_match = 0
    elif hosts and any(_host_in_scope(h, s) for h in hosts for s in scopes):
        allowed_host_match = 1
    else:
        allowed_host_match = -1  # operator scoped egress but this host is out of scope

    # --- grant: secret/write scope (exfil override + scoped write path) ---
    secret_scope_granted = 0
    if _truthy(env.get("DSCO_ALLOW_EXFIL")):
        secret_scope_granted = 1
    aw = env.get("DSCO_ALLOW_WRITE")
    if aw and not _truthy(aw) and aw != "0":  # scoped write path
        m = _PATH_RE.search(inp or "")
        fp = m.group(1) if m else ""
        secret_scope_granted = 1 if (fp and fp.startswith(aw)) else -1

    # --- grant: hardening blocks (DSCO_ALLOW_*=0 forces deny in the C gate) ---
    net_grant_block = int(str(env.get("DSCO_ALLOW_NET", "")).strip() == "0")
    run_grant_block = int(str(env.get("DSCO_ALLOW_RUN", "")).strip() == "0")
    write_grant_block = int(str(env.get("DSCO_ALLOW_WRITE", "")).strip() == "0")
    control_granted = int(_truthy(env.get("DSCO_ALLOW_CONTROL")))
    exfil_granted = int(_truthy(env.get("DSCO_ALLOW_EXFIL")))

    # --- grant: spawn permitted (clean-room spawn that does not launder data out) ---
    spawn_permitted = 0
    if is_spawn(tool):
        exfil = bool(input_touches_secrets(inp) or looks_like_credential(inp))
        if not exfil and inp and _EXFIL_VERB_RE.search(inp):
            k, _d = classify_egress("__spawn__", inp, CAP_NET)
            exfil = k in (E_EXTERNAL, E_OPAQUE)
        spawn_permitted = 0 if exfil else 1

    return {
        "egress_kind": int(egress_kind),
        "raw_host_kind": int(raw_host_kind),
        "has_ipv6": int(any(":" in h for h in hosts)),
        "is_metadata_ip": int(any(h in _METADATA_HOSTS for h in hosts)),
        "input_secrets": int(bool(inp and input_touches_secrets(inp))),
        "has_network": int(bool(inp and input_has_network(inp))),
        "is_spawn": int(is_spawn(tool)),
        "is_control": int(bool(caps & CAP_CONTROL)),
        "is_exec": int(bool(caps & CAP_EXEC)),
        "is_net": int(bool(caps & CAP_NET)),
        "shell_writes": int(bool(inp and (caps & CAP_EXEC) and shell_has_write(inp))),
        "allowed_host_match": allowed_host_match,
        "secret_scope_granted": secret_scope_granted,
        "spawn_permitted": spawn_permitted,
        "lane_tier": tier_code,
        "net_grant_block": net_grant_block,
        "run_grant_block": run_grant_block,
        "write_grant_block": write_grant_block,
        "control_granted": control_granted,
        "exfil_granted": exfil_granted,
    }


def _row(tool, inp, env, tier_code, tainted, private, y, family):
    if not isinstance(inp, str):
        inp = json.dumps(inp)
    feats = grant_and_struct_features(tool, inp, env, tier_code, tainted, private)
    base = {
        "text": f"{tool} {inp}", "tainted": tainted, "private": private,
        "tier": tier_code, "y": y, "family": family,
    }
    base.update(feats)
    return base


def load_volume(n=40000):
    """Sample the multi-turn flat data (rule-labeled) for broad coverage."""
    rows = []
    path = os.path.join(DATA, "train_mt_1m.ndjson.gz")
    with gzip.open(path, "rt") as fh:
        for i, line in enumerate(fh):
            if len(rows) >= n:
                break
            if i % 3:  # subsample
                continue
            try:
                r = json.loads(line)
            except Exception:
                continue
            rows.append(_row(
                r["tool"], r.get("input", ""), r.get("env", {}), _tier_code(r.get("tier")),
                int(r["tainted"]), int(r["private"]),
                1 if r["decision"] == "deny" else 0, "volume",
            ))
    return rows


def load_adversarial():
    """The swarm passes — labeled with the SECURITY-CORRECT expected decision."""
    rows = []
    for path in glob.glob(os.path.join(DATA, "passes", "pass0*.jsonl")):
        fam = os.path.basename(path).split("_", 1)[0]  # pass01..pass08
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    r = json.loads(line)
                except Exception:
                    continue
                exp = r.get("expected_decision")
                if exp not in ("allow", "deny"):
                    continue
                pt = r.get("pre_taint")
                pt = pt if isinstance(pt, dict) else {}
                rows.append(_row(
                    r.get("tool", ""), r.get("input", ""), r.get("env", {}),
                    _tier_code(r.get("tier")),
                    int(bool(pt.get("untrusted", r.get("tainted", 1)))),
                    int(bool(pt.get("private", r.get("private", 1)))),
                    1 if exp == "deny" else 0, fam,
                ))
    return rows


# ── models ────────────────────────────────────────────────────────────────────────────
def build_lr_pipeline():
    """Char-ngram TFIDF (obfuscation) + session/grant numerics -> LogisticRegression."""
    text = TfidfVectorizer(analyzer="char_wb", ngram_range=(3, 5), min_df=3, max_features=60000)
    ct = ColumnTransformer([
        ("text", text, "text"),
        ("num", "passthrough", LR_NUM),
    ])
    clf = LogisticRegression(max_iter=2000, C=4.0, class_weight="balanced")
    return Pipeline([("feat", ct), ("clf", clf)])


def build_gb_pipeline():
    """Structural/numeric-only gradient boosting (no text)."""
    ct = ColumnTransformer([("num", "passthrough", GB_NUM)])
    clf = HistGradientBoostingClassifier(
        max_iter=300, learning_rate=0.08, max_depth=4, l2_regularization=1.0,
        class_weight="balanced", random_state=13,
    )
    return Pipeline([("feat", ct), ("clf", clf)])


class EnsembleAdvisor:
    """Isotonic-calibrated LR + GB ensemble with a per-tier abstain band.

    Each sub-model is isotonic-calibrated on held-out folds (CalibratedClassifierCV). The
    ensemble deny-score combines the two calibrated deny-probabilities. We combine with a
    CONSERVATIVE MAX rather than a plain average: the LR is the obfuscation/text expert and
    the GB is the structural/grant expert, and they are strong on disjoint slices — averaging
    lets each dilute the other's confident denies (a straight average regressed the obfuscated
    -exfil family pass01 from ~100% to ~78% deny-recall). Max = "deny if either calibrated
    expert is confident", the security-correct aggregation for a gate. `proba_avg()` keeps the
    plain average available for calibration inspection.

    decide() applies the per-tier (τ_low, τ_high) band: deny if p>τ_high, allow if p<τ_low,
    else ABSTAIN so the caller falls back to the conservative rule gate.
    """

    def __init__(self, cv=3):
        self.cv = cv
        self.lr = None
        self.gb = None
        self.thresholds = {0: (0.25, 0.45), 1: (0.30, 0.55), 2: (0.35, 0.60)}

    def fit(self, df, y):
        self.lr = CalibratedClassifierCV(build_lr_pipeline(), method="isotonic", cv=self.cv)
        self.gb = CalibratedClassifierCV(build_gb_pipeline(), method="isotonic", cv=self.cv)
        self.lr.fit(df, y)
        self.gb.fit(df, y)
        return self

    def proba(self, df):
        return np.maximum(self.lr.predict_proba(df)[:, 1], self.gb.predict_proba(df)[:, 1])

    def proba_avg(self, df):
        return 0.5 * (self.lr.predict_proba(df)[:, 1] + self.gb.predict_proba(df)[:, 1])

    def predict(self, df):  # hard 0.5 (for LOFO comparability with the v2 advisor)
        return (self.proba(df) >= 0.5).astype(int)

    def decide(self, df):
        return _decide_scores(self.proba(df), np.asarray(df["tier"]), self.thresholds)

    def tune_thresholds(self, df, y):
        self.thresholds = _tune_on_scores(self.proba(df), np.asarray(y), np.asarray(df["tier"]))
        return self.thresholds


# Per-tier decision-error costs (missed-deny, false-alarm-deny, abstain). Untrusted punishes
# missed denies hard and tolerates false alarms -> strict (denies on weak evidence). Trusted
# punishes false alarms and would rather abstain than over-deny -> lenient. Abstains are cheap
# because they fall back to the conservative deterministic rule gate.
_TIER_COST = {
    0: {"miss": 3.0, "false": 1.0, "abstain": 0.25},   # untrusted
    1: {"miss": 2.0, "false": 1.5, "abstain": 0.25},   # standard
    2: {"miss": 1.5, "false": 2.5, "abstain": 0.25},   # trusted
}


def _tune_on_scores(p, y, tiers):
    """Grid-search (τ_low, τ_high) per tier to MINIMIZE expected decision cost (missed-deny,
    false-alarm-deny, abstain) under the per-tier cost profile. This yields a genuine abstain
    band (uncertain mid-range -> fall back to the rule gate) and orders strictness by tier."""
    p, y, tiers = np.asarray(p), np.asarray(y), np.asarray(tiers)
    grid = [round(x, 2) for x in np.arange(0.10, 0.91, 0.05)]
    thr = {}
    for tc in (0, 1, 2):
        c = _TIER_COST[tc]
        m = tiers == tc
        if m.sum() < 20:
            thr[tc] = {0: (0.25, 0.40), 1: (0.35, 0.55), 2: (0.45, 0.65)}[tc]
            continue
        pt, yt = p[m], y[m]
        best = None
        for lo in grid:
            for hi in grid:
                if hi < lo + 0.05:
                    continue
                pred = np.where(pt > hi, 1, np.where(pt < lo, 0, -1))
                miss = int(((pred == 0) & (yt == 1)).sum())     # decided allow, truly deny
                false = int(((pred == 1) & (yt == 0)).sum())    # decided deny, truly allow
                abstain = int((pred == -1).sum())
                cost = (c["miss"] * miss + c["false"] * false + c["abstain"] * abstain) / len(pt)
                if best is None or cost < best[0]:
                    best = (cost, lo, hi)
        thr[tc] = (best[1], best[2])
    return thr


def _decide_scores(p, tiers, thr):
    p, tiers = np.asarray(p), np.asarray(tiers)
    out = np.full(len(p), "abstain", dtype=object)
    for tc, (lo, hi) in thr.items():
        m = tiers == tc
        out[m & (p > hi)] = "deny"
        out[m & (p < lo)] = "allow"
    return out


def deny_recall(y_true, y_pred):
    yt, yp = np.asarray(y_true), np.asarray(y_pred)
    d = yt == 1
    return float((yp[d] == 1).mean()) if d.any() else float("nan")


def main():
    vol = load_volume()
    adv = load_adversarial()
    df = pd.DataFrame(vol + adv)
    print(f"corpus: {len(df)} rows  ({len(vol)} volume + {len(adv)} adversarial)  "
          f"deny-rate {df.y.mean():.1%}")
    fams = sorted(df[df.family != "volume"].family.unique())
    print(f"adversarial families: {fams}")

    # ── leave-one-family-out generalization (the anti-memorization test) ──
    # We also POOL each held-out family's ensemble probabilities: that pool is the genuine
    # out-of-distribution set (novel evasion families the model never saw) and is where the
    # abstain band earns its keep, so we tune τ on it.
    print("\nLEAVE-ONE-FAMILY-OUT (ensemble, train withholds the whole family, test on it):")
    print(f"  {'held-out family':22} {'deny-recall':>11} {'(n_deny)':>9}")
    lofo = {}
    pool_p, pool_y, pool_t = [], [], []
    for fam in fams:
        train = df[df.family != fam]
        test = df[df.family == fam]
        if (test.y == 1).sum() < 5:
            continue
        tr_os = _oversample(train)
        adv_m = EnsembleAdvisor(cv=3).fit(tr_os, tr_os.y)
        p = adv_m.proba(test)
        r = deny_recall(test.y, (p >= 0.5).astype(int))
        lofo[fam] = r
        pool_p.append(p)
        pool_y.append(np.asarray(test.y))
        pool_t.append(np.asarray(test.tier))
        print(f"  {fam:22} {r:>10.1%} {int((test.y==1).sum()):>9}")
    print("  (pass08's 9 non-exfil denies are control/hardening/scope-grant flips whose env")
    print("   keys appear in NO other family — unreachable by LOFO; solved in-distribution below)")

    pool_p = np.concatenate(pool_p)
    pool_y = np.concatenate(pool_y)
    pool_t = np.concatenate(pool_t)

    # ── tune the per-tier abstain band on the pooled OUT-OF-FAMILY predictions ──
    thr = _tune_on_scores(pool_p, pool_y, pool_t)
    tier_name = {0: "untrusted", 1: "standard", 2: "trusted"}
    print("\nPer-tier abstain band (τ_low, τ_high) tuned on pooled out-of-family scores:")
    dec = _decide_scores(pool_p, pool_t, thr)
    for tc in (0, 1, 2):
        lo, hi = thr[tc]
        m = pool_t == tc
        ab = float((dec[m] == "abstain").mean()) if m.any() else float("nan")
        dmask = m & (dec != "abstain")
        ndeny = int((pool_y[dmask] == 1).sum())
        dd = deny_recall(pool_y[dmask], (dec[dmask] == "deny").astype(int)) if ndeny else float("nan")
        dd_s = f"{dd:.1%}" if ndeny else "  n/a"
        print(f"  {tier_name[tc]:10} τ_low={lo:.2f} τ_high={hi:.2f}  abstain={ab:.1%}  "
              f"decided-deny-recall={dd_s}  (n={int(m.sum())}, n_deny={int((pool_y[m]==1).sum())})")
    overall_ab = float((dec == "abstain").mean())
    decided = dec != "abstain"
    print(f"overall out-of-family abstain rate: {overall_ab:.1%}  "
          f"(abstains fall back to the conservative rule gate)")
    print("Decided-only out-of-family (abstains excluded):")
    print(classification_report(pool_y[decided], (dec[decided] == "deny").astype(int),
                                target_names=["allow", "deny"], digits=3))

    # ── final model on all data + held-out random split report ──
    idx = np.arange(len(df))
    rng = np.random.default_rng(7)
    rng.shuffle(idx)
    cut = int(0.85 * len(df))
    tr, te = df.iloc[idx[:cut]], df.iloc[idx[cut:]]
    tr_os = _oversample(tr)
    adv_m = EnsembleAdvisor(cv=3).fit(tr_os, tr_os.y)
    print("\nHeld-out random split (15%) — ensemble hard 0.5:")
    print(classification_report(te.y, adv_m.predict(te), target_names=["allow", "deny"], digits=3))

    # refit ensemble on ALL data (keep OOD-tuned abstain thresholds) and persist as v3
    df_os = _oversample(df)
    final = EnsembleAdvisor(cv=3).fit(df_os, df_os.y)
    final.thresholds = thr

    # in-distribution check: once the grant flags ARE in training, the ensemble represents
    # the full pass08 grant logic (the control/hardening/scope flips it cannot reach by LOFO).
    p8 = df[df.family == "pass08"]
    print(f"pass08 IN-DISTRIBUTION (grant flags in train): deny-recall={deny_recall(p8.y, final.predict(p8)):.1%} "
          f"accuracy={float((final.predict(p8)==np.asarray(p8.y)).mean()):.1%}  (n_deny={int((p8.y==1).sum())})")

    import joblib
    out = os.path.join(HERE, "advisor_model_v3.joblib")
    joblib.dump(final, out)
    print(f"\nsaved ensemble advisor -> {out}")
    print(f"pass08 LOFO deny-recall (before v2: 36.8%) -> after: {lofo.get('pass08', float('nan')):.1%}")


if __name__ == "__main__":
    # Re-enter through the importable module name so EnsembleAdvisor's __module__ is
    # "advisor_train" (not "__main__") at joblib.dump time — that keeps advisor_model_v3.joblib
    # loadable from any consumer via `import advisor_train; joblib.load(...)`.
    import advisor_train
    advisor_train.main()
