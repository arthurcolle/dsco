#!/usr/bin/env python3
"""CMS Open Payments — General Payments mirror.

Mirrors the public CMS Open Payments General Payments dataset — every payment
or other transfer of value from drug / device manufacturers and GPOs to US
physicians, NPPs, and teaching hospitals. Joins back to the existing NPI
corpus (people/npi.db) via covered_recipient_npi and to the SEC filer corpus
by applicable_manufacturer_or_applicable_gpo_name, closing the conflict-of-
interest loop on the provider / pharma / device-maker triangle.

Public-record basis: Affordable Care Act § 6002 (the Physician Payments
Sunshine Act) and 42 CFR Part 403, Subpart I. CMS publishes the bulk files
specifically for public re-analysis; nothing synthesized.

Source:
  - https://www.cms.gov/openpayments/data — per-year ZIPs. The actual file
    URLs include a CMS-assigned download token that changes per release;
    set CMS_OP_GENERAL_URL_<YEAR> env vars for non-default years, or pass
    --url <URL> to override.

Sizes are large: a single program year is typically ~10M+ General Payments
rows and the CSV alone runs 1-3 GB. Use --chunksize to bound memory.

Usage:
    python3 -m cmo.open_payments --refresh 2023
    python3 -m cmo.open_payments --refresh 2023 --url https://...
    python3 -m cmo.open_payments --all-years
    python3 -m cmo.open_payments --stats
"""
import argparse
import csv
import io
import os
import sqlite3
import threading
import time
import zipfile
from datetime import datetime, timezone

import requests

UA = "dsco-research arthurcolle@gmail.com"
SQL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "open_payments.sql")
DEFAULT_DB = os.path.expanduser(os.environ.get("OPS_DB", "~/dsco-cli/data/cmo/cmo.db"))

# Default year range. CMS started publishing in 2013 program year.
FIRST_YEAR = 2013
LAST_YEAR = datetime.now(timezone.utc).year - 1

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


def _url_for(year):
    """Read the per-year URL from env, or return None if not configured."""
    return os.environ.get(f"CMS_OP_GENERAL_URL_{year}")


def _to_float(v):
    try:
        return float(str(v).replace("$", "").replace(",", "").strip())
    except (TypeError, ValueError):
        return None


def _to_int(v):
    try:
        return int(str(v).strip())
    except (TypeError, ValueError):
        return None


def _open_general_csv(url):
    """Stream-download a per-year ZIP and yield rows from the General Payments CSV inside."""
    r = session().get(url, stream=True, timeout=600)
    if r.status_code != 200:
        return None
    # CMS bundles per-year payments as a ZIP with multiple CSVs; the General
    # Payments file is conventionally named OP_DTL_GNRL_PGYR<YYYY>_*.csv.
    data = io.BytesIO(r.content)
    z = zipfile.ZipFile(data)
    names = [n for n in z.namelist() if "GNRL" in n.upper() and n.lower().endswith(".csv")]
    if not names:
        return None
    name = sorted(names)[0]
    f = io.TextIOWrapper(z.open(name, "r"), encoding="utf-8", errors="replace")
    return csv.DictReader(f)


def refresh_year(conn, year, url=None, chunksize=10000):
    url = url or _url_for(year)
    if not url:
        print(f"refresh: no URL configured for year {year}. "
              f"Set CMS_OP_GENERAL_URL_{year} or pass --url.")
        return 0

    reader = _open_general_csv(url)
    if reader is None:
        print(f"refresh: download or unzip failed for year {year}")
        return 0

    # CMS column names use the long Sunshine-Act schema. Map flexibly so a CMS
    # rename doesn't silently drop rows. All keys upper-cased.
    cols = {c.upper().strip(): c for c in (reader.fieldnames or [])}

    def col(*names):
        for n in names:
            if n.upper() in cols:
                return cols[n.upper()]
        return None

    c_record       = col("Record_ID")
    c_npi          = col("Covered_Recipient_NPI")
    c_first        = col("Covered_Recipient_First_Name", "Physician_First_Name")
    c_last         = col("Covered_Recipient_Last_Name", "Physician_Last_Name")
    c_state        = col("Recipient_State")
    c_manuf        = col("Applicable_Manufacturer_or_Applicable_GPO_Making_Payment_Name",
                         "Submitting_Applicable_Manufacturer_or_Applicable_GPO_Name")
    c_date         = col("Date_of_Payment")
    c_amount       = col("Total_Amount_of_Payment_USDollars")
    c_form         = col("Form_of_Payment_or_Transfer_of_Value")
    c_nature       = col("Nature_of_Payment_or_Transfer_of_Value")
    c_prod_ind     = col("Indicate_Drug_or_Biological_or_Device_or_Medical_Supply_1",
                         "Related_Product_Indicator")
    c_prod_name    = col("Name_of_Drug_or_Biological_or_Device_or_Medical_Supply_1",
                         "Related_Product_Name")

    if c_record is None:
        print(f"refresh: unexpected schema for year {year} — Record_ID missing")
        return 0

    n = 0
    total_dollars = 0.0
    pending = 0
    for row in reader:
        rid = _to_int(row.get(c_record))
        if rid is None:
            continue
        amount = _to_float(row.get(c_amount)) if c_amount else None
        if amount is not None:
            total_dollars += amount
        conn.execute(
            """INSERT OR REPLACE INTO cms_op_general
                 (record_id, covered_recipient_npi,
                  covered_recipient_first_name, covered_recipient_last_name,
                  recipient_state,
                  applicable_manufacturer_or_applicable_gpo_name,
                  payment_date, total_amount_of_payment,
                  payment_form, nature_of_payment,
                  related_product_indicator, related_product_name,
                  program_year)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (
                rid,
                (row.get(c_npi) or "").strip() or None,
                (row.get(c_first) or "").strip() or None,
                (row.get(c_last) or "").strip() or None,
                (row.get(c_state) or "").strip() or None,
                (row.get(c_manuf) or "").strip() or None,
                (row.get(c_date) or "").strip() or None,
                amount,
                (row.get(c_form) or "").strip() or None,
                (row.get(c_nature) or "").strip() or None,
                (row.get(c_prod_ind) or "").strip() or None,
                (row.get(c_prod_name) or "").strip() or None,
                year,
            ),
        )
        n += 1
        pending += 1
        if pending >= chunksize:
            conn.commit()
            pending = 0
            print(f"...year {year}: {n} payments")
    conn.commit()
    conn.execute(
        """INSERT OR REPLACE INTO cms_op_program_year
             (program_year, n_records, total_amount_dollars, last_refreshed)
           VALUES (?,?,?,?)""",
        (year, n, total_dollars, datetime.now(timezone.utc).isoformat(timespec="seconds")),
    )
    conn.commit()
    print(f"year {year}: stored {n} payments, ${total_dollars:,.0f} total")
    return n


def refresh_all_years(conn, chunksize=10000):
    total = 0
    for y in range(FIRST_YEAR, LAST_YEAR + 1):
        if not _url_for(y):
            print(f"year {y}: skipped (no CMS_OP_GENERAL_URL_{y})")
            continue
        total += refresh_year(conn, y, chunksize=chunksize)
        time.sleep(2)
    return total


def stats(conn):
    n = conn.execute("SELECT COUNT(*) FROM cms_op_general").fetchone()[0]
    npi_count = conn.execute(
        "SELECT COUNT(DISTINCT covered_recipient_npi) FROM cms_op_general "
        "WHERE covered_recipient_npi IS NOT NULL AND covered_recipient_npi != ''"
    ).fetchone()[0]
    total = conn.execute(
        "SELECT COALESCE(SUM(total_amount_of_payment),0) FROM cms_op_general"
    ).fetchone()[0]
    print(f"payments           {n:>12}")
    print(f"distinct NPIs      {npi_count:>12}")
    print(f"total $ paid out   ${total:>12,.0f}")
    print("\nby program year:")
    for y, cnt, tot in conn.execute(
        "SELECT program_year, n_records, total_amount_dollars "
        "FROM cms_op_program_year ORDER BY program_year"
    ):
        print(f"  {y}  {cnt:>10}  ${tot:>14,.0f}")
    print("\ntop manufacturers:")
    for m, s in conn.execute(
        "SELECT applicable_manufacturer_or_applicable_gpo_name, "
        "SUM(total_amount_of_payment) "
        "FROM cms_op_general "
        "WHERE applicable_manufacturer_or_applicable_gpo_name IS NOT NULL "
        "GROUP BY 1 ORDER BY 2 DESC LIMIT 10"
    ):
        print(f"  ${s:>14,.0f}  {m}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--refresh", type=int, metavar="YEAR",
                    help=f"download + parse one program year ({FIRST_YEAR}-{LAST_YEAR})")
    ap.add_argument("--url", default=None,
                    help="override the URL for the per-year ZIP")
    ap.add_argument("--all-years", action="store_true",
                    help=f"loop {FIRST_YEAR}-{LAST_YEAR} using CMS_OP_GENERAL_URL_<YEAR> env vars")
    ap.add_argument("--chunksize", type=int, default=10000)
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    conn = sqlite3.connect(args.db, timeout=120)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=120000")
    ensure(conn)

    if args.refresh is not None:
        refresh_year(conn, args.refresh, url=args.url, chunksize=args.chunksize)
    elif args.all_years:
        refresh_all_years(conn, chunksize=args.chunksize)
    elif args.stats:
        stats(conn)
    else:
        ap.error("pick an action; see --help")


if __name__ == "__main__":
    main()
