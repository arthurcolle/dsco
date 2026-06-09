#!/usr/bin/env python3
"""Unit tests for the cashflow engine. Run: python3 -m cmo.test_cashflow

Cases are chosen so the answer is hand-verifiable: a zero-coupon pool amortizes
to its principal; conservation of cash holds; sequential pay retires senior
first; losses write up from the junior class; faster prepay shortens WAL.
"""
from . import cashflow as cf


def approx(a, b, tol=1e-6):
    return abs(a - b) <= tol * max(1.0, abs(b))


def test_amortize_conserves_principal():
    # With no losses, scheduled+prepay principal must repay the whole balance.
    rows = cf.amortize(1_000_000, 0.06, 60, cpr=0.10, cdr=0.0, severity=0.0)
    paid = sum(r["sched_prin"] + r["prepay"] for r in rows)
    assert approx(paid, 1_000_000, 1e-4), paid
    assert rows[-1]["balance_end"] < 1.0


def test_zero_rate_straight_line():
    # Zero WAC, no prepay -> equal scheduled principal each month.
    rows = cf.amortize(1200.0, 0.0, 12, cpr=0.0, cdr=0.0)
    assert len(rows) == 12
    for r in rows:
        assert approx(r["sched_prin"], 100.0, 1e-6), r["sched_prin"]
        assert approx(r["interest"], 0.0)


def test_defaults_produce_loss_and_recovery():
    rows = cf.amortize(1_000_000, 0.06, 60, cpr=0.0, cdr=0.05, severity=0.40)
    tot_loss = sum(r["loss"] for r in rows)
    tot_rec = sum(r["recovery"] for r in rows)
    assert tot_loss > 0 and tot_rec > 0
    # every defaulted dollar is split into loss (severity) + recovery (1-sev)
    for r in rows:
        d = r["loss"] + r["recovery"]
        if d > 0:
            assert approx(r["loss"] / d, 0.40, 1e-6)


def test_sequential_pay_retires_senior_first():
    rows = cf.amortize(1_000_000, 0.05, 36, cpr=0.0, cdr=0.0)
    trs = [cf.Tranche("A", 600_000, 4.0, "A"),
           cf.Tranche("B", 400_000, 5.0, "B")]
    cf.waterfall(trs, rows)
    a, b = trs
    # A must be fully repaid before B receives any principal
    a_done = next(m for m, (_i, p) in enumerate(a.flows)
                  if sum(pp for _ii, pp in a.flows[:m + 1]) >= a.orig - 1)
    b_start = next((m for m, (_i, p) in enumerate(b.flows) if p > 0), len(b.flows))
    assert b_start >= a_done, (a_done, b_start)


def test_losses_hit_junior_first():
    rows = cf.amortize(1_000_000, 0.06, 48, cpr=0.0, cdr=0.08, severity=0.5)
    trs = [cf.Tranche("A", 700_000, 4.0, "A"),
           cf.Tranche("B", 200_000, 5.0, "B"),
           cf.Tranche("C", 100_000, 6.0, "C")]
    cf.waterfall(trs, rows)
    a, b, c = trs
    # the most junior class absorbs writedowns before any senior class
    assert c.writedown >= b.writedown >= a.writedown
    assert c.writedown > 0


def test_faster_prepay_shortens_wal():
    rows_slow = cf.amortize(1_000_000, 0.06, 60, cpr=0.02)
    rows_fast = cf.amortize(1_000_000, 0.06, 60, cpr=0.30)
    t_slow = cf.Tranche("A", 1_000_000, 5.0, "A")
    t_fast = cf.Tranche("A", 1_000_000, 5.0, "A")
    cf.waterfall([t_slow], rows_slow)
    cf.waterfall([t_fast], rows_fast)
    assert cf.wal(t_fast.flows) < cf.wal(t_slow.flows)


def test_yield_at_par_approximates_coupon():
    # A single bond bought at par should yield ~ its coupon (no losses).
    rows = cf.amortize(1_000_000, 0.05, 60, cpr=0.0, cdr=0.0)
    t = cf.Tranche("A", 1_000_000, 5.0, "A")
    cf.waterfall([t], rows)
    y = cf.yield_to_price(t, 100.0)
    assert y is not None and approx(y, 0.05, 0.02), y


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print(f"\n{len(tests)} passed")


if __name__ == "__main__":
    main()
