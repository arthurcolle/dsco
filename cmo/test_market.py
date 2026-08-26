#!/usr/bin/env python3
"""Unit tests for the market/execution layer. Run: python3 -m cmo.test_market"""
import csv, sqlite3, tempfile, os
from . import market as mk, pricing, cashflow as cf


def approx(a, b, tol=1e-3):
    return abs(a - b) <= tol * max(1.0, abs(b))


def _par_bond():
    rows = cf.amortize(1_000_000, 0.05, 60, cpr=0.0, cdr=0.0)
    t = cf.Tranche("A", 1_000_000, 5.0, "A")
    cf.waterfall([t], rows)
    return t


def test_quote_two_way_brackets_mid():
    q = mk.quote_tranche(_par_bond(), 40.0, 4.0, 6.0, pricing.BENCHMARK)
    assert q is not None
    assert q["bid"] < q["px"] < q["offer"]
    assert approx(q["offer"] - q["bid"], 2 * 4.0 / 32.0)   # full bid/offer width


def test_quote_wider_spread_lower_price():
    tight = mk.quote_tranche(_par_bond(), 20.0, 4.0, 6.0, pricing.BENCHMARK)
    wide = mk.quote_tranche(_par_bond(), 200.0, 4.0, 6.0, pricing.BENCHMARK)
    assert wide["px"] < tight["px"]


def test_quote_default_spread_by_class():
    # grp None falls back to the class letter; "B" maps to a wider target spread
    t = _par_bond()
    t.grp = None
    t.name = "B"
    q = mk.quote_tranche(t, None, 4.0, 6.0, pricing.BENCHMARK)
    assert q["spread"] == mk.CLASS_SPREAD["B"]


def test_hedge_scales_with_face():
    t = _par_bond()
    q = mk.quote_tranche(t, 40.0, 4.0, 6.0, pricing.BENCHMARK)
    # hedge notional ~ face * dur / tba_dur
    assert approx(q["hedge_mm"], q["face_mm"] * q["dur"] / 6.0)


def test_load_trace_maps_columns_and_scales_size():
    conn = sqlite3.connect(":memory:")
    mk.ensure_market(conn)
    fd, path = tempfile.mkstemp(suffix=".csv")
    os.close(fd)
    try:
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["CUSIP", "Trade_Price", "Yield", "Quantity",
                        "Buy_Sell", "Trade_Date"])
            w.writerow(["31418XYZ7", "100.25", "4.85", "5,000,000", "B",
                        "2026-06-08"])
            w.writerow(["", "99.0", "5.0", "1000000", "S", "2026-06-08"])  # no cusip
        n = mk.load_trace(conn, path)
    finally:
        os.unlink(path)
    assert n == 1                                  # the cusip-less row is skipped
    row = conn.execute("SELECT cusip,price,yield,size_mm,side FROM trace_print").fetchone()
    assert row[0] == "31418XYZ7"
    assert approx(row[1], 100.25)
    assert approx(row[3], 5.0)                      # 5,000,000 par -> 5.0 $MM
    assert row[4] == "B"


def main():
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print(f"\n{len(tests)} passed")


if __name__ == "__main__":
    main()
