#!/usr/bin/env python3
"""
End-to-end odds-making demo. Uses:
  - openskill   → matchup win-probability from ratings
  - netcal      → probability calibration (temperature scaling) + ECE/MCE
  - penaltyblog → Dixon-Coles bivariate Poisson goals model
  - kelly + fair-value math → translate calibrated prob → Kalshi-style cents
"""

from __future__ import annotations

import numpy as np
import pandas as pd

np.random.seed(7)


# ─────────────────────────────────────────────────────────────────────────────
# 1.  Rating-based win probability with OpenSkill
# ─────────────────────────────────────────────────────────────────────────────
def demo_openskill() -> None:
    from openskill.models import PlackettLuce

    model = PlackettLuce()
    # "True" strengths (hidden)
    true_strength = {"AAPL": 0.9, "MSFT": 0.7, "NVDA": 1.2, "GOOG": 0.6, "META": 0.5}
    ratings = {n: model.rating(name=n) for n in true_strength}

    # Simulate 400 head-to-heads; stronger side wins with logistic prob on the gap
    for _ in range(400):
        a, b = np.random.choice(list(true_strength), 2, replace=False)
        p = 1 / (1 + np.exp(-(true_strength[a] - true_strength[b])))
        if np.random.rand() < p:
            ratings[a], ratings[b] = model.rate([[ratings[a]], [ratings[b]]])
            ratings[a], ratings[b] = ratings[a][0], ratings[b][0]
        else:
            ratings[b], ratings[a] = model.rate([[ratings[b]], [ratings[a]]])
            ratings[b], ratings[a] = ratings[b][0], ratings[a][0]

    print("── OpenSkill ratings after 400 matches ──")
    for n in sorted(ratings, key=lambda x: -ratings[x].mu):
        r = ratings[n]
        print(f"  {n}: μ={r.mu:6.3f}  σ={r.sigma:5.3f}  ordinal={r.ordinal():6.3f}")

    a, b = "NVDA", "META"
    p_win = model.predict_win([[ratings[a]], [ratings[b]]])
    print(f"\n  P({a} beats {b}) = {p_win[0]*100:.1f}%   "
          f"(true: {1/(1+np.exp(-(true_strength[a]-true_strength[b])))*100:.1f}%)")


# ─────────────────────────────────────────────────────────────────────────────
# 2.  Probability calibration with netcal
# ─────────────────────────────────────────────────────────────────────────────
def demo_netcal() -> None:
    from sklearn.datasets import make_classification
    from sklearn.ensemble import GradientBoostingClassifier
    from sklearn.model_selection import train_test_split
    from netcal.scaling import TemperatureScaling
    from netcal.metrics import ECE, MCE

    X, y = make_classification(
        n_samples=8000, n_features=18, n_informative=6,
        n_redundant=4, weights=[0.65, 0.35], random_state=7,
    )
    Xtr, Xte, ytr, yte = train_test_split(X, y, test_size=0.5, random_state=7)
    Xva, Xte, yva, yte = train_test_split(Xte, yte, test_size=0.5, random_state=7)

    clf = GradientBoostingClassifier(n_estimators=120, max_depth=4, random_state=7)
    clf.fit(Xtr, ytr)
    p_raw_val = clf.predict_proba(Xva)[:, 1]
    p_raw_te = clf.predict_proba(Xte)[:, 1]

    ts = TemperatureScaling()
    ts.fit(p_raw_val, yva)
    p_cal_te = ts.transform(p_raw_te)

    ece, mce = ECE(bins=10), MCE(bins=10)
    brier = lambda p, y: np.mean((p - y) ** 2)

    print("\n── netcal: temperature scaling on a GBM ──")
    print(f"  RAW         ECE={ece.measure(p_raw_te, yte):.4f}  "
          f"MCE={mce.measure(p_raw_te, yte):.4f}  Brier={brier(p_raw_te, yte):.4f}")
    print(f"  CALIBRATED  ECE={ece.measure(p_cal_te, yte):.4f}  "
          f"MCE={mce.measure(p_cal_te, yte):.4f}  Brier={brier(p_cal_te, yte):.4f}")
    return p_cal_te


# ─────────────────────────────────────────────────────────────────────────────
# 3.  Dixon-Coles bivariate Poisson with penaltyblog
# ─────────────────────────────────────────────────────────────────────────────
def demo_penaltyblog() -> None:
    import penaltyblog as pb

    teams = ["Arsenal", "Chelsea", "Liverpool", "Spurs", "ManCity", "ManUtd"]
    strength = {t: s for t, s in zip(teams, [1.6, 1.3, 1.7, 1.2, 1.9, 1.4])}
    defense = {t: s for t, s in zip(teams, [0.8, 1.0, 0.7, 1.1, 0.6, 1.0])}
    rows = []
    for _ in range(360):
        h, a = np.random.choice(teams, 2, replace=False)
        gh = np.random.poisson(strength[h] / defense[a] * 1.20)
        ga = np.random.poisson(strength[a] / defense[h])
        rows.append((h, a, gh, ga))
    df = pd.DataFrame(rows, columns=["home", "away", "gh", "ga"])

    model = pb.models.DixonColesGoalModel(
        goals_home=df["gh"].values,
        goals_away=df["ga"].values,
        teams_home=df["home"].values,
        teams_away=df["away"].values,
    )
    model.fit()

    fixture = ("ManCity", "Spurs")
    pred = model.predict(*fixture)
    print(f"\n── penaltyblog Dixon-Coles: {fixture[0]} vs {fixture[1]} ──")
    print(f"  P(home win) = {pred.home_win:.3f}")
    print(f"  P(draw)     = {pred.draw:.3f}")
    print(f"  P(away win) = {pred.away_win:.3f}")
    print(f"  P(over 2.5) = {pred.total_goals('over', 2.5):.3f}")
    print(f"  P(BTTS yes) = {pred.btts_yes:.3f}")
    print(f"  λ home/away = {pred.home_goal_expectation:.2f} / {pred.away_goal_expectation:.2f}")
    return pred


# ─────────────────────────────────────────────────────────────────────────────
# 4.  Fair-value cents + Kelly sizing (Kalshi-flavored)
# ─────────────────────────────────────────────────────────────────────────────
def fair_value_and_kelly(p_true: float, ask_cents: int, bankroll: float = 1000,
                          fraction: float = 0.25) -> dict:
    """Given calibrated win prob, market ask price (cents), return edge + Kelly stake.

    Kalshi-style binary YES: pay `ask_cents`, collect 100 if YES resolves.
    Net odds b = (100 - ask) / ask. Kelly: f* = (b·p − q) / b.
    """
    p = float(p_true)
    q = 1.0 - p
    fair = round(p * 100)
    edge_cents = fair - ask_cents
    b = (100 - ask_cents) / ask_cents
    kelly = (b * p - q) / b
    kelly = max(kelly, 0.0)
    stake = bankroll * fraction * kelly
    contracts = int(stake // (ask_cents / 100))
    return {
        "p_true": p,
        "fair_cents": fair,
        "ask_cents": ask_cents,
        "edge_cents": edge_cents,
        "full_kelly": kelly,
        "frac_kelly_stake_$": round(stake, 2),
        "contracts": contracts,
        "max_loss_$": round(contracts * ask_cents / 100, 2),
        "max_win_$": round(contracts * (100 - ask_cents) / 100, 2),
    }


def demo_kalshi() -> None:
    print("\n── Kalshi-style fair-value + ¼-Kelly sizing ──")
    print(f"  Bankroll $1,000 | rule: skip if ask > 40c or edge < 3c (Arthur's guardrails)\n")
    book = [
        ("KNYC_HIGH>=72", 0.61, 38),
        ("KNYC_HIGH>=72", 0.61, 45),   # blown by 40c cap
        ("WIND_GUST>=20", 0.34, 22),
        ("PRECIP_TRACE",  0.78, 71),
        ("LOWPRICE_FLYR", 0.18, 12),
    ]
    cols = ["market", "p_true", "fair", "ask", "edge", "kelly", "stake$", "ctrs"]
    rows = []
    for mkt, p, ask in book:
        r = fair_value_and_kelly(p, ask)
        skip = "SKIP" if ask > 40 or r["edge_cents"] < 3 else "TRADE"
        rows.append([mkt, f"{p:.2f}", r["fair_cents"], ask, r["edge_cents"],
                     f"{r['full_kelly']*100:.1f}%", r["frac_kelly_stake_$"],
                     r["contracts"]] + [skip])
    print(pd.DataFrame(rows, columns=cols + ["action"]).to_string(index=False))


if __name__ == "__main__":
    demo_openskill()
    demo_netcal()
    demo_penaltyblog()
    demo_kalshi()
