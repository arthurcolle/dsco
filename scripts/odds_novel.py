#!/usr/bin/env python3
"""
Novel odds-making techniques most libraries don't expose directly.

1. No-vig fair-value extraction  — multiplicative, additive, Shin (insider-trader model)
2. Brownian-bridge intraday weather threshold crossing
3. LMSR (logarithmic market scoring rule) automated market maker
4. Gaussian copula for correlated multi-leg parlays
5. Multi-asset Kelly with correlation (joint log-wealth maximization)
6. Split conformal prediction → calibrated weather quantile → binary probability

Each section runs standalone and prints concrete numbers.
"""

from __future__ import annotations

import numpy as np
import pandas as pd
from scipy import stats, optimize

np.random.seed(11)
pd.options.display.float_format = lambda x: f"{x:.4f}"


# ─────────────────────────────────────────────────────────────────────────────
# 1.  No-vig fair-value extraction
# ─────────────────────────────────────────────────────────────────────────────
def novig_multiplicative(p):
    s = sum(p);  return np.array(p) / s

def novig_additive(p):
    p = np.array(p);  return p - (p.sum() - 1) / len(p)

def novig_shin(p):
    """Shin (1993): observed prices reflect (1-z) noise traders + z insiders.
    Returns (true_probs, z̃) where z̃ is the implied insider fraction."""
    p = np.array(p, dtype=float);  s = p.sum()
    def true_from_z(z):
        return (np.sqrt(z * z + 4 * (1 - z) * p * p / s) - z) / (2 * (1 - z))
    z = optimize.brentq(lambda z: true_from_z(z).sum() - 1.0, 1e-6, 0.99)
    return true_from_z(z), z


def demo_novig():
    print("\n══ 1. No-vig fair-value extraction ══")
    # 3-way: home, draw, away — bookmaker offers prices that sum to 1.075 (7.5% vig)
    market = [0.502, 0.298, 0.275]  # implied probs from offered prices
    print(f"  Quoted implied probs: {market}  (sum={sum(market):.3f})")
    mult = novig_multiplicative(market)
    add  = novig_additive(market)
    shin, z = novig_shin(market)
    df = pd.DataFrame({
        "raw":           market,
        "multiplicative": mult,
        "additive":       add,
        f"Shin (z={z:.3f})": shin,
    }, index=["home", "draw", "away"])
    print(df.to_string())


# ─────────────────────────────────────────────────────────────────────────────
# 2.  Brownian bridge — intraday weather threshold crossing
# ─────────────────────────────────────────────────────────────────────────────
def bridge_prob_max_above(a, b, K, var_path):
    """P(max in [t,T] ≥ K | X_t = a, X_T = b) for Brownian bridge with σ²·Δt = var_path.

    Closed form: 1 if K ≤ max(a,b); else exp(-2(K-a)(K-b)/var_path)."""
    if K <= max(a, b):
        return 1.0
    return float(np.exp(-2 * (K - a) * (K - b) / var_path))


def demo_brownian_bridge():
    print("\n══ 2. Brownian bridge — intraday HIGH-TEMP crossing ══")
    # Setup: it's 2:00 PM, current temp = 69°F, NWS forecast high (end of obs window)
    # at 5:00 PM = 71°F. Hourly path std σ = 0.9°F.
    t_now, t_end = 14.0, 17.0      # hours
    T_now, T_fore = 69.0, 71.0
    sigma_h = 0.9                  # °F per √hour
    var_path = (sigma_h ** 2) * (t_end - t_now)

    print(f"  At {t_now:.0f}:00 → T={T_now}°F, forecast {t_end:.0f}:00 high={T_fore}°F, σ_h={sigma_h}")
    print(f"  Bridge variance over remaining window = {var_path:.3f}\n")
    rows = []
    for K in [70, 71, 72, 73, 74, 75]:
        p_cross = bridge_prob_max_above(T_now, T_fore, K, var_path)
        fair_cents = round(p_cross * 100)
        # Tradeable if Arthur's ≤40c rule lets us in
        rows.append({"threshold_°F": K, "P(max≥K)": p_cross, "fair_cents": fair_cents,
                     "tradeable_≤40c": "✓" if fair_cents <= 40 else "—"})
    print(pd.DataFrame(rows).to_string(index=False))


# ─────────────────────────────────────────────────────────────────────────────
# 3.  LMSR — logarithmic market scoring rule (Manifold / Hanson-style AMM)
# ─────────────────────────────────────────────────────────────────────────────
class LMSR:
    """Hanson (2003) LMSR market maker. Cost C(q) = b·log Σ exp(q_i/b)."""
    def __init__(self, n_outcomes, b=100.0):
        self.q = np.zeros(n_outcomes);  self.b = b
    def cost(self, q=None):
        q = self.q if q is None else q
        return self.b * np.log(np.exp(q / self.b).sum())
    def prices(self):
        e = np.exp(self.q / self.b);  return e / e.sum()
    def buy(self, outcome, shares):
        c0 = self.cost()
        q_new = self.q.copy();  q_new[outcome] += shares
        c1 = self.cost(q_new);  self.q = q_new
        return c1 - c0  # $ paid


def demo_lmsr():
    print("\n══ 3. LMSR market maker (Hanson, 2003) ══")
    mm = LMSR(n_outcomes=2, b=80)  # YES/NO market
    rows = [["start (no trades)", 0, 0, *mm.prices(), 0]]
    schedule = [(0, 50, "trader buys 50 YES"),
                (0, 80, "trader buys 80 more YES"),
                (1, 200, "contrarian buys 200 NO"),
                (0, 60, "momentum buy: 60 YES")]
    cum_cost = 0
    for outcome, shares, label in schedule:
        cost = mm.buy(outcome, shares);  cum_cost += cost
        rows.append([label, mm.q[0], mm.q[1], *mm.prices(), cost])
    df = pd.DataFrame(rows, columns=["event", "q_YES", "q_NO", "p_YES", "p_NO", "trade_$"])
    print(df.to_string(index=False))
    print(f"  Bookmaker max loss (LMSR formula) = b·log(n_outcomes) = "
          f"{mm.b * np.log(2):.2f}  ← guaranteed worst case")


# ─────────────────────────────────────────────────────────────────────────────
# 4.  Gaussian copula → correlated 2-leg parlay pricing
# ─────────────────────────────────────────────────────────────────────────────
def gauss_copula_joint(p1, p2, rho):
    """P(both YES) under bivariate Gaussian copula with marginals p1, p2 and corr ρ."""
    z1, z2 = stats.norm.ppf(p1), stats.norm.ppf(p2)
    cov = np.array([[1.0, rho], [rho, 1.0]])
    return float(stats.multivariate_normal([0, 0], cov).cdf([z1, z2]))


def demo_copula():
    print("\n══ 4. Gaussian copula — correlated parlay pricing ══")
    # Two legs:
    #   A = "KNYC high ≥72°F"      true p = 0.62
    #   B = "KLGA gust ≥20 mph"    true p = 0.34
    p1, p2 = 0.62, 0.34
    rows = []
    for rho in [-0.4, -0.2, 0.0, 0.2, 0.4, 0.6, 0.85]:
        joint_yy = gauss_copula_joint(p1, p2, rho)
        naive    = p1 * p2
        fair_parlay = round(joint_yy * 100)
        rows.append({
            "ρ(A,B)": rho,
            "joint P(YES,YES)": joint_yy,
            "independence baseline": naive,
            "fair parlay (cents)": fair_parlay,
            "naive parlay (cents)": round(naive * 100),
            "Δ_cents": fair_parlay - round(naive * 100),
        })
    print(pd.DataFrame(rows).to_string(index=False))
    print(f"  → Even mild correlation (ρ=0.4) makes the parlay {rows[4]['Δ_cents']:+d}c richer than independence.")


# ─────────────────────────────────────────────────────────────────────────────
# 5.  Multi-asset correlated Kelly
# ─────────────────────────────────────────────────────────────────────────────
def correlated_kelly(p1, p2, rho, ask1, ask2):
    """Max E[log(W)] over (f1, f2) for two correlated YES bets.
    Each bet: pay ask_i (cents), receive 100 on YES. Joint via Gaussian copula."""
    j_yy = gauss_copula_joint(p1, p2, rho)
    j_yn = p1 - j_yy;  j_ny = p2 - j_yy;  j_nn = 1 - p1 - p2 + j_yy
    a1, a2 = ask1 / 100, ask2 / 100
    payoff = lambda y, f, a: f * ((1 - a) / a) if y else -f

    def neg_logwealth(f):
        f1, f2 = f
        terms = [
            (j_yy, payoff(True,  f1, a1) + payoff(True,  f2, a2)),
            (j_yn, payoff(True,  f1, a1) + payoff(False, f2, a2)),
            (j_ny, payoff(False, f1, a1) + payoff(True,  f2, a2)),
            (j_nn, payoff(False, f1, a1) + payoff(False, f2, a2)),
        ]
        g = 0.0
        for w, r in terms:
            if 1 + r <= 0:
                return 1e9
            g += w * np.log(1 + r)
        return -g

    res = optimize.minimize(neg_logwealth, x0=[0.05, 0.05],
                            bounds=[(0, 0.5), (0, 0.5)], method="L-BFGS-B")
    return res.x, (j_yy, j_yn, j_ny, j_nn)


def demo_corr_kelly():
    print("\n══ 5. Correlated Kelly — joint stake optimization ══")
    p1, p2, ask1, ask2 = 0.62, 0.34, 38, 22
    print(f"  Leg A: p={p1}, ask={ask1}c | Leg B: p={p2}, ask={ask2}c\n")
    rows = []
    for rho in [-0.4, 0.0, 0.4, 0.7]:
        (f1, f2), _ = correlated_kelly(p1, p2, rho, ask1, ask2)
        # Naive: each Kelly solo
        b1 = (100-ask1)/ask1;  b2 = (100-ask2)/ask2
        k1_solo = max(0, (b1*p1 - (1-p1)) / b1)
        k2_solo = max(0, (b2*p2 - (1-p2)) / b2)
        rows.append({"ρ": rho, "f_A* joint": f1, "f_B* joint": f2,
                     "f_A solo": k1_solo, "f_B solo": k2_solo,
                     "joint_total": f1 + f2, "solo_total": k1_solo + k2_solo})
    print(pd.DataFrame(rows).to_string(index=False))
    print("  → Higher correlation → joint Kelly trims stakes; negative ρ → it can INCREASE them (diversification).")


# ─────────────────────────────────────────────────────────────────────────────
# 6.  Split conformal prediction — distribution-free temperature bands
# ─────────────────────────────────────────────────────────────────────────────
def demo_conformal():
    print("\n══ 6. Split conformal prediction — guaranteed-coverage temp bands ══")
    from sklearn.ensemble import GradientBoostingRegressor

    # Synthetic: y = 70 + 5·x[0] - 3·x[1] + noise(σ=3)
    n = 4000
    X = np.random.randn(n, 4)
    y = 70 + 5 * X[:, 0] - 3 * X[:, 1] + 2 * np.sin(X[:, 2]) + np.random.randn(n) * 3

    Xtr, Xcal, Xte = X[:2400], X[2400:3200], X[3200:]
    ytr, ycal, yte = y[:2400], y[2400:3200], y[3200:]

    reg = GradientBoostingRegressor(n_estimators=200, max_depth=3).fit(Xtr, ytr)
    cal_resid = np.abs(ycal - reg.predict(Xcal))

    alpha = 0.10  # target 90% coverage
    q = np.quantile(cal_resid, np.ceil((len(cal_resid) + 1) * (1 - alpha)) / len(cal_resid))
    yhat = reg.predict(Xte)
    lo, hi = yhat - q, yhat + q
    cov = float(np.mean((yte >= lo) & (yte <= hi)))
    print(f"  Calibration quantile (α={alpha}): ±{q:.2f}°F   target cov = 90.0%   actual cov = {cov*100:.1f}%")

    # Use the conformal band to price a Kalshi-style HIGH ≥72 contract per-day.
    z = (72 - yhat) / (q / stats.norm.ppf(1 - alpha / 2))  # use conformal q as σ proxy
    p_cross = 1 - stats.norm.cdf(z)
    sample = pd.DataFrame({
        "ŷ_high": yhat[:8].round(2),
        "conformal_lo": lo[:8].round(2),
        "conformal_hi": hi[:8].round(2),
        "P(high≥72)":  p_cross[:8].round(3),
        "actual":      yte[:8].round(2),
    })
    print(sample.to_string(index=False))


if __name__ == "__main__":
    demo_novig()
    demo_brownian_bridge()
    demo_lmsr()
    demo_copula()
    demo_corr_kelly()
    demo_conformal()
