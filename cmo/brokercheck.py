#!/usr/bin/env python3
"""FINRA BrokerCheck scanner: mirror real broker-dealer firms and the individuals
registered with them, straight from FINRA's public BrokerCheck API.

This is the provenance layer for the trading agents — every persona must resolve
to a real FINRA CRD (Central Registration Depository number), the same way a real
broker like CRD 6356563 does, never a fabricated name. All data here is public
under FINRA Rule 8312; nothing is synthesized.

Access patterns (validated):
  - firm by CRD:        /search/firm/{crd}            -> one firm or total=0
  - firm by name:       /search/firm?query=...        -> resolve name -> CRD
  - firm's roster:      /search/individual?firm={crd} -> paginate active brokers
  - individual detail:  /search/individual/{crd}      -> full registration history

There is no global match-all: individuals are only enumerable scoped to a firm,
so the universe is crawled as (walk firm CRDs) x (enumerate each firm's roster).

Usage:
    python3 -m cmo.brokercheck --resolve-underwriters
    python3 -m cmo.brokercheck --firm 19714
    python3 -m cmo.brokercheck --crawl-firms --from 1 --to 350000
    python3 -m cmo.brokercheck --roster 19714
    python3 -m cmo.brokercheck --roster-all
    python3 -m cmo.brokercheck --detail 6356563
    python3 -m cmo.brokercheck --stats
"""
import argparse, json, os, sqlite3, time
import requests

UA = "dsco-research arthurcolle@gmail.com"
BASE = "https://api.brokercheck.finra.org"
_session = None


def session():
    global _session
    if _session is None:
        _session = requests.Session()
        _session.headers.update({"User-Agent": UA})
    return _session


def ensure_bc(conn):
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "brokercheck.sql")) as f:
        conn.executescript(f.read())
    conn.commit()


def _get(url, params, tries=4):
    """GET with backoff; return parsed JSON or None. FINRA throttles, so be polite."""
    for i in range(tries):
        try:
            r = session().get(url, params=params, timeout=60)
            if r.status_code == 200:
                return r.json()
            if r.status_code in (403, 429, 500, 502, 503):
                time.sleep(2 ** i)
                continue
            return None
        except requests.RequestException:
            time.sleep(2 ** i)
    return None


# ---- firm endpoints -------------------------------------------------------

def firm_by_crd(crd):
    """Return a firm dict for a CRD, or None if no firm exists at that integer."""
    j = _get(f"{BASE}/search/firm/{crd}", {"query": "*", "nrows": 1, "wt": "json"})
    hits = (j or {}).get("hits") or {}
    if not hits.get("total"):
        return None
    src = hits["hits"][0]["_source"]
    content = src.get("content")
    if content:
        b = json.loads(content).get("basicInformation", {})
        return {"crd": b.get("firmId") or crd, "name": b.get("firmName"),
                "other_names": ";".join(b.get("firmOtherNames") or []),
                "sec_number": b.get("firmSecNumber") or b.get("firmBdSecNumber"),
                "bd_scope": b.get("bdScope"), "ia_scope": b.get("iaScope"),
                "branches": b.get("firmBranchOfficeCount")}
    return {"crd": src.get("firm_source_id") or crd, "name": src.get("firm_name"),
            "other_names": ";".join(src.get("firm_other_names") or []),
            "sec_number": src.get("firm_bd_sec_number"),
            "bd_scope": None, "ia_scope": None, "branches": None}


def firm_search(name):
    """Resolve a firm name to its best-match CRD record."""
    j = _get(f"{BASE}/search/firm",
             {"query": name, "hl": "true", "nrows": 5, "start": 0, "wt": "json"})
    hits = (j or {}).get("hits") or {}
    if not hits.get("total"):
        return None
    s = hits["hits"][0]["_source"]
    return {"crd": int(s["firm_source_id"]), "name": s.get("firm_name"),
            "other_names": ";".join(s.get("firm_other_names") or []),
            "sec_number": s.get("firm_bd_sec_number")}


def firm_roster(firm_crd, start, nrows):
    """One page of a firm's active individuals."""
    j = _get(f"{BASE}/search/individual",
             {"query": "", "filter": "active=true", "includePrevious": "false",
              "hl": "true", "nrows": nrows, "start": start,
              "firm": firm_crd, "wt": "json"})
    hits = (j or {}).get("hits") or {}
    total = hits.get("total") or 0
    out = []
    for h in hits.get("hits", []):
        s = h["_source"]
        out.append({"crd": int(s["ind_source_id"]),
                    "first_name": s.get("ind_firstname"),
                    "middle_name": s.get("ind_middlename"),
                    "last_name": s.get("ind_lastname")})
    return total, out


def broker_detail(crd):
    """Full record for one individual: basics + current/previous employments."""
    j = _get(f"{BASE}/search/individual/{crd}",
             {"query": "*", "hl": "true", "nrows": 1, "wt": "json"})
    hits = (j or {}).get("hits") or {}
    if not hits.get("total"):
        return None
    src = json.loads(hits["hits"][0]["_source"]["content"])
    bi = src.get("basicInformation", {})
    regs = []
    for e in src.get("currentEmployments", []) or []:
        regs.append((e.get("firmId"), e.get("firmName"),
                     e.get("registrationBeginDate"), None, 1))
    for e in src.get("previousEmployments", []) or []:
        regs.append((e.get("firmId"), e.get("firmName"),
                     e.get("registrationBeginDate"),
                     e.get("registrationEndDate"), 0))
    return {"crd": bi.get("individualId") or crd,
            "first_name": bi.get("firstName"),
            "middle_name": bi.get("middleName"),
            "last_name": bi.get("lastName"),
            "scope": bi.get("bcScope"), "regs": regs}


# ---- persistence ----------------------------------------------------------

def save_firm(conn, f, src):
    conn.execute(
        """INSERT INTO bc_firm(crd,name,other_names,sec_number,bd_scope,ia_scope,
                               branches,src)
           VALUES(?,?,?,?,?,?,?,?)
           ON CONFLICT(crd) DO UPDATE SET
             name=excluded.name, other_names=excluded.other_names,
             sec_number=excluded.sec_number, bd_scope=excluded.bd_scope,
             ia_scope=excluded.ia_scope, branches=excluded.branches""",
        (f["crd"], f.get("name"), f.get("other_names"), f.get("sec_number"),
         f.get("bd_scope"), f.get("ia_scope"), f.get("branches"), src))


def save_broker(conn, b):
    conn.execute(
        """INSERT INTO bc_broker(crd,first_name,middle_name,last_name,scope)
           VALUES(?,?,?,?,?)
           ON CONFLICT(crd) DO UPDATE SET
             first_name=excluded.first_name, middle_name=excluded.middle_name,
             last_name=excluded.last_name,
             scope=COALESCE(excluded.scope, bc_broker.scope)""",
        (b["crd"], b.get("first_name"), b.get("middle_name"),
         b.get("last_name"), b.get("scope")))


def save_reg(conn, broker_crd, firm_crd, firm_name, begin, end, current):
    conn.execute(
        """INSERT OR REPLACE INTO bc_registration
             (broker_crd,firm_crd,firm_name,begin_date,end_date,current)
           VALUES(?,?,?,?,?,?)""",
        (broker_crd, firm_crd or 0, firm_name, begin or "", end, current))


# ---- crawls ---------------------------------------------------------------

def resolve_underwriters(conn):
    """Map every firm in the deal DB's underwriter table to its FINRA CRD."""
    names = [r[0] for r in conn.execute(
        "SELECT DISTINCT name FROM underwriter ORDER BY name")]
    n = 0
    for name in names:
        f = firm_search(name)
        if f:
            save_firm(conn, f, "underwriter")
            n += 1
            print(f"  {name:<28} -> CRD {f['crd']}  {f['name']}")
        else:
            print(f"  {name:<28} -> (no match)")
        time.sleep(0.2)
    conn.commit()
    return n


def crawl_firms(conn, lo, hi, sleep):
    """Walk the firm CRD integer space, storing every CRD that resolves."""
    found = 0
    row = conn.execute("SELECT last_crd FROM bc_progress WHERE kind='firm_walk'").fetchone()
    if row and row[0] and row[0] >= lo:
        lo = row[0] + 1
        print(f"resuming firm walk at CRD {lo}")
    for crd in range(lo, hi + 1):
        f = firm_by_crd(crd)
        if f:
            save_firm(conn, f, "crawl")
            found += 1
            if found % 50 == 0:
                print(f"  ...{found} firms (at CRD {crd})")
        if crd % 200 == 0:
            conn.execute(
                """INSERT INTO bc_progress(kind,last_crd,ts)
                   VALUES('firm_walk',?,datetime('now'))
                   ON CONFLICT(kind) DO UPDATE SET last_crd=excluded.last_crd,
                     ts=excluded.ts""", (crd,))
            conn.commit()
        time.sleep(sleep)
    conn.commit()
    return found


def roster_firm(conn, firm_crd, sleep, page=100):
    """Enumerate one firm's active individuals and record their current reg."""
    fname_row = conn.execute("SELECT name FROM bc_firm WHERE crd=?", (firm_crd,)).fetchone()
    fname = fname_row[0] if fname_row else None
    start, total, n = 0, None, 0
    while True:
        t, rows = firm_roster(firm_crd, start, page)
        if total is None:
            total = t
        if not rows:
            break
        for b in rows:
            save_broker(conn, b)
            save_reg(conn, b["crd"], firm_crd, fname, None, None, 1)
            n += 1
        start += len(rows)
        conn.commit()
        if start >= total:
            break
        time.sleep(sleep)
    conn.execute("UPDATE bc_firm SET roster_total=?, rostered=1 WHERE crd=?",
                 (total or 0, firm_crd))
    conn.commit()
    return n, (total or 0)


def roster_all(conn, sleep, only_bd=True):
    """Pull rosters for every stored firm not yet rostered."""
    q = "SELECT crd,name FROM bc_firm WHERE rostered=0"
    if only_bd:
        q += " AND (bd_scope='ACTIVE' OR bd_scope IS NULL)"
    firms = conn.execute(q).fetchall()
    print(f"rostering {len(firms)} firms")
    grand = 0
    for crd, name in firms:
        n, total = roster_firm(conn, crd, sleep)
        grand += n
        print(f"  CRD {crd:<8} {name or '?':<40} {n}/{total}")
    return grand


def fetch_detail(conn, broker_crd):
    """Pull and store a broker's full registration history."""
    d = broker_detail(broker_crd)
    if not d:
        return None
    save_broker(conn, d)
    for firm_crd, firm_name, begin, end, current in d["regs"]:
        save_reg(conn, d["crd"], firm_crd, firm_name, begin, end, current)
    conn.execute("UPDATE bc_broker SET detailed=1 WHERE crd=?", (broker_crd,))
    conn.commit()
    return d


def stats(conn):
    f = conn.execute("SELECT COUNT(*) FROM bc_firm").fetchone()[0]
    fr = conn.execute("SELECT COUNT(*) FROM bc_firm WHERE rostered=1").fetchone()[0]
    b = conn.execute("SELECT COUNT(*) FROM bc_broker").fetchone()[0]
    r = conn.execute("SELECT COUNT(*) FROM bc_registration").fetchone()[0]
    print(f"firms        {f:>8}  ({fr} rostered)")
    print(f"brokers      {b:>8}")
    print(f"registrations{r:>8}")
    top = conn.execute(
        """SELECT f.name, f.roster_total FROM bc_firm f
           WHERE f.rostered=1 ORDER BY f.roster_total DESC LIMIT 10""").fetchall()
    if top:
        print("\ntop firms by roster:")
        for name, t in top:
            print(f"  {t:>7}  {name}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--resolve-underwriters", action="store_true")
    ap.add_argument("--firm", type=int, metavar="CRD")
    ap.add_argument("--firm-name", metavar="NAME")
    ap.add_argument("--crawl-firms", action="store_true")
    ap.add_argument("--from", dest="lo", type=int, default=1)
    ap.add_argument("--to", dest="hi", type=int, default=350000)
    ap.add_argument("--roster", type=int, metavar="CRD")
    ap.add_argument("--roster-all", action="store_true")
    ap.add_argument("--detail", type=int, metavar="CRD")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--sleep", type=float, default=0.15)
    args = ap.parse_args()

    conn = sqlite3.connect(args.db)
    ensure_bc(conn)

    if args.resolve_underwriters:
        n = resolve_underwriters(conn)
        print(f"resolved {n} firms")
    elif args.firm is not None:
        f = firm_by_crd(args.firm)
        if not f:
            print(f"no firm at CRD {args.firm}")
        else:
            save_firm(conn, f, "search"); conn.commit()
            print(json.dumps(f, indent=2))
    elif args.firm_name:
        f = firm_search(args.firm_name)
        if not f:
            print("no match")
        else:
            save_firm(conn, f, "search"); conn.commit()
            print(json.dumps(f, indent=2))
    elif args.crawl_firms:
        n = crawl_firms(conn, args.lo, args.hi, args.sleep)
        print(f"found {n} firms in CRD [{args.lo},{args.hi}]")
    elif args.roster is not None:
        if not conn.execute("SELECT 1 FROM bc_firm WHERE crd=?", (args.roster,)).fetchone():
            f = firm_by_crd(args.roster)
            if f:
                save_firm(conn, f, "search"); conn.commit()
        n, total = roster_firm(conn, args.roster, args.sleep)
        print(f"stored {n}/{total} brokers for firm CRD {args.roster}")
    elif args.roster_all:
        n = roster_all(conn, args.sleep)
        print(f"stored {n} broker rows")
    elif args.detail is not None:
        d = fetch_detail(conn, args.detail)
        print(json.dumps(d, indent=2) if d else "no record")
    elif args.stats:
        stats(conn)
    else:
        ap.error("pick an action; see --help")


if __name__ == "__main__":
    main()
