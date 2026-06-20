#!/usr/bin/env python3
"""Unit tests for pricing/RV. Run: python3 -m cmo.test_pricing"""
from . import cashflow as cf, pricing as pr


def approx(a, b, tol=1e-3):
    return abs(a - b) <= tol * max(1.0, abs(b))


def _par_bond():
    rows = cf.amortize(1_000_000, 0.05, 60, cpr=0.0, cdr=0.0)
    t = cf.Tranche("A", 1_000_000, 5.0, "A")
    cf.waterfall([t], rows)
    return t


def test_interp_endpoints_and_midpoint():
    c = [(1.0, 0.04), (3.0, 0.05)]
    assert approx(pr.interp(c, 0.5), 0.04)      # flat below
    assert approx(pr.interp(c, 5.0), 0.05)      # flat above
    assert approx(pr.interp(c, 2.0), 0.045)     # linear midpoint


def test_price_yield_roundtrip():
    t = _par_bond()
    y = cf.yield_to_price(t, 100.0)
    p = pr.price_at_yield(t, y)
    assert approx(p, 100.0, 1e-3), p


def test_higher_yield_lower_price():
    t = _par_bond()
    assert pr.price_at_yield(t, 0.06) < pr.price_at_yield(t, 0.04)


def test_duration_positive_convexity_nonneg():
    t = _par_bond()
    y = cf.yield_to_price(t, 100.0)
    dur, cvx = pr.eff_duration_convexity(t, y)
    assert dur is not None and dur > 0, dur
    assert cvx is not None and cvx > -1e-6, cvx


def test_ispread_sign():
    # a yield above the benchmark at that WAL is a positive spread
    assert pr.i_spread(0.05, 2.0) > 0
    assert pr.i_spread(0.02, 2.0) < 0


def main():
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print(f"\n{len(tests)} passed")


if __name__ == "__main__":
    main()
