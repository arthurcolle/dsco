#!/usr/bin/env python3
"""Cashflow engine: amortize a pool under prepay/loss scenarios and run the
waterfall to get per-tranche principal, interest, WAL and yield.

This is what turns the static deal book into something an agent can price. Given
a collateral pool (balance, WAC, WAM) and a sequential-pay tranche stack with
subordination, we:

  1. amortize the pool month by month — scheduled principal (level-pay annuity),
     voluntary prepayments (CPR), and defaults (CDR) with a recovery lag/severity;
  2. run the waterfall — interest senior->junior at each class's coupon, principal
     sequentially down the stack, losses written up from the most junior class;
  3. report per-tranche cashflows, WAL, and yield-to-price.

Conventions: rates are annual decimals (wac 0.06 = 6%); CPR/CDR annual decimals;
severity is loss-given-default (recovery = 1 - severity). Sequential pay only
(the dominant auto/ABS structure); pro-rata and triggers are out of scope here.

Usage:
    python3 -m cmo.cashflow --deal "WORLD OMNI AUTO RECEIVABLES TRUST 2024-C" \
        --cpr 0.13 --cdr 0.01 --sev 0.45 --price 100
"""
import argparse, sys
from . import fetch


def smm_from_cpr(cpr):
    return 1.0 - (1.0 - cpr) ** (1.0 / 12.0)


def mdr_from_cdr(cdr):
    return 1.0 - (1.0 - cdr) ** (1.0 / 12.0)


def amortize(balance, wac, wam, cpr=0.0, cdr=0.0, severity=0.0, recovery_lag=0):
    """Return a list of monthly pool cashflows until the balance runs off.

    Each row: {month, interest, sched_prin, prepay, recovery, loss, principal,
    balance_end}. `principal` is the cash passed to the bond waterfall (scheduled
    + prepay + recovery); `loss` is the unrecovered default that writes down bonds.
    """
    r = wac / 12.0
    smm = smm_from_cpr(cpr)
    mdr = mdr_from_cdr(cdr)
    rows, pending = [], []                 # pending: recoveries awaiting their lag
    bal = float(balance)
    for m in range(1, int(round(wam)) + 1):
        if bal <= 1e-6:
            break
        n = int(round(wam)) - m + 1        # remaining term this month
        default = bal * mdr
        perf = bal - default               # performing balance bears P&I
        interest = perf * r
        # scheduled principal: annuity factor over the remaining term
        factor = (r * (1 + r) ** -n) / (1 - (1 + r) ** -n) if r > 0 else 1.0 / n
        sched = min(perf * factor, perf)
        prepay = (perf - sched) * smm
        bal_end = perf - sched - prepay
        loss = default * severity
        rec = default * (1 - severity)
        if recovery_lag > 0:               # defer recovery cash by the lag
            pending.append((m + recovery_lag, rec))
            rec = 0.0
        rec += sum(a for due, a in pending if due == m)
        pending = [(d, a) for d, a in pending if d != m]
        rows.append({
            "month": m, "interest": interest, "sched_prin": sched,
            "prepay": prepay, "recovery": rec, "loss": loss,
            "principal": sched + prepay + rec, "balance_end": bal_end,
        })
        bal = bal_end
    return rows


class Tranche:
    def __init__(self, name, balance, coupon, credit_class):
        self.name = name
        self.orig = float(balance)
        self.bal = float(balance)
        self.coupon = (coupon or 0.0) / 100.0   # stored as percent in the DB
        self.grp = credit_class
        self.flows = []                          # per-month (interest, principal)
        self.writedown = 0.0


def season_stack(tranches, paid_down):
    """Burn an original tranche stack down by principal already paid, sequentially
    (senior first), to recover today's outstanding balances. The collateral tape
    is a seasoned snapshot (current pool < original), but the DB stores original
    tranche faces; this realigns liabilities to the remaining assets. Each
    surviving tranche's `orig` is reset to its current face (the buyer's basis).
    Returns the still-outstanding tranches in seniority order."""
    paid = max(0.0, paid_down)
    for t in tranches:
        cut = min(t.bal, paid)
        t.bal -= cut
        paid -= cut
        t.orig = t.bal
    return [t for t in tranches if t.bal > 1.0]


def waterfall(tranches, pool_rows):
    """Run a sequential-pay waterfall. `tranches` is senior->junior order.
    Mutates/returns the Tranche objects with monthly (interest, principal) flows.
    Losses are written up from the most junior outstanding class."""
    for row in pool_rows:
        # interest: pay each class its coupon, senior first, from collected interest
        avail_i = row["interest"]
        for t in tranches:
            due = t.bal * t.coupon / 12.0
            pay = min(due, avail_i)
            avail_i -= pay
            t_int = pay
            t._pend_int = t_int
        # principal: sequential down the stack
        avail_p = row["principal"] + avail_i   # excess interest turbos principal
        for t in tranches:
            pay = min(t.bal, avail_p)
            t.bal -= pay
            avail_p -= pay
            t.flows.append((t._pend_int, pay))
        # losses: write down from the bottom up
        loss = row["loss"]
        for t in reversed(tranches):
            hit = min(t.bal, loss)
            t.bal -= hit
            t.writedown += hit
            loss -= hit
            if loss <= 1e-9:
                break
    return tranches


def wal(flows):
    """Weighted-average life in years from a list of (interest, principal)."""
    tot = sum(p for _i, p in flows)
    if tot <= 0:
        return 0.0
    return sum((m + 1) * p for m, (_i, p) in enumerate(flows)) / tot / 12.0


def yield_to_price(tranche, price):
    """Bond-equivalent annual yield for a tranche at a clean price (per 100)."""
    if tranche.orig <= 0:
        return None
    scaled = [((i + p) / tranche.orig * 100.0) for (i, p) in tranche.flows]
    def npv(y):
        return -price + sum(cf / (1 + y) ** (t + 1) for t, cf in enumerate(scaled))
    lo, hi = -0.99, 2.0
    if npv(lo) * npv(hi) > 0:
        return None
    for _ in range(200):
        mid = (lo + hi) / 2
        (lo, hi) = (mid, hi) if npv(mid) > 0 else (lo, mid)
    ym = (lo + hi) / 2
    return (1 + ym) ** 12 - 1            # annualize the monthly yield


def load_deal(conn, deal_key):
    """Pull one deal's pool summary and tranche stack from the DB by deal_key."""
    pool = conn.execute("""
        SELECT SUM(p.cur_balance),
               SUM(p.wac*p.cur_balance)/NULLIF(SUM(p.cur_balance),0),
               SUM(p.wam*p.cur_balance)/NULLIF(SUM(p.cur_balance),0),
               SUM(p.orig_balance)
        FROM collateral_pool p JOIN deal_identity i ON i.accession=p.accession
        WHERE i.deal_key=?""", (deal_key,)).fetchone()
    trs = conn.execute("""
        SELECT s.class_name, s.orig_balance, s.coupon, s.credit_class
        FROM tranche_subordination s JOIN deal_identity i ON i.accession=s.accession
        WHERE i.deal_key=? ORDER BY s.class_name""", (deal_key,)).fetchall()
    return pool, trs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--deal", required=True, help="deal_key (see deal_book)")
    ap.add_argument("--cpr", type=float, default=0.0)
    ap.add_argument("--cdr", type=float, default=0.0)
    ap.add_argument("--sev", type=float, default=0.0)
    ap.add_argument("--lag", type=int, default=0, help="recovery lag (months)")
    ap.add_argument("--price", type=float, default=100.0)
    ap.add_argument("--at-issuance", action="store_true",
                    help="price the original pool/stack instead of today's")
    args = ap.parse_args()
    conn = fetch.ensure_db(args.db)
    pool, trs = load_deal(conn, args.deal)
    if not pool or not pool[0]:
        sys.exit(f"no collateral pool for deal_key {args.deal!r}")
    if not trs:
        sys.exit(f"no tranche stack for deal_key {args.deal!r}")
    cur_bal, wac, wam, _orig = pool
    tranches = [Tranche(c, b, cp, g) for c, b, cp, g in trs]
    if args.at_issuance:
        # original stack priced against a pool equal to the bonds (OC ignored)
        bal = sum(t.orig for t in tranches)
    else:
        # seasoned tape: realign the stack to the remaining collateral. Bonds
        # paid down = original bond stack - current pool (bonds track the pool,
        # net of overcollateralization); burn that off the stack sequentially.
        bal = cur_bal
        paid = sum(t.orig for t in tranches) - cur_bal
        tranches = season_stack(tranches, paid)
    rows = amortize(bal, wac, wam, args.cpr, args.cdr, args.sev, args.lag)
    waterfall(tranches, rows)
    print(f"deal: {args.deal}")
    print(f"pool: ${bal/1e6:.1f}M  wac={wac*100:.2f}%  wam={wam:.0f}mo  "
          f"CPR={args.cpr*100:.1f} CDR={args.cdr*100:.1f} sev={args.sev*100:.0f}%")
    print(f"{'CLASS':<7}{'ORIG$M':>9}{'CPN':>7}{'WAL':>7}{'PRIN$M':>9}"
          f"{'WRITE$M':>9}{'YLD%':>8}")
    for t in tranches:
        prin = sum(p for _i, p in t.flows)
        y = yield_to_price(t, args.price)
        print(f"{t.name:<7}{t.orig/1e6:>9.1f}{t.coupon*100:>7.2f}"
              f"{wal(t.flows):>7.2f}{prin/1e6:>9.1f}{t.writedown/1e6:>9.2f}"
              f"{(y*100 if y is not None else float('nan')):>8.2f}")
    tot_loss = sum(r["loss"] for r in rows)
    print(f"pool cumulative loss: ${tot_loss/1e6:.1f}M "
          f"({tot_loss/bal*100:.2f}% of current balance)")


if __name__ == "__main__":
    main()
