#!/usr/bin/env python3
"""Ingest the SEC EDGAR structured-finance deal universe.

Every RMBS/CMBS/ABS/agency-CMO deal is a trust that registers with the SEC: a
prospectus (424B*) at issuance, a distribution report (10-D) every period, and
asset-level data (ABS-EE). EDGAR publishes a quarterly full-index of *all*
filings, ungated. We crawl those indexes, keep the structured-finance forms,
classify each issuing entity to a dealer shelf + asset class, and load into the
same cmo.db. From this we reverse-engineer who issues what, on what cadence,
through which broker-dealer — the substrate for a mortgage/ABS trading agent.

Usage:
    python3 -m cmo.edgar --start-year 2024 --end-year 2025 --db data/cmo/cmo.db
    python3 -m cmo.edgar --start-year 1994 --end-year 2025   # full history
"""
import argparse, os, re, sqlite3, sys, time
import requests

HERE = os.path.dirname(os.path.abspath(__file__))
IDX = "https://www.sec.gov/Archives/edgar/full-index/{y}/QTR{q}/form.idx"
# SEC requires a descriptive User-Agent with contact info.
UA = "dsco-research arthurcolle@gmail.com"

# Structured-finance form types worth keeping.
ABS_FORMS = {
    "424B5", "424B2", "424B3",   # prospectus supplements (the deal)
    "FWP",                       # free-writing prospectus (preliminary structure)
    "305B2",                     # designation of trustee
    "10-D",                      # periodic distribution report (the monthly)
    "ABS-EE",                    # asset-level exhibits
    "ABS-15G",                   # Reg AB representations/warranties
    "8-K",                       # material events (often distribution-linked)
}

# Dealer shelf detection — ordered, first match wins. Each entry: (regex, dealer).
DEALERS = [
    (r"ginnie mae|government national",                 "GINNIE"),
    (r"fannie mae|federal national mortgage",           "FANNIE"),
    (r"freddie mac|federal home loan mortgage|fremf|fhlmc", "FREDDIE"),
    (r"\bgs mortgage|goldman",                          "GS"),
    (r"morgan stanley|\bmsbam\b|\bmsc\b|\bmsci\b",       "MS"),
    (r"jpmorgan|j\.?p\.? morgan|jpmbb|jpmcc|jpmdb|\bchase\b", "JPM"),
    (r"wells fargo|wfrbs|wfcm|wachovia",                "WELLS"),
    (r"citigroup|\bciti\b|cgcmt|\bcd20|salomon",        "CITI"),
    (r"bank of america|banc of america|\bbank 20|baml|merrill|bacm", "BOFA"),
    (r"deutsche|comm 20|german american|dbjpm",         "DEUTSCHE"),
    (r"benchmark|\bbmark\b",                            "BENCHMARK"),
    (r"barclays|bbcms|bcap",                            "BARCLAYS"),
    (r"credit suisse|csail|csmc|csfb",                  "CS"),
    (r"\bubs\b|ubscm",                                  "UBS"),
    (r"nomura",                                         "NOMURA"),
    (r"cantor|cfcre|\bcf 20",                           "CANTOR"),
    (r"natixis|\bncms\b",                               "NATIXIS"),
    (r"\bladder|lccm",                                  "LADDER"),
    (r"starwood|\bstwd\b",                              "STARWOOD"),
]

CMBS_RE = re.compile(r"commercial mortgage|cmbs|fremf|bmark|benchmark|comm 20|bank 20", re.I)
RMBS_RE = re.compile(r"residential|rmbs|mortgage loan trust|home equity|heloc|mortgage[- ]backed|remic", re.I)
ABS_RE  = re.compile(r"\bauto\b|credit card|equipment|student loan|receivabl|\babs\b|floorplan|\blease", re.I)


def dealer_of(name):
    n = name.lower()
    for rx, who in DEALERS:
        if re.search(rx, n):
            return who
    return "OTHER"


def asset_class_of(name, dealer):
    if dealer in ("GINNIE", "FANNIE", "FREDDIE"):
        # Freddie multifamily (FREMF) is CMBS collateral; still an agency shelf.
        return "CMBS" if re.search(r"fremf|multifamily|\bk[- ]?series\b", name, re.I) else "AGENCY"
    if CMBS_RE.search(name):
        return "CMBS"
    if RMBS_RE.search(name):
        return "RMBS"
    if ABS_RE.search(name):
        return "ABS"
    return "OTHER"


LINE = re.compile(
    r"^(\S+)\s+(.*?)\s+(\d+)\s+(\d{4}-\d{2}-\d{2})\s+(edgar/\S+)\s*$")


def parse_index(text, forms):
    """Yield (form, name, cik, date, path, accession) for kept forms."""
    for ln in text.splitlines():
        m = LINE.match(ln)
        if not m:
            continue
        form, name, cik, date, path = m.groups()
        if form not in forms:
            continue
        acc = os.path.basename(path)
        if acc.endswith(".txt"):
            acc = acc[:-4]
        yield form, name.strip(), int(cik), date, path, acc


def ensure_db(db_path):
    conn = sqlite3.connect(db_path)
    with open(os.path.join(HERE, "edgar_schema.sql")) as f:
        conn.executescript(f.read())
    return conn


def fetch_index(session, y, q, cache):
    name = f"form_{y}Q{q}.idx"
    cached = os.path.join(cache, name)
    if os.path.exists(cached) and os.path.getsize(cached) > 1000:
        with open(cached, encoding="latin-1") as f:
            return f.read()
    url = IDX.format(y=y, q=q)
    r = session.get(url, timeout=120)
    if r.status_code != 200:
        return None
    with open(cached, "wb") as f:
        f.write(r.content)
    return r.content.decode("latin-1")


def load_index(conn, rows):
    entities, filings = {}, []
    for form, name, cik, date, path, acc in rows:
        d = dealer_of(name)
        ac = asset_class_of(name, d)
        e = entities.setdefault(cik, {"name": name, "dealer": d, "asset_class": ac,
                                      "first": date, "last": date})
        e["first"] = min(e["first"], date)
        e["last"] = max(e["last"], date)
        filings.append((acc, cik, form, date, path))
    cur = conn.cursor()
    for cik, e in entities.items():
        cur.execute(
            """INSERT INTO entity(cik,name,dealer,asset_class,first_filed,last_filed)
               VALUES(?,?,?,?,?,?)
               ON CONFLICT(cik) DO UPDATE SET
                 dealer=excluded.dealer, asset_class=excluded.asset_class,
                 first_filed=MIN(entity.first_filed,excluded.first_filed),
                 last_filed=MAX(entity.last_filed,excluded.last_filed)""",
            (cik, e["name"], e["dealer"], e["asset_class"], e["first"], e["last"]))
    cur.executemany(
        """INSERT INTO filing(accession,cik,form,filed_date,path) VALUES(?,?,?,?,?)
           ON CONFLICT(accession) DO NOTHING""", filings)
    conn.commit()
    return len(entities), len(filings)


def _verify(conn):
    c = conn.cursor()
    ent = c.execute("SELECT COUNT(*) FROM entity").fetchone()[0]
    fil = c.execute("SELECT COUNT(*) FROM filing").fetchone()[0]
    orphan = c.execute(
        "SELECT COUNT(*) FROM filing WHERE cik NOT IN (SELECT cik FROM entity)"
    ).fetchone()[0]
    span = c.execute("SELECT MIN(filed_date),MAX(filed_date) FROM filing").fetchone()
    print(f"\n=== EDGAR universe ===")
    print(f"entities={ent} filings={fil} span={span[0]}..{span[1]} orphan={orphan}")
    assert orphan == 0, "referential integrity broken"
    print("integrity: OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--start-year", type=int, default=2024)
    ap.add_argument("--end-year", type=int, default=2025)
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--cache", default="data/edgar")
    ap.add_argument("--forms", nargs="+", default=sorted(ABS_FORMS))
    ap.add_argument("--delay", type=float, default=0.3)
    args = ap.parse_args()

    os.makedirs(os.path.dirname(args.db), exist_ok=True)
    os.makedirs(args.cache, exist_ok=True)
    conn = ensure_db(args.db)
    forms = set(args.forms)

    session = requests.Session()
    session.headers.update({"User-Agent": UA, "Accept-Encoding": "gzip, deflate"})

    for y in range(args.start_year, args.end_year + 1):
        for q in (1, 2, 3, 4):
            text = fetch_index(session, y, q, args.cache)
            if text is None:
                continue
            rows = list(parse_index(text, forms))
            ne, nf = load_index(conn, rows)
            print(f"[{y}Q{q}] {len(rows)} kept -> {ne} entities, {nf} filings",
                  file=sys.stderr)
            time.sleep(args.delay)

    _verify(conn)


if __name__ == "__main__":
    main()
