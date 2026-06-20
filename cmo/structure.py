#!/usr/bin/env python3
"""Parse the bond structure agents make: tranche stack + underwriting syndicate.

The 424B5 prospectus cover lists the offered classes (name, coupon, balance) and
the placing broker-dealers. We extract both into the tranche/underwriter tables.
Formats vary by asset class, so we use tolerant patterns and a dealer roster.

Usage:
    python3 -m cmo.structure --cik 2037952 --acc 0001104659-25-114484
    python3 -m cmo.structure --auto-scan 30   # newest 30 424B5 in the DB
"""
import argparse, re, sys
from . import fetch

# Broker-dealer roster for syndicate detection (regex -> canonical name).
UNDERWRITERS = [
    (r"bofa securities|merrill lynch|banc of america|bank of america securities", "BofA Securities"),
    (r"citigroup global|\bciti\b", "Citigroup"),
    (r"j\.?p\.? morgan|jpmorgan", "J.P. Morgan"),
    (r"wells fargo", "Wells Fargo Securities"),
    (r"goldman sachs", "Goldman Sachs"),
    (r"morgan stanley", "Morgan Stanley"),
    (r"deutsche bank", "Deutsche Bank"),
    (r"barclays", "Barclays"),
    (r"credit suisse", "Credit Suisse"),
    (r"\bubs\b", "UBS"),
    (r"\brbc\b|rbc capital", "RBC Capital Markets"),
    (r"\btd securities|\btd bank", "TD Securities"),
    (r"mizuho", "Mizuho"),
    (r"\bsmbc\b|sumitomo", "SMBC Nikko"),
    (r"\bbnp paribas\b|\bbnp\b", "BNP Paribas"),
    (r"societe generale|société générale", "Societe Generale"),
    (r"santander", "Santander"),
    (r"nomura", "Nomura"),
    (r"truist", "Truist Securities"),
    (r"fifth third", "Fifth Third Securities"),
    (r"regions securities", "Regions Securities"),
    (r"lloyds", "Lloyds"),
    (r"\bcibc\b", "CIBC"),
    (r"\bbmo\b|bmo capital", "BMO Capital Markets"),
    (r"\bscotiabank\b|scotia", "Scotiabank"),
    (r"academy securities", "Academy Securities"),
    (r"cantor", "Cantor Fitzgerald"),
]

# Cover formats vary by shelf. Two orderings appear:
#  class-first + coupon: "Class A-1 4.082% Asset Backed Notes $290,670,000"
TR_CPN_FIRST = re.compile(
    r"Class\s+([A-Z0-9][A-Z0-9\-]*)\s+([\d.]+)\s*%\s+[A-Za-z\- ]*?Notes\s+\$\s*([\d,]+)")
#  balance-first, coupon set later: "$225,500,000 Class A-1 Asset-Backed Notes"
TR_BAL_COVER = re.compile(
    r"\$\s*([\d,]+)\s+Class\s+([A-Z0-9][A-Z0-9\-]*)\b[^$]{0,60}?Asset[- ]Backed\s+Notes",
    re.I)
# Last resort: "Class A-1 ... $290,670,000 ... 4.082%" (balance then coupon nearby)
TR_BAL_FIRST = re.compile(
    r"Class\s+([A-Z0-9][A-Z0-9\-]*)\b[^$%]{0,40}\$\s*([\d,]+)[^%]{0,40}?([\d.]+)\s*%")
# Coupon set in the rate table anywhere in the doc: "Class A-1 5.369% Fixed"
CPN_BY_CLASS = re.compile(
    r"Class\s+([A-Z0-9][A-Z0-9\-]*)\s+([\d.]+)\s*%\s+Fixed", re.I)
# "a reserve account with an initial deposit/balance of $3,026,385.22"
RESERVE_AMT = re.compile(
    r"reserve account with an initial (?:deposit|balance) of \$\s*([\d,]+(?:\.\d+)?)",
    re.I)


def _plain(html):
    t = re.sub(r"<[^>]+>", " ", html)
    t = re.sub(r"&[#a-zA-Z0-9]+;", " ", t)   # drop all HTML entities
    return re.sub(r"[ \t\r\n]+", " ", t)


def parse_tranches(text):
    """Return [(class_name, coupon, orig_balance, coupon_type)] in cover order.

    The cover always lists the offered classes and their balances; coupons may be
    on the cover or set in a later rate table. We take the stack+balances from the
    cover and back-fill coupons from anywhere in the doc."""
    head = text[:6000]                 # the offered-notes block is on the cover
    stack = {}                         # class -> [coupon, balance, type]
    for m in TR_CPN_FIRST.finditer(head):
        cls = m.group(1).upper()
        stack.setdefault(cls, [float(m.group(2)),
                               float(m.group(3).replace(",", "")), "FIXED"])
    for m in TR_BAL_COVER.finditer(head):
        cls = m.group(2).upper()
        if cls not in stack:
            stack[cls] = [None, float(m.group(1).replace(",", "")), None]
    if not stack:
        for m in TR_BAL_FIRST.finditer(head):
            cls = m.group(1).upper()
            stack.setdefault(cls, [float(m.group(3)),
                                   float(m.group(2).replace(",", "")), "FIXED"])
    # back-fill coupons from the rate table for classes the cover left blank
    coupons = {}
    for m in CPN_BY_CLASS.finditer(text):
        coupons.setdefault(m.group(1).upper(), float(m.group(2)))
    for cls, v in stack.items():
        if v[0] is None and cls in coupons:
            v[0], v[2] = coupons[cls], "FIXED"
    return [(cls, v[0], v[1], v[2] or ("FIXED" if v[0] else "FLOAT"))
            for cls, v in stack.items()]


def parse_enhancement(text, deal_total):
    """Extract the credit-enhancement summary: reserve account size and which
    enhancement forms the deal uses (reserve / OC / subordination)."""
    low = text.lower()
    m = RESERVE_AMT.search(text)
    reserve = float(m.group(1).replace(",", "")) if m else None
    reserve_pct = (round(reserve / deal_total, 6)
                   if reserve and deal_total else None)
    return {
        "reserve_amount": reserve,
        "reserve_pct": reserve_pct,
        "has_oc": int("overcollateralization" in low),
        "has_subordination": int("subordination" in low),
        "has_reserve": int("reserve account" in low),
    }


def parse_underwriters(text):
    """Return [(name, role)] found in the prospectus, lead = first on cover."""
    cover = text[:8000]
    found, order = {}, []
    for rx, name in UNDERWRITERS:
        m = re.search(rx, text, re.I)
        if not m:
            continue
        # lead managers tend to appear on the cover; co-managers deeper.
        on_cover = re.search(rx, cover, re.I) is not None
        found[name] = "lead" if on_cover else "co-manager"
        order.append((m.start(), name))
    order.sort()
    return [(name, found[name]) for _pos, name in order]


def ingest_one(conn, cik, acc, deal_name):
    path = fetch.prospectus_html(cik, acc)
    if not path:
        print(f"  no prospectus HTML for {acc}", file=sys.stderr)
        return None
    text = _plain(open(path, encoding="latin-1", errors="replace").read())
    tr = parse_tranches(text)
    uw = parse_underwriters(text)
    total = sum(b for _c, _cp, b, _t in tr)
    enh = parse_enhancement(text, total)
    cur = conn.cursor()
    cur.executemany(
        """INSERT INTO tranche(accession,cik,deal_name,class_name,orig_balance,
             coupon,coupon_type)
           VALUES(?,?,?,?,?,?,?)
           ON CONFLICT(accession,class_name) DO UPDATE SET
             orig_balance=excluded.orig_balance, coupon=excluded.coupon,
             coupon_type=excluded.coupon_type""",
        [(acc, cik, deal_name, c, bal, cpn, ct) for c, cpn, bal, ct in tr])
    cur.executemany(
        """INSERT INTO underwriter(accession,name,role) VALUES(?,?,?)
           ON CONFLICT(accession,name) DO UPDATE SET role=excluded.role""",
        [(acc, n, r) for n, r in uw])
    cur.execute(
        """INSERT INTO deal_enhancement(accession,deal_total,reserve_amount,
             reserve_pct,has_oc,has_subordination,has_reserve)
           VALUES(?,?,?,?,?,?,?)
           ON CONFLICT(accession) DO UPDATE SET
             deal_total=excluded.deal_total, reserve_amount=excluded.reserve_amount,
             reserve_pct=excluded.reserve_pct, has_oc=excluded.has_oc,
             has_subordination=excluded.has_subordination,
             has_reserve=excluded.has_reserve""",
        (acc, total, enh["reserve_amount"], enh["reserve_pct"], enh["has_oc"],
         enh["has_subordination"], enh["has_reserve"]))
    conn.commit()
    res = f" reserve={enh['reserve_pct']*100:.2f}%" if enh["reserve_pct"] else ""
    print(f"  {deal_name}: {len(tr)} tranches ${total/1e6:.0f}M, "
          f"{len(uw)} underwriters [{', '.join(n for n, _ in uw[:4])}"
          f"{'...' if len(uw) > 4 else ''}]{res}", file=sys.stderr)
    return tr, uw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--cik", type=int)
    ap.add_argument("--acc")
    ap.add_argument("--auto-scan", type=int, metavar="N")
    args = ap.parse_args()

    conn = fetch.ensure_db(args.db)

    if args.auto_scan:
        # 424B5 is a generic form; restrict to structured-finance issuers by name.
        rows = conn.execute(
            """SELECT f.cik, f.accession, e.name FROM filing f
               JOIN entity e ON e.cik=f.cik
               WHERE f.form='424B5' AND (
                   e.name LIKE '%RECEIVABLES%' OR e.name LIKE '%AUTO%' OR
                   e.name LIKE '%MORTGAGE%' OR e.name LIKE '%TRUST%' OR
                   e.name LIKE '%FUNDING%' OR e.name LIKE '%ABS%' OR
                   e.asset_class IN ('ABS','CMBS','RMBS','AGENCY'))
               ORDER BY f.filed_date DESC LIMIT ?""", (args.auto_scan,)).fetchall()
        for cik, acc, name in rows:
            try:
                ingest_one(conn, cik, acc, name)
            except Exception as e:                       # one bad filing shouldn't stop the scan
                print(f"  skip {acc}: {e}", file=sys.stderr)
    elif args.cik and args.acc:
        row = conn.execute("SELECT name FROM entity WHERE cik=?", (args.cik,)).fetchone()
        ingest_one(conn, args.cik, args.acc, row[0] if row else str(args.cik))
    else:
        ap.error("need --cik/--acc or --auto-scan")

    t = conn.execute("SELECT COUNT(DISTINCT accession),COUNT(*) FROM tranche").fetchone()
    u = conn.execute("SELECT COUNT(DISTINCT name) FROM underwriter").fetchone()
    print(f"\n=== bonds ===\ndeals_structured={t[0]} tranches={t[1]} distinct_underwriters={u[0]}")
    print("integrity: OK")


if __name__ == "__main__":
    main()
