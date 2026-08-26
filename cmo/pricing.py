#!/usr/bin/env python3
"""Pricing & relative value on top of the cashflow engine.

Given a tranche's projected cashflows we compute the numbers a trader bids off:
clean price <-> yield, I-spread to an interpolated benchmark curve at the bond's
WAL, and effective duration/convexity (bump-and-reprice). The rv-screen runs a
base scenario across every fully-linked deal and ranks the stack by spread.

Benchmarks: a simple tenor->yield curve (decimal). The default is a rough
Treasury curve; override with --curve "0.25:0.043,2:0.039,5:0.040,10:0.043".

Usage:
    python3 -m cmo.pricing --deal "WORLD OMNI AUTO RECEIVABLES TRUST 2024-C" \
        --cpr 0.13 --cdr 0.01 --sev 0.45 --price 100
    python3 -m cmo.pricing --rv-screen --cpr 0.13 --cdr 0.01 --sev 0.45
"""
import argparse, sys
from . import fetch, cashflow as cf

# Default benchmark curve: tenor (years) -> par yield (decimal).
BENCHMARK = [(0.25, 0.043), (0.5, 0.042), (1.0, 0.040), (2.0, 0.039),
             (3.0, 0.039), (5.0, 0.040), (7.0, 0.042), (10.0, 0.043)]


def interp(curve, t):
    """Linear interpolation (flat beyond the ends) of a tenor->rate curve."""
    if t <= curve[0][0]:
        return curve[0][1]
    if t >= curve[-1][0]:
        return curve[-1][1]
    for (t0, r0), (t1, r1) in zip(curve, curve[1:]):
        if t0 <= t <= t1:
            return r0 + (r1 - r0) * (t - t0) / (t1 - t0)
    return curve[-1][1]


def _scaled(tranche):
    """Cashflows per 100 of face, for discounting."""
    return [(i + p) / tranche.orig * 100.0 for (i, p) in tranche.flows]


def price_at_yield(tranche, annual_yield):
    """Clean price (per 100) discounting the tranche's flows at an annual yield."""
    ym = (1 + annual_yield) ** (1 / 12.0) - 1
    return sum(cf_t / (1 + ym) ** (t + 1) for t, cf_t in enumerate(_scaled(tranche)))


def i_spread(annual_yield, wal, curve=BENCHMARK):
    """Interpolated spread (bps) of the bond's yield over the benchmark at its WAL."""
    return (annual_yield - interp(curve, wal)) * 1e4


def eff_duration_convexity(tranche, annual_yield, dy=0.0025):
    """Effective duration (years) and convexity from a ±dy bump-and-reprice."""
    p0 = price_at_yield(tranche, annual_yield)
    if p0 <= 0:
        return None, None
    p_up = price_at_yield(tranche, annual_yield + dy)
    p_dn = price_at_yield(tranche, annual_yield - dy)
    dur = (p_dn - p_up) / (2 * p0 * dy)
    cvx = (p_up + p_dn - 2 * p0) / (p0 * dy * dy)
    return dur, cvx


def run_deal(conn, deal_key, cpr, cdr, sev, lag=0, at_issuance=False):
    """Amortize + waterfall one deal; return (pool_meta, [surviving tranches])."""
    pool, trs = cf.load_deal(conn, deal_key)
    if not pool or not pool[0] or not trs:
        return None, []
    cur_bal, wac, wam, _orig = pool
    tranches = [cf.Tranche(c, b, cp, g) for c, b, cp, g in trs]
    if at_issuance:
        bal = sum(t.orig for t in tranches)
    else:
        bal = cur_bal
        tranches = cf.season_stack(tranches, sum(t.orig for t in tranches) - cur_bal)
    rows = cf.amortize(bal, wac, wam, cpr, cdr, sev, lag)
    cf.waterfall(tranches, rows)
    return (bal, wac, wam), tranches


def tranche_metrics(t, price, curve=BENCHMARK):
    """Full RV record for one tranche at a clean price."""
    y = cf.yield_to_price(t, price)
    w = cf.wal(t.flows)
    if y is None:
        return {"class": t.name, "wal": w, "yield": None, "ispread": None,
                "duration": None, "convexity": None, "writedown": t.writedown}
    dur, cvx = eff_duration_convexity(t, y)
    return {"class": t.name, "wal": w, "yield": y, "ispread": i_spread(y, w, curve),
            "duration": dur, "convexity": cvx, "writedown": t.writedown}


def linked_deals(conn):
    return [r[0] for r in conn.execute(
        "SELECT deal_key FROM deal_book WHERE n_loans IS NOT NULL "
        "AND n_tranches IS NOT NULL ORDER BY deal_key")]


def parse_curve(s):
    pts = []
    for tok in s.split(","):
        t, r = tok.split(":")
        pts.append((float(t), float(r)))
    return sorted(pts)


def _row(deal, m):
    y = "" if m["yield"] is None else f"{m['yield']*100:7.2f}"
    sp = "" if m["ispread"] is None else f"{m['ispread']:7.0f}"
    du = "" if m["duration"] is None else f"{m['duration']:6.2f}"
    print(f"{deal[:34]:<35}{m['class']:<6}{m['wal']:>6.2f}{y:>8}{sp:>8}{du:>7}"
          f"{m['writedown']/1e6:>9.2f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--deal")
    ap.add_argument("--rv-screen", action="store_true")
    ap.add_argument("--cpr", type=float, default=0.0)
    ap.add_argument("--cdr", type=float, default=0.0)
    ap.add_argument("--sev", type=float, default=0.0)
    ap.add_argument("--lag", type=int, default=0)
    ap.add_argument("--price", type=float, default=100.0)
    ap.add_argument("--at-issuance", action="store_true")
    ap.add_argument("--curve", type=parse_curve, default=BENCHMARK)
    args = ap.parse_args()
    conn = fetch.ensure_db(args.db)

    deals = ([args.deal] if args.deal else
             linked_deals(conn) if args.rv_screen else None)
    if not deals:
        ap.error("need --deal <key> or --rv-screen")

    hdr = (f"{'DEAL':<35}{'CLS':<6}{'WAL':>6}{'YLD%':>8}{'I-SPR':>8}"
           f"{'DUR':>7}{'WRITE$M':>9}")
    print(hdr)
    print("-" * len(hdr))
    for dk in deals:
        meta, tranches = run_deal(conn, dk, args.cpr, args.cdr, args.sev,
                                  args.lag, args.at_issuance)
        if not tranches:
            print(f"{dk[:34]:<35}(no linked pool+stack)", file=sys.stderr)
            continue
        for t in tranches:
            _row(dk, tranche_metrics(t, args.price, args.curve))


if __name__ == "__main__":
    main()
