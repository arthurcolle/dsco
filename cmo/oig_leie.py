#!/usr/bin/env python3
"""HHS OIG List of Excluded Individuals/Entities (LEIE) mirror.

Mirrors the public LEIE — every individual and entity excluded by the Office
of Inspector General from federal healthcare programs (Medicare, Medicaid,
TRICARE, et seq.). The data joins straight back to the existing NPI corpus
in ~/dsco-cli/data/people/npi.db via the npi column, closing a real
compliance loop: any provider record we ingest can be checked against
the LEIE in one SQL join.

Public-record basis: Social Security Act § 1128 (mandatory and permissive
exclusions) and 42 CFR Part 1001. OIG publishes the LEIE specifically so
covered providers and contractors can screen against it; nothing synthesized.

Source:
  - Monthly updated CSV at https://oig.hhs.gov/exclusions/downloadables/UPDATED.csv

Usage:
    python3 -m cmo.oig_leie --refresh
    python3 -m cmo.oig_leie --stats
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
LEIE_CSV_URL = os.environ.get(
    "OIG_LEIE_CSV_URL",
    "https://oig.hhs.gov/exclusions/downloadables/UPDATED.csv",
)
SQL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "oig_leie.sql")
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


def _norm(v):
    if v is None:
        return None
    s = str(v).strip()
    return s or None


def refresh(conn):
    """Re-download the LEIE CSV and INSERT OR REPLACE every row."""
    r = _get(LEIE_CSV_URL)
    if r is None:
        print(f"leie: download failed (OIG_LEIE_CSV_URL={LEIE_CSV_URL})")
        return 0
    text = r.content.decode("utf-8", errors="replace")
    reader = csv.DictReader(io.StringIO(text))
    cols = {c.upper().strip(): c for c in (reader.fieldnames or [])}

    def col(*names):
        for n in names:
            if n.upper() in cols:
                return cols[n.upper()]
        return None

    # OIG's headers are stable but defensive lookup keeps us tolerant of header drift.
    c_last     = col("LASTNAME", "LAST_NAME")
    c_first    = col("FIRSTNAME", "FIRST_NAME")
    c_mid      = col("MIDNAME", "MIDDLE_NAME")
    c_biz      = col("BUSNAME", "BUSINESS_NAME")
    c_general  = col("GENERAL")
    c_spec     = col("SPECIALTY")
    c_upin     = col("UPIN")
    c_npi      = col("NPI")
    c_dob      = col("DOB")
    c_addr     = col("ADDRESS")
    c_city     = col("CITY")
    c_state    = col("STATE")
    c_zip      = col("ZIP")
    c_extype   = col("EXCLTYPE", "EXCL_TYPE", "EXCLUSION_TYPE")
    c_exdate   = col("EXCLDATE", "EXCL_DATE")
    c_redate   = col("REINDATE", "REIN_DATE", "REINSTATE_DATE")
    c_wvdate   = col("WAIVERDATE", "WAIVER_DATE")
    c_wvstate  = col("WVRSTATE", "WAIVER_STATE")

    n = 0
    for row in reader:
        # OIG doesn't publish a single "exclusion_id" — the natural key is
        # (last_name, first_name, dob, exclusion_date). We synthesize a stable
        # string from that tuple so re-imports are idempotent.
        last  = _norm(row.get(c_last)) if c_last else None
        first = _norm(row.get(c_first)) if c_first else None
        biz   = _norm(row.get(c_biz)) if c_biz else None
        dob   = _norm(row.get(c_dob)) if c_dob else ""
        ed    = _norm(row.get(c_exdate)) if c_exdate else ""
        excl_id = "|".join([
            (last or "").upper(),
            (first or "").upper(),
            (biz or "").upper(),
            dob or "",
            ed or "",
        ])
        if not any([last, first, biz]):
            continue

        conn.execute(
            """INSERT OR REPLACE INTO oig_exclusion
                 (exclusion_id, last_name, first_name, midname, business_name,
                  general, specialty, upin, npi, dob, address, city, state, zip,
                  exclusion_type, exclusion_date, reinstate_date,
                  waiver_date, waiver_state)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (
                excl_id, last, first,
                _norm(row.get(c_mid)) if c_mid else None,
                biz,
                _norm(row.get(c_general)) if c_general else None,
                _norm(row.get(c_spec)) if c_spec else None,
                _norm(row.get(c_upin)) if c_upin else None,
                _norm(row.get(c_npi)) if c_npi else None,
                _norm(row.get(c_dob)) if c_dob else None,
                _norm(row.get(c_addr)) if c_addr else None,
                _norm(row.get(c_city)) if c_city else None,
                _norm(row.get(c_state)) if c_state else None,
                _norm(row.get(c_zip)) if c_zip else None,
                _norm(row.get(c_extype)) if c_extype else None,
                _norm(row.get(c_exdate)) if c_exdate else None,
                _norm(row.get(c_redate)) if c_redate else None,
                _norm(row.get(c_wvdate)) if c_wvdate else None,
                _norm(row.get(c_wvstate)) if c_wvstate else None,
            ),
        )
        n += 1
        if n % 5000 == 0:
            print(f"...leie {n} exclusions")
            conn.commit()
    conn.commit()
    print(f"leie: stored {n} exclusions")
    return n


def stats(conn):
    n = conn.execute("SELECT COUNT(*) FROM oig_exclusion").fetchone()[0]
    npi_count = conn.execute(
        "SELECT COUNT(*) FROM oig_exclusion WHERE npi IS NOT NULL AND npi != ''"
    ).fetchone()[0]
    print(f"exclusions {n:>8}")
    print(f"with NPI   {npi_count:>8}")
    print("\nby exclusion_type:")
    for et, cnt in conn.execute(
        "SELECT exclusion_type, COUNT(*) FROM oig_exclusion "
        "WHERE exclusion_type IS NOT NULL GROUP BY 1 ORDER BY 2 DESC LIMIT 10"
    ):
        print(f"  {cnt:>5}  {et}")
    print("\nby state:")
    for st, cnt in conn.execute(
        "SELECT state, COUNT(*) FROM oig_exclusion "
        "WHERE state IS NOT NULL GROUP BY 1 ORDER BY 2 DESC LIMIT 10"
    ):
        print(f"  {cnt:>5}  {st}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--refresh", action="store_true")
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    conn = sqlite3.connect(args.db, timeout=60)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=120000")
    ensure(conn)

    if args.refresh:
        refresh(conn)
    elif args.stats:
        stats(conn)
    else:
        ap.error("pick an action; see --help")


if __name__ == "__main__":
    main()
