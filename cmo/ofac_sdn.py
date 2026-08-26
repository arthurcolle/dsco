#!/usr/bin/env python3
"""Treasury OFAC Specially Designated Nationals (SDN) list mirror.

Mirrors the public OFAC sanctions data — every sanctioned person, entity, and
vessel the US Treasury has designated — into local SQLite for cross-referencing
against the firm/individual graph (FINRA brokers, PCAOB auditors, NPI providers,
SEC filers, etc.). Public-record basis: 31 CFR Part 501, the SDN List itself,
and the underlying Executive Orders (12947, 13224, 13694, et seq.). Nothing
synthesized; OFAC publishes the CSVs.

Sources (legacy CSV trio — the most reliable feed):
  - https://www.treasury.gov/ofac/downloads/sdn.csv      (primary entities)
  - https://www.treasury.gov/ofac/downloads/sdn_alt.csv  (aliases)
  - https://www.treasury.gov/ofac/downloads/sdn_add.csv  (addresses)

Usage:
    python3 -m cmo.ofac_sdn --refresh
    python3 -m cmo.ofac_sdn --stats
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
BASE = "https://www.treasury.gov/ofac/downloads"
SQL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ofac_sdn.sql")
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


def _get_csv(url, tries=4):
    """Fetch a CSV; return list of rows (each is a list[str])."""
    for i in range(tries):
        try:
            r = session().get(url, timeout=120)
            if r.status_code == 200:
                # OFAC CSVs are latin-1 with embedded commas / quotes; csv handles it.
                text = r.content.decode("latin-1")
                return list(csv.reader(io.StringIO(text)))
            if r.status_code in (429, 500, 502, 503):
                time.sleep(2 ** i)
                continue
            return None
        except requests.RequestException:
            time.sleep(2 ** i)
    return None


def _to_int(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def _none_if_dash(v):
    """OFAC uses literal '-0-' for empty cells."""
    if v is None:
        return None
    s = str(v).strip()
    return None if s in ("", "-0-") else s


def refresh(conn):
    """Re-download the SDN trio and INSERT OR REPLACE every row."""
    sdn = _get_csv(f"{BASE}/sdn.csv")
    if sdn is None:
        print("sdn.csv: download failed")
        return 0
    n = 0
    for row in sdn:
        # Columns per OFAC's documented order (no header in the legacy CSV).
        if len(row) < 12:
            continue
        ent_num = _to_int(row[0])
        if ent_num is None:
            continue
        conn.execute(
            """INSERT OR REPLACE INTO ofac_sdn
                 (ent_num, sdn_name, sdn_type, program, title, call_sign,
                  vessel_type, tonnage, grt, vessel_flag, vessel_owner, remarks)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?)""",
            (
                ent_num,
                _none_if_dash(row[1]),
                _none_if_dash(row[2]),
                _none_if_dash(row[3]),
                _none_if_dash(row[4]),
                _none_if_dash(row[5]),
                _none_if_dash(row[6]),
                _none_if_dash(row[7]),
                _none_if_dash(row[8]),
                _none_if_dash(row[9]),
                _none_if_dash(row[10]),
                _none_if_dash(row[11]),
            ),
        )
        n += 1
        if n % 5000 == 0:
            print(f"...sdn {n} entities")
            conn.commit()
    conn.commit()
    print(f"sdn: stored {n} entities")

    alt = _get_csv(f"{BASE}/sdn_alt.csv") or []
    a = 0
    for row in alt:
        if len(row) < 5:
            continue
        ent_num = _to_int(row[0])
        alt_num = _to_int(row[1])
        if ent_num is None or alt_num is None:
            continue
        conn.execute(
            "INSERT OR REPLACE INTO ofac_alt (ent_num, alt_num, alt_type, alt_name, alt_remarks) VALUES (?,?,?,?,?)",
            (ent_num, alt_num, _none_if_dash(row[2]), _none_if_dash(row[3]), _none_if_dash(row[4])),
        )
        a += 1
        if a % 5000 == 0:
            print(f"...alt {a} aliases")
            conn.commit()
    conn.commit()
    print(f"alt: stored {a} aliases")

    add = _get_csv(f"{BASE}/sdn_add.csv") or []
    d = 0
    for row in add:
        if len(row) < 7:
            continue
        ent_num = _to_int(row[0])
        add_num = _to_int(row[1])
        if ent_num is None or add_num is None:
            continue
        conn.execute(
            """INSERT OR REPLACE INTO ofac_address
                 (ent_num, add_num, address, city_state_province, postal_code, country, add_remarks)
               VALUES (?,?,?,?,?,?,?)""",
            (
                ent_num, add_num,
                _none_if_dash(row[2]), _none_if_dash(row[3]),
                _none_if_dash(row[4]), _none_if_dash(row[5]),
                _none_if_dash(row[6]),
            ),
        )
        d += 1
        if d % 5000 == 0:
            print(f"...add {d} addresses")
            conn.commit()
    conn.commit()
    print(f"add: stored {d} addresses")
    return n


def stats(conn):
    n = conn.execute("SELECT COUNT(*) FROM ofac_sdn").fetchone()[0]
    a = conn.execute("SELECT COUNT(*) FROM ofac_alt").fetchone()[0]
    d = conn.execute("SELECT COUNT(*) FROM ofac_address").fetchone()[0]
    print(f"sdn entities  {n:>8}")
    print(f"aliases       {a:>8}")
    print(f"addresses     {d:>8}")
    print("\ntop programs:")
    for prog, cnt in conn.execute(
        "SELECT program, COUNT(*) FROM ofac_sdn WHERE program IS NOT NULL "
        "GROUP BY program ORDER BY 2 DESC LIMIT 10"
    ):
        print(f"  {cnt:>6}  {prog}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--refresh", action="store_true",
                    help="re-download the sdn.csv trio and INSERT OR REPLACE")
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
