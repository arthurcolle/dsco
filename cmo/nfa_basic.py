#!/usr/bin/env python3
"""NFA BASIC (CFTC futures registrants) mirror.

Mirrors NFA's public BASIC registry — every firm and individual registered
under the Commodity Exchange Act with the National Futures Association: FCMs,
IBs, CPOs, CTAs, RFEDs, swap dealers, and the APs / principals associated
with them. This is the futures-side counterpart to the FINRA broker-dealer
corpus (cmo.brokercheck). Public-record basis: CEA § 17 / CFTC regulations
making NFA registration and disciplinary status statutorily public.

Source: https://www.nfa.futures.org/basicnet/ (the public BASIC search UI).
NFA does not currently expose a clean public bulk-download API; per-firm and
per-individual lookups are accessible by NFA ID. For a full sweep, the
operator needs to either (a) drive the public search to enumerate the
universe or (b) request NFA bulk-membership access. This module supports
both per-ID lookup verbs and a --crawl-firms placeholder that the operator
fills in once bulk access is decided.

Usage:
    python3 -m cmo.nfa_basic --crawl-firms
    python3 -m cmo.nfa_basic --firm 0123456
    python3 -m cmo.nfa_basic --roster 0123456
    python3 -m cmo.nfa_basic --detail 0987654
    python3 -m cmo.nfa_basic --stats
"""
import argparse
import os
import sqlite3
import threading
import time

import requests

UA = "dsco-research arthurcolle@gmail.com"
BASE = "https://www.nfa.futures.org/basicnet"
SQL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "nfa_basic.sql")
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


def _get(url, params=None, tries=4):
    for i in range(tries):
        try:
            r = session().get(url, params=params or {}, timeout=60)
            if r.status_code == 200:
                return r
            if r.status_code in (429, 500, 502, 503):
                time.sleep(2 ** i)
                continue
            return None
        except requests.RequestException:
            time.sleep(2 ** i)
    return None


def fetch_firm(nfa_id):
    """One firm record by NFA ID. Returns dict or None.

    The BASIC firm page lives at /basicnet/FirmDetails.aspx?nfa-id={id};
    this stub returns None until the operator wires up the HTML parse or
    swaps to bulk access.
    """
    r = _get(f"{BASE}/FirmDetails.aspx", {"nfa-id": nfa_id})
    if r is None:
        return None
    # HTML parse intentionally deferred — see module docstring.
    return None


def fetch_individual(nfa_id):
    """One individual by NFA ID. Returns dict or None. See fetch_firm note."""
    r = _get(f"{BASE}/IndividualDetails.aspx", {"nfa-id": nfa_id})
    if r is None:
        return None
    return None


def save_firm(conn, f):
    if not f:
        return
    conn.execute(
        """INSERT OR REPLACE INTO nfa_firm
             (nfa_id, name, registration_type, status,
              address_city, address_state, registered_categories)
           VALUES (?,?,?,?,?,?,?)""",
        (
            f["nfa_id"], f.get("name"), f.get("registration_type"),
            f.get("status"), f.get("address_city"), f.get("address_state"),
            f.get("registered_categories"),
        ),
    )


def save_individual(conn, p):
    if not p:
        return
    conn.execute(
        """INSERT OR REPLACE INTO nfa_individual
             (nfa_id, name, status, primary_firm_nfa_id,
              registered_categories, disciplinary_actions)
           VALUES (?,?,?,?,?,?)""",
        (
            p["nfa_id"], p.get("name"), p.get("status"),
            p.get("primary_firm_nfa_id"), p.get("registered_categories"),
            p.get("disciplinary_actions"),
        ),
    )
    for reg in p.get("registrations", []):
        conn.execute(
            """INSERT OR REPLACE INTO nfa_registration
                 (individual_nfa_id, firm_nfa_id, begin_date, end_date, current)
               VALUES (?,?,?,?,?)""",
            (
                p["nfa_id"], reg["firm_nfa_id"], reg["begin_date"],
                reg.get("end_date"), 1 if reg.get("current") else 0,
            ),
        )


def crawl_firms(conn):
    """Enumerate all registered firms. Stub: see module docstring."""
    _ = conn
    print("crawl-firms: stub — operator needs to choose bulk path "
          "(public search drive or NFA-granted bulk access). "
          "Use --firm <nfa_id> for per-ID lookups in the meantime.")
    return 0


def roster_firm(conn, firm_nfa_id):
    """Pull every individual registered with one firm. Stub for now."""
    _ = conn
    _ = firm_nfa_id
    print(f"roster: stub — needs HTML parse of FirmRoster.aspx?nfa-id={firm_nfa_id}")
    return 0


def stats(conn):
    f = conn.execute("SELECT COUNT(*) FROM nfa_firm").fetchone()[0]
    p = conn.execute("SELECT COUNT(*) FROM nfa_individual").fetchone()[0]
    r = conn.execute("SELECT COUNT(*) FROM nfa_registration").fetchone()[0]
    print(f"firms         {f:>8}")
    print(f"individuals   {p:>8}")
    print(f"registrations {r:>8}")
    if f:
        print("\nby registration type:")
        for rt, cnt in conn.execute(
            "SELECT registration_type, COUNT(*) FROM nfa_firm "
            "WHERE registration_type IS NOT NULL GROUP BY 1 ORDER BY 2 DESC"
        ):
            print(f"  {cnt:>5}  {rt}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--crawl-firms", action="store_true")
    ap.add_argument("--firm", type=int, metavar="NFA_ID")
    ap.add_argument("--roster", type=int, metavar="FIRM_NFA_ID")
    ap.add_argument("--detail", type=int, metavar="INDIVIDUAL_NFA_ID")
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    conn = sqlite3.connect(args.db, timeout=60)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=120000")
    ensure(conn)

    if args.crawl_firms:
        crawl_firms(conn)
    elif args.firm is not None:
        f = fetch_firm(args.firm)
        if not f:
            print(f"no firm at NFA ID {args.firm}")
        else:
            save_firm(conn, f)
            conn.commit()
            print(f)
    elif args.roster is not None:
        n = roster_firm(conn, args.roster)
        conn.commit()
        print(f"stored {n} individuals for firm NFA ID {args.roster}")
    elif args.detail is not None:
        p = fetch_individual(args.detail)
        if not p:
            print(f"no individual at NFA ID {args.detail}")
        else:
            save_individual(conn, p)
            conn.commit()
            print(p)
    elif args.stats:
        stats(conn)
    else:
        ap.error("pick an action; see --help")


if __name__ == "__main__":
    main()
