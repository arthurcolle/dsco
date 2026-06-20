#!/usr/bin/env python3
"""Market & execution layer: turn a priced deal into a quotable two-way market,
record desk axes and client RFQs, and mark against real traded prints.

The pricing engine gives model value; a desk has to quote a bid and an offer,
size a hedge, and reconcile to where bonds actually trade. This module:

  - quote: for each surviving tranche, derive a fair price from a target spread
    over the benchmark curve, build a two-way market around it, and size a
    rates hedge (DV01 / duration ratio to a current-coupon TBA);
  - axe:   record/show the levels the desk is showing on a class;
  - rfq:   log a client/street bid-wanted or offer-wanted and the desk's quote;
  - trace: load actual FINRA TRACE Securitized-Products prints from a CSV export
    so the desk can mark to realized levels (never synthesized).

Usage:
    python3 -m cmo.market --quote --deal "WORLD OMNI AUTO RECEIVABLES TRUST 2024-C" \
        --cpr 0.13 --cdr 0.01 --sev 0.45 --target-spread 35
    python3 -m cmo.market --axe-set --deal KEY --class A2 --side offer --level 100.1 --size 25
    python3 -m cmo.market --axe-list
    python3 -m cmo.market --rfq-add --deal KEY --class A2 --side bid_wanted --size 25 --client "Tyler Wick"
    python3 -m cmo.market --rfq-list
    python3 -m cmo.market --load-trace prints.csv
"""
import argparse, csv, os
from . import fetch, pricing, cashflow as cf

# Target new-issue I-spread (bps) by credit-class letter — the level the desk
# models a class to when there is no observed print. Seniority widens the spread.
CLASS_SPREAD = {"A": 30.0, "B": 90.0, "C": 160.0, "D": 280.0, "E": 400.0}


def ensure_market(conn):
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "market.sql")) as f:
        conn.executescript(f.read())
    conn.commit()


def _class_letter(name):
    return (name or "?").strip()[:1].upper()


def quote_tranche(t, target_bps, half32, tba_dur, curve):
    """Two-way market + hedge for one tranche at a target spread (bps)."""
    w = cf.wal(t.flows)
    if w <= 0 or not t.flows:
        return None
    spread = (target_bps if target_bps is not None
              else CLASS_SPREAD.get(t.grp or _class_letter(t.name), 120.0))
    y = pricing.interp(curve, w) + spread / 1e4
    px = pricing.price_at_yield(t, y)
    dur, _cvx = pricing.eff_duration_convexity(t, y)
    half = half32 / 32.0                       # bid/offer half-width in price pts
    dv01 = px * dur * 1e-4 if dur else None     # price move per 1bp, per 100 face
    hedge_mm = t.orig / 1e6 * (dur / tba_dur) if dur and tba_dur else None
    return {"class": t.name, "wal": w, "yield": y, "spread": spread, "px": px,
            "bid": px - half, "offer": px + half, "dur": dur, "dv01": dv01,
            "face_mm": t.orig / 1e6, "hedge_mm": hedge_mm}


def quote_deal(conn, deal_key, cpr, cdr, sev, target_bps, half32, tba_dur, curve):
    _meta, tranches = pricing.run_deal(conn, deal_key, cpr, cdr, sev)
    out = []
    for t in tranches:
        q = quote_tranche(t, target_bps, half32, tba_dur, curve)
        if q:
            out.append(q)
    return out


def _print_quotes(deal, rows):
    hdr = (f"{'DEAL':<35}{'CLS':<5}{'WAL':>6}{'SPR':>6}{'BID':>9}{'OFFER':>9}"
           f"{'DUR':>6}{'DV01':>7}{'FACE$M':>9}{'HEDGE$M':>9}")
    print(hdr); print("-" * len(hdr))
    for q in rows:
        dv = "" if q["dv01"] is None else f"{q['dv01']:.4f}"
        hg = "" if q["hedge_mm"] is None else f"{q['hedge_mm']:.1f}"
        print(f"{deal[:34]:<35}{q['class']:<5}{q['wal']:>6.2f}{q['spread']:>6.0f}"
              f"{q['bid']:>9.3f}{q['offer']:>9.3f}{q['dur']:>6.2f}{dv:>7}"
              f"{q['face_mm']:>9.1f}{hg:>9}")


def set_axe(conn, deal_key, cls, side, level, unit, size, trader):
    conn.execute(
        """INSERT INTO desk_axe(deal_key,class_name,side,level,unit,size_mm,trader)
           VALUES(?,?,?,?,?,?,?)
           ON CONFLICT(deal_key,class_name,side) DO UPDATE SET
             level=excluded.level, unit=excluded.unit, size_mm=excluded.size_mm,
             trader=excluded.trader, ts=datetime('now')""",
        (deal_key, cls, side, level, unit, size, trader))
    conn.commit()


def list_axe(conn):
    rows = conn.execute(
        """SELECT deal_key,class_name,side,level,unit,size_mm,trader,ts
           FROM desk_axe ORDER BY ts DESC""").fetchall()
    print(f"{'DEAL':<35}{'CLS':<5}{'SIDE':<7}{'LEVEL':>9}{'UNIT':>7}"
          f"{'SIZE$M':>8}  TRADER")
    for dk, c, sd, lv, u, sz, tr, _ts in rows:
        print(f"{(dk or '?')[:34]:<35}{c:<5}{sd:<7}{lv or 0:>9.3f}{u:>7}"
              f"{sz or 0:>8.1f}  {tr or '-'}")


def add_rfq(conn, deal_key, cls, side, size, client):
    cur = conn.execute(
        """INSERT INTO rfq(deal_key,class_name,side,size_mm,client)
           VALUES(?,?,?,?,?)""", (deal_key, cls, side, size, client))
    conn.commit()
    return cur.lastrowid


def quote_rfq(conn, rfq_id, level, status):
    conn.execute("UPDATE rfq SET quote=?, status=? WHERE id=?",
                 (level, status, rfq_id))
    conn.commit()


def list_rfq(conn):
    rows = conn.execute(
        """SELECT id,deal_key,class_name,side,size_mm,client,quote,status,ts
           FROM rfq ORDER BY id DESC""").fetchall()
    print(f"{'ID':>4}{'DEAL':<35}{'CLS':<5}{'SIDE':<13}{'SIZE$M':>8}"
          f"{'QUOTE':>9}{'STATUS':>9}  CLIENT")
    for i, dk, c, sd, sz, cl, q, st, _ts in rows:
        qs = "" if q is None else f"{q:.3f}"
        print(f"{i:>4}{(dk or '?')[:34]:<35}{c or '-':<5}{sd or '-':<13}"
              f"{sz or 0:>8.1f}{qs:>9}{st:>9}  {cl or '-'}")


# FINRA TRACE Securitized-Products export columns vary by vintage; accept the
# common header names and map them to our schema. Unknown columns are ignored.
TRACE_COLS = {
    "cusip": "cusip", "cusip_id": "cusip",
    "price": "price", "trade_price": "price", "rptd_pr": "price",
    "yield": "yield", "yld_pt": "yield",
    "quantity": "size_mm", "trade_size": "size_mm", "entrd_vol_qt": "size_mm",
    "side": "side", "buy_sell": "side", "rpt_side_cd": "side",
    "execution_date": "exec_ts", "trade_date": "exec_ts", "trd_exctn_dt": "exec_ts",
}


def load_trace(conn, path):
    """Ingest real FINRA TRACE prints from a CSV export. Maps known columns,
    converts par quantity to $MM, and leaves deal_key/class_name NULL (matched
    later). Never fabricates rows — a missing file is an error, not a stub."""
    n = 0
    with open(path, newline="") as f:
        rd = csv.DictReader(f)
        for raw in rd:
            rec = {}
            for k, v in raw.items():
                col = TRACE_COLS.get((k or "").strip().lower())
                if col:
                    rec[col] = v
            if not rec.get("cusip"):
                continue
            size_mm = None
            if rec.get("size_mm"):
                try:
                    size_mm = float(str(rec["size_mm"]).replace(",", "")) / 1e6
                except ValueError:
                    size_mm = None
            conn.execute(
                """INSERT INTO trace_print(cusip,side,price,yield,size_mm,exec_ts)
                   VALUES(?,?,?,?,?,?)""",
                (rec.get("cusip"), rec.get("side"),
                 _f(rec.get("price")), _f(rec.get("yield")), size_mm,
                 rec.get("exec_ts")))
            n += 1
    conn.commit()
    return n


def _f(x):
    try:
        return float(str(x).replace(",", ""))
    except (TypeError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--quote", action="store_true")
    ap.add_argument("--rv-screen", action="store_true",
                    help="quote every fully-linked deal")
    ap.add_argument("--deal")
    ap.add_argument("--class", dest="cls")
    ap.add_argument("--cpr", type=float, default=0.0)
    ap.add_argument("--cdr", type=float, default=0.0)
    ap.add_argument("--sev", type=float, default=0.0)
    ap.add_argument("--target-spread", type=float, default=None,
                    help="target I-spread bps (default: per credit class)")
    ap.add_argument("--half32", type=float, default=4.0,
                    help="bid/offer half-width in 32nds (default 4 = 0.125)")
    ap.add_argument("--tba-dur", type=float, default=6.0,
                    help="current-coupon TBA duration for the hedge ratio")
    ap.add_argument("--curve", type=pricing.parse_curve, default=pricing.BENCHMARK)
    ap.add_argument("--side")
    ap.add_argument("--level", type=float)
    ap.add_argument("--unit", default="price")
    ap.add_argument("--size", type=float)
    ap.add_argument("--trader")
    ap.add_argument("--client")
    ap.add_argument("--axe-set", action="store_true")
    ap.add_argument("--axe-list", action="store_true")
    ap.add_argument("--rfq-add", action="store_true")
    ap.add_argument("--rfq-quote", type=int, metavar="ID")
    ap.add_argument("--rfq-status", default="quoted")
    ap.add_argument("--rfq-list", action="store_true")
    ap.add_argument("--load-trace", metavar="CSV")
    args = ap.parse_args()

    conn = fetch.ensure_db(args.db)
    ensure_market(conn)

    if args.quote or args.rv_screen:
        deals = ([args.deal] if args.deal else
                 pricing.linked_deals(conn) if args.rv_screen else None)
        if not deals:
            ap.error("need --deal <key> or --rv-screen")
        for dk in deals:
            rows = quote_deal(conn, dk, args.cpr, args.cdr, args.sev,
                              args.target_spread, args.half32, args.tba_dur,
                              args.curve)
            if rows:
                _print_quotes(dk, rows)
    elif args.axe_set:
        if not (args.deal and args.cls and args.side):
            ap.error("--axe-set needs --deal --class --side [--level --size]")
        set_axe(conn, args.deal, args.cls, args.side, args.level, args.unit,
                args.size, args.trader)
        list_axe(conn)
    elif args.axe_list:
        list_axe(conn)
    elif args.rfq_add:
        if not (args.deal and args.side):
            ap.error("--rfq-add needs --deal --side [--class --size --client]")
        rid = add_rfq(conn, args.deal, args.cls, args.side, args.size, args.client)
        print(f"rfq #{rid} logged")
        list_rfq(conn)
    elif args.rfq_quote is not None:
        quote_rfq(conn, args.rfq_quote, args.level, args.rfq_status)
        list_rfq(conn)
    elif args.rfq_list:
        list_rfq(conn)
    elif args.load_trace:
        n = load_trace(conn, args.load_trace)
        print(f"loaded {n} TRACE prints from {args.load_trace}")
    else:
        ap.error("pick an action: --quote/--rv-screen, --axe-set/--axe-list, "
                 "--rfq-add/--rfq-quote/--rfq-list, --load-trace")


if __name__ == "__main__":
    main()
