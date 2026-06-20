#!/usr/bin/env python3
"""PCAOB Registered Public Accounting Firms mirror.

Mirrors the public list of every accounting firm registered with the Public
Company Accounting Oversight Board — the firms legally permitted to audit
US-listed issuers and SEC-registered broker-dealers. This is the auditor side
of the issuer/auditor/banker triangle that the FINRA broker corpus and the
SEC filer corpus already cover. Public-record basis: Sarbanes-Oxley Act § 102
(registration is a precondition to issuing public-company audit reports) and
the PCAOB's own published registration list.

Sources:
  - Current registered firms CSV: published from
    https://pcaobus.org/about/administration/registration/  (download link is
    re-issued periodically; the operator must supply the current URL via
    PCAOB_FIRMS_CSV_URL env var if the default 404s).
  - Inspection reports index:
    https://pcaobus.org/oversight/inspections/firm-inspection-reports

Usage:
    python3 -m cmo.pcaob --refresh-firms
    python3 -m cmo.pcaob --refresh-inspections
    python3 -m cmo.pcaob --stats
"""
import argparse
import csv
import io
import os
import sqlite3
import threading
import time

import requests

UA = "dsco-research arthurcolle@gmail.com"
# PCAOB renames the registered-firm CSV link periodically; allow override.
FIRMS_CSV_URL = os.environ.get(
    "PCAOB_FIRMS_CSV_URL",
    "https://pcaobus.org/about/administration/registration/Documents/RegisteredFirms.csv",
)
INSPECTIONS_INDEX_URL = "https://pcaobus.org/oversight/inspections/firm-inspection-reports"
SQL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pcaob.sql")
DEFAULT_DB = os.path.expanduser(os.environ.get("OPS_DB", "~/dsco-cli/data/cmo/cmo.db"))

_local = threading.local()


def session():
    s = getattr(_local, "session", None)
    if s is None:
        s = requests.Session()
        s.headers.update({"User-Agent": UA})
        _local.session = s
    return s


def ensure(conn):
    with open(SQL_PATH) as f:
        conn.executescript(f.read())
    conn.commit()


def _get(url, tries=4):
    for i in range(tries):
        try:
            r = session().get(url, timeout=120)
            if r.status_code == 200:
                return r
            if r.status_code in (429, 500, 502, 503):
                time.sleep(2 ** i)
                continue
            return None
        except requests.RequestException:
            time.sleep(2 ** i)
    return None


def _to_int(v):
    try:
        return int(str(v).strip())
    except (TypeError, ValueError):
        return None


def refresh_firms(conn):
    """Re-download the registered-firm list. INSERT OR REPLACE on firm_id."""
    r = _get(FIRMS_CSV_URL)
    if r is None:
        print(f"firms: download failed (PCAOB_FIRMS_CSV_URL={FIRMS_CSV_URL})")
        return 0
    # PCAOB CSVs are utf-8 with a header row. Column names have varied over the
    # years; map flexibly so a header rename doesn't silently drop rows.
    text = r.content.decode("utf-8", errors="replace")
    reader = csv.DictReader(io.StringIO(text))
    cols = {c.lower().strip(): c for c in (reader.fieldnames or [])}

    def col(*names):
        for n in names:
            if n in cols:
                return cols[n]
        return None

    c_id = col("firm id", "firmid", "pcaob firm id", "id")
    c_name = col("firm name", "name")
    c_country = col("country", "headquarters country")
    c_status = col("status", "registration status")
    c_reg = col("registration date", "registered", "date registered")
    c_scope = col("scope", "audits", "service category")
    c_city = col("city", "headquarters city", "hq city")
    c_state = col("state", "headquarters state", "hq state")

    if c_id is None or c_name is None:
        print(f"firms: unexpected CSV header: {reader.fieldnames}")
        return 0

    n = 0
    for row in reader:
        firm_id = _to_int(row.get(c_id))
        if firm_id is None:
            continue
        conn.execute(
            """INSERT OR REPLACE INTO pcaob_firm
                 (firm_id, name, country, status, registration_date,
                  scope, headquarters_city, headquarters_state)
               VALUES (?,?,?,?,?,?,?,?)""",
            (
                firm_id,
                (row.get(c_name) or "").strip() or None,
                ((row.get(c_country) or "").strip() or None) if c_country else None,
                ((row.get(c_status) or "").strip() or None) if c_status else None,
                ((row.get(c_reg) or "").strip() or None) if c_reg else None,
                ((row.get(c_scope) or "").strip() or None) if c_scope else None,
                ((row.get(c_city) or "").strip() or None) if c_city else None,
                ((row.get(c_state) or "").strip() or None) if c_state else None,
            ),
        )
        n += 1
        if n % 1000 == 0:
            print(f"...firms {n}")
            conn.commit()
    conn.commit()
    print(f"firms: stored {n} firms")
    return n


def refresh_inspections(conn):
    """Scrape the PCAOB inspection-reports index. Stub: this index is HTML and
    PCAOB does not provide a public JSON or CSV. The crawl logic is left to be
    written against the live page; this function asserts the table exists and
    is callable so the schema is honored end-to-end.
    """
    _ = conn  # parser unimplemented; arg kept for symmetry with refresh_firms
    r = _get(INSPECTIONS_INDEX_URL)
    if r is None:
        print("inspections: download failed")
        return 0
    # The actual parse is deferred — the inspection-reports index links out to
    # per-year PDF reports rather than a structured feed. Operator action:
    # decide whether to pull per-year PDFs or use the PCAOB API once published.
    print("inspections: stub — index downloaded but parser not implemented")
    return 0


def stats(conn):
    n = conn.execute("SELECT COUNT(*) FROM pcaob_firm").fetchone()[0]
    active = conn.execute(
        "SELECT COUNT(*) FROM pcaob_firm WHERE status='Registered'"
    ).fetchone()[0]
    insp = conn.execute("SELECT COUNT(*) FROM pcaob_inspection").fetchone()[0]
    print(f"pcaob firms        {n:>6}  ({active} registered)")
    print(f"inspection records {insp:>6}")
    print("\ntop countries:")
    for country, cnt in conn.execute(
        "SELECT country, COUNT(*) FROM pcaob_firm WHERE country IS NOT NULL "
        "GROUP BY country ORDER BY 2 DESC LIMIT 10"
    ):
        print(f"  {cnt:>4}  {country}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--refresh-firms", action="store_true")
    ap.add_argument("--refresh-inspections", action="store_true")
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    conn = sqlite3.connect(args.db, timeout=60)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=120000")
    ensure(conn)

    if args.refresh_firms:
        refresh_firms(conn)
    elif args.refresh_inspections:
        refresh_inspections(conn)
    elif args.stats:
        stats(conn)
    else:
        ap.error("pick an action; see --help")


if __name__ == "__main__":
    main()
