#!/usr/bin/env python3
"""Ingest Ginnie Mae REMIC tranche-factor disclosure into the CMO model.

The bulk files live at bulk.ginniemae.gov behind a SharePoint "download
profile" gate (registration, not real auth). We replay the gate with a
session, parse the fixed-width factor records, classify each tranche, and load
deals/classes/factors into SQLite. Self-verifies counts at the end.

Usage:
    python3 -m cmo.ingest --months 202605 [202604 ...] [--db data/cmo/cmo.db]
"""
import argparse, io, os, re, sqlite3, sys, zipfile
import requests

BULK = "https://bulk.ginniemae.gov"
FILE_URL = BULK + "/protectedfiledownload.aspx?dlfile=data_bulk/{name}"
PROFILE = "https://www.ginniemae.gov/pages/profile.aspx?src="
HERE = os.path.dirname(os.path.abspath(__file__))

UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0 Safari/537.36")
PROFILE_EMAIL = os.environ.get("GINNIE_EMAIL", "research@distributed.systems")


def _hidden_fields(html):
    fields = {}
    for m in re.finditer(r'<input[^>]*type="hidden"[^>]*>', html, re.I):
        tag = m.group(0)
        n = re.search(r'name="([^"]+)"', tag)
        v = re.search(r'value="([^"]*)"', tag)
        if n:
            fields[n.group(1)] = v.group(1) if v else ""
    return fields


def _is_zip(b):
    return b[:2] == b"PK"


def download(session, name):
    """Return raw zip bytes for a bulk file, replaying the profile gate."""
    url = FILE_URL.format(name=name)
    r = session.get(url, headers={"User-Agent": UA}, allow_redirects=True, timeout=60)
    if _is_zip(r.content):
        return r.content
    # Gated: we landed on the SharePoint profile page. Submit the email form.
    html = r.text
    fields = _hidden_fields(html)
    # Find the email textbox + the query button in the profile webpart.
    email_name = None
    for m in re.finditer(r'<input[^>]*name="([^"]*)"[^>]*>', html, re.I):
        nm = m.group(1)
        if nm.lower().endswith("txtemail") or "Email" in nm and "txt" in nm:
            email_name = nm
            break
    btn = next((k for k in re.findall(r'name="([^"]*btnQueryEmail[^"]*)"', html)), None)
    if email_name and btn:
        fields[email_name] = PROFILE_EMAIL
        fields[btn] = "Continue"
        session.post(r.url, data=fields, headers={"User-Agent": UA}, timeout=60)
    # Retry the direct file URL now that the session carries the profile cookie.
    r2 = session.get(url, headers={"User-Agent": UA}, allow_redirects=True, timeout=60)
    if _is_zip(r2.content):
        return r2.content
    raise RuntimeError(f"{name}: gate not passed (got {len(r2.content)}B, "
                       f"content-type {r2.headers.get('content-type')})")


# Fixed-width is whitespace-aligned in these files; tranche records start "2".
def parse_factor_records(text):
    """Yield dicts for each tranche (record type 2) line."""
    for ln in text.splitlines():
        if not ln or ln[0] != "2":
            continue
        toks = ln[1:].split()
        if len(toks) < 10:
            continue
        series, class_id, cusip, factor, coupon, maturity, orig, cur, rpt, issue = toks[:10]
        yield {
            "series": series.strip(),
            "class_id": class_id.strip(),
            "cusip": cusip.strip(),
            "factor": float(factor),
            "coupon": float(coupon),
            "maturity": maturity.strip(),
            "orig_balance": float(orig),
            "cur_balance": float(cur),
            "report_date": rpt.strip(),
            "issue_date": issue.strip(),
        }


def classify(class_id, coupon, orig_balance):
    """Heuristic tranche-type from the class id + economics. Refined later by
    the offering circular, but the class-letter conventions are strong:
      Z accrual, S/inverse, F/floater, IO/PO strips, R/RS residual, else SEQ."""
    c = (class_id or "").upper()
    if c in ("R", "RS", "RL", "RR"):
        return "RES"
    if c.endswith("IO") or c == "IO" or (orig_balance == 0 and coupon > 0):
        return "IO"
    if c.endswith("PO") or c == "PO" or (coupon == 0 and orig_balance > 0):
        return "PO"
    if c.startswith("S") and not c.startswith("SC"):
        return "INV"        # S-prefixed classes are conventionally inverse floaters
    if c.startswith("F"):
        return "FLT"        # F-prefixed classes are conventionally floaters
    if c.startswith("Z"):
        return "Z"          # accrual / accretion-directed
    if c.startswith("P"):
        return "PAC"        # P-family: PAC/PAC-companion conventions
    if c.startswith("T"):
        return "TAC"
    return "SEQ"


def issuer_from_series(series):
    s = series.upper()
    if s.startswith("GNMA"):
        return "GNMA"
    if s.startswith("FNMA") or s.startswith("FN"):
        return "FNMA"
    if s.startswith("FHLMC") or s.startswith("FH") or s.startswith("FR"):
        return "FHLMC"
    return "GNMA"


def year_from_series(series):
    m = re.search(r"(\d{4})", series)
    return int(m.group(1)) if m else None


def ensure_db(db_path):
    conn = sqlite3.connect(db_path)
    with open(os.path.join(HERE, "schema.sql")) as f:
        conn.executescript(f.read())
    return conn


def load(conn, records, report_month):
    deals, classes, factors = {}, [], []
    for r in records:
        deal_id = r["series"]
        issuer = issuer_from_series(deal_id)
        ctype = classify(r["class_id"], r["coupon"], r["orig_balance"])
        d = deals.setdefault(deal_id, {"issuer": issuer, "series": deal_id,
                                       "year": year_from_series(deal_id),
                                       "n": 0, "orig": 0.0})
        d["n"] += 1
        d["orig"] += r["orig_balance"]
        classes.append((r["cusip"], deal_id, r["class_id"], ctype, r["coupon"],
                        r["orig_balance"], r["maturity"], r["issue_date"]))
        factors.append((r["cusip"], report_month, r["factor"], r["cur_balance"]))

    cur = conn.cursor()
    for did, d in deals.items():
        cur.execute(
            """INSERT INTO deal(deal_id,issuer,series,year,n_classes,orig_balance,first_seen)
               VALUES(?,?,?,?,?,?,?)
               ON CONFLICT(deal_id) DO UPDATE SET
                 n_classes=excluded.n_classes, orig_balance=excluded.orig_balance""",
            (did, d["issuer"], d["series"], d["year"], d["n"], d["orig"], report_month))
    cur.executemany(
        """INSERT INTO class(cusip,deal_id,class_id,class_type,coupon,orig_balance,maturity,issue_date)
           VALUES(?,?,?,?,?,?,?,?)
           ON CONFLICT(cusip) DO UPDATE SET class_type=excluded.class_type""", classes)
    cur.executemany(
        """INSERT INTO factor(cusip,report_month,factor,cur_balance) VALUES(?,?,?,?)
           ON CONFLICT(cusip,report_month) DO UPDATE SET
             factor=excluded.factor, cur_balance=excluded.cur_balance""", factors)
    conn.commit()
    return len(deals), len(classes), len(factors)


def _verify(conn, db_path):
    c = conn.cursor()
    deals = c.execute("SELECT COUNT(*) FROM deal").fetchone()[0]
    classes = c.execute("SELECT COUNT(*) FROM class").fetchone()[0]
    facts = c.execute("SELECT COUNT(*) FROM factor").fetchone()[0]
    orphans = c.execute(
        "SELECT COUNT(*) FROM class WHERE deal_id NOT IN (SELECT deal_id FROM deal)"
    ).fetchone()[0]
    print(f"\n=== DB {db_path} ===")
    print(f"deals={deals} classes={classes} factors={facts} orphan_classes={orphans}")
    assert orphans == 0, "referential integrity broken"
    print("integrity: OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--months", nargs="+", default=["202605"])
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--tranches", nargs="+", default=["remic1", "remic2"])
    ap.add_argument("--cache", default="data/ginnie")
    ap.add_argument("--raw", nargs="+", help="load already-extracted .txt factor files")
    ap.add_argument("--report-month", default="SAMPLE", help="report month tag for --raw")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(args.db), exist_ok=True)
    os.makedirs(args.cache, exist_ok=True)
    conn = ensure_db(args.db)

    if args.raw:
        for path in args.raw:
            text = open(path, encoding="latin-1").read()
            recs = list(parse_factor_records(text))
            nd, nc, nf = load(conn, recs, args.report_month)
            print(f"[load] {path}: {len(recs)} records -> "
                  f"{nd} deals, {nc} classes, {nf} factors", file=sys.stderr)
        _verify(conn, args.db)
        return

    session = requests.Session()

    grand = [0, 0, 0]
    for ym in args.months:
        for tr in args.tranches:
            name = f"{tr}_{ym}.zip"
            cached = os.path.join(args.cache, name)
            if os.path.exists(cached) and _is_zip(open(cached, "rb").read(2)):
                blob = open(cached, "rb").read()
            else:
                print(f"[download] {name}", file=sys.stderr)
                blob = download(session, name)
                with open(cached, "wb") as f:
                    f.write(blob)
            zf = zipfile.ZipFile(io.BytesIO(blob))
            text = zf.read(zf.namelist()[0]).decode("latin-1")
            recs = list(parse_factor_records(text))
            nd, nc, nf = load(conn, recs, ym)
            print(f"[load] {name}: {len(recs)} records -> "
                  f"{nd} deals, {nc} classes, {nf} factors", file=sys.stderr)
            grand[1] += nc

    _verify(conn, args.db)


if __name__ == "__main__":
    main()
