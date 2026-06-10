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
import argparse, json, os, sqlite3, threading, time
from concurrent.futures import ThreadPoolExecutor
import requests

UA = "dsco-research arthurcolle@gmail.com"
BASE = "https://api.brokercheck.finra.org"
_local = threading.local()


def session():
    s = getattr(_local, "session", None)
    if s is None:
        s = requests.Session()
        s.headers.update({"User-Agent": UA})
        _local.session = s
    return s


# Name search returns a best fuzzy match, which for big banks is often the wrong
# legal entity (a holding co, a fund, a foreign sub). Pin the flagship US
# broker-dealer CRD for each underwriter so rosters hit the real trading entity.
PINS = {
    "J.P. Morgan": 79,            # J.P. MORGAN SECURITIES LLC (not Prime Inc.)
    "Morgan Stanley": 8209,       # MORGAN STANLEY & CO. LLC (not the holdco)
    "Deutsche Bank": 2525,        # DEUTSCHE BANK SECURITIES INC. (not M&A Inc.)
    "UBS": 7654,                  # UBS SECURITIES LLC (not OConnor fund)
    "Nomura": 4297,               # NOMURA SECURITIES INTERNATIONAL, INC.
    "Cantor Fitzgerald": 134,     # CANTOR FITZGERALD & CO. (not the G.P.)
    "SMBC Nikko": 28602,          # SMBC NIKKO SECURITIES AMERICA (not Canada)
    "Wells Fargo Securities": 7665,  # WELLS FARGO SECURITIES, LLC (active BD)
}

# Prop / HFT / quant trading firms — registered broker-dealer CRDs (real).
PROP_FIRMS = {
    "Jump Trading": 106124, "Jane Street Capital": 103790,
    "Jane Street Markets": 104485, "DRW Securities": 45908,
    "Renaissance Technologies": 106661, "Citadel Securities": 116797,
    "Optiver US": 128030, "IMC": 106089, "Susquehanna (SIG)": 35874,
    "Flow Traders US": 150780, "XTX Markets": 289846, "Akuna Securities": 159041,
}


def _commit_retry(conn, tries=10):
    """Commit, retrying through transient WAL writer-lock contention so a roster
    can coexist with the discovery crawl writing the same DB."""
    for i in range(tries):
        try:
            conn.commit()
            return
        except sqlite3.OperationalError as e:
            if "locked" in str(e) or "busy" in str(e):
                time.sleep(0.5 * (i + 1))
                continue
            raise
    conn.commit()


_BROKER_COLS = [
    ("ia_scope", "TEXT"), ("other_names", "TEXT"), ("days_in_industry", "TEXT"),
    ("exams", "TEXT"), ("exam_count", "INTEGER"), ("registered_states", "TEXT"),
    ("registered_sros", "TEXT"), ("disclosure_flag", "TEXT"),
    ("num_disclosures", "INTEGER"), ("cur_firm_crd", "INTEGER"),
    ("cur_firm_name", "TEXT"), ("cur_city", "TEXT"), ("cur_state", "TEXT"),
]


def ensure_bc(conn):
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "brokercheck.sql")) as f:
        conn.executescript(f.read())
    # The live DB predates the detail columns; CREATE TABLE IF NOT EXISTS won't
    # add them, so backfill any missing column.
    have = {r[1] for r in conn.execute("PRAGMA table_info(bc_broker)")}
    for col, typ in _BROKER_COLS:
        if col not in have:
            conn.execute(f"ALTER TABLE bc_broker ADD COLUMN {col} {typ}")
    have_reg = {r[1] for r in conn.execute("PRAGMA table_info(bc_registration)")}
    for col in ("city", "state"):
        if col not in have_reg:
            conn.execute(f"ALTER TABLE bc_registration ADD COLUMN {col} TEXT")
    # Per-person products/specialties (exam name/scope/date) for cross-referencing.
    conn.execute("""CREATE TABLE IF NOT EXISTS bc_exam (
        broker_crd    INTEGER,
        exam_category TEXT,
        exam_name     TEXT,
        exam_scope    TEXT,
        taken_date    TEXT,
        PRIMARY KEY(broker_crd, exam_category)
    )""")
    conn.execute("CREATE INDEX IF NOT EXISTS idx_exam_cat ON bc_exam(exam_category)")
    conn.execute("CREATE INDEX IF NOT EXISTS idx_exam_name ON bc_exam(exam_name)")
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
                "bd_scope": b.get("bcScope"), "ia_scope": b.get("iaScope"),
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


def _branch(e):
    """First branch office (city, state) for an employment, if any."""
    locs = e.get("branchOfficeLocations") or []
    if locs:
        b = locs[0]
        return b.get("city"), b.get("state")
    return None, None


def _exam_cats(src):
    """Return the rich per-exam records (products/specialties) a person holds.
    Each: (category, name, scope, taken_date). De-duped by category."""
    seen, out = set(), []
    for key in ("stateExamCategory", "principalExamCategory", "productExamCategory"):
        for x in src.get(key) or []:
            c = x.get("examCategory")
            if not c or c in seen:
                continue
            seen.add(c)
            out.append((c, x.get("examName"), x.get("examScope"),
                        x.get("examTakenDate")))
    return out


def broker_detail(crd):
    """Full individual record: identity, scopes, exams, states, SROs,
    disclosures, and the complete BD + IA employment history."""
    j = _get(f"{BASE}/search/individual/{crd}",
             {"query": "*", "hl": "true", "nrows": 1, "wt": "json"})
    hits = (j or {}).get("hits") or {}
    if not hits.get("total"):
        return None
    src = json.loads(hits["hits"][0]["_source"]["content"])
    bi = src.get("basicInformation", {})
    regs = []
    cur_firm_crd = cur_firm_name = cur_city = cur_state = None
    for e in (src.get("currentEmployments") or []) + (src.get("currentIAEmployments") or []):
        city, state = _branch(e)
        regs.append((e.get("firmId"), e.get("firmName"),
                     e.get("registrationBeginDate"), None, 1, city, state))
        if cur_firm_crd is None:
            cur_firm_crd, cur_firm_name = e.get("firmId"), e.get("firmName")
            cur_city, cur_state = city, state
    for e in (src.get("previousEmployments") or []) + (src.get("previousIAEmployments") or []):
        city, state = _branch(e)
        regs.append((e.get("firmId"), e.get("firmName"),
                     e.get("registrationBeginDate"),
                     e.get("registrationEndDate"), 0, city, state))
    states = [s.get("state") if isinstance(s, dict) else s
              for s in (src.get("registeredStates") or [])]
    sros = [s.get("sro") if isinstance(s, dict) else s
            for s in (src.get("registeredSROs") or [])]
    discs = src.get("disclosures") or []
    exams = _exam_cats(src)
    exam_codes = [e[0] for e in exams]
    return {"crd": bi.get("individualId") or crd,
            "first_name": bi.get("firstName"),
            "middle_name": bi.get("middleName"),
            "last_name": bi.get("lastName"),
            "scope": bi.get("bcScope"), "ia_scope": bi.get("iaScope"),
            "other_names": ";".join([n for n in (bi.get("otherNames") or []) if n]),
            "days_in_industry": bi.get("daysInIndustryCalculatedDate"),
            "exams": ";".join(c for c in exam_codes if c),
            "exam_count": len(exam_codes), "exam_cats": exams,
            "registered_states": ";".join(s for s in states if s),
            "registered_sros": ";".join(s for s in sros if s),
            "disclosure_flag": src.get("disclosureFlag"),
            "num_disclosures": len(discs),
            "cur_firm_crd": cur_firm_crd, "cur_firm_name": cur_firm_name,
            "cur_city": cur_city, "cur_state": cur_state,
            "regs": regs, "disclosures": discs}


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


def save_broker_detail(conn, b):
    """Persist the full detail record (identity + scopes + exams + disclosures)."""
    conn.execute(
        """INSERT INTO bc_broker
             (crd,first_name,middle_name,last_name,scope,ia_scope,other_names,
              days_in_industry,exams,exam_count,registered_states,registered_sros,
              disclosure_flag,num_disclosures,cur_firm_crd,cur_firm_name,
              cur_city,cur_state,detailed)
           VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1)
           ON CONFLICT(crd) DO UPDATE SET
             first_name=excluded.first_name, middle_name=excluded.middle_name,
             last_name=excluded.last_name, scope=excluded.scope,
             ia_scope=excluded.ia_scope, other_names=excluded.other_names,
             days_in_industry=excluded.days_in_industry, exams=excluded.exams,
             exam_count=excluded.exam_count,
             registered_states=excluded.registered_states,
             registered_sros=excluded.registered_sros,
             disclosure_flag=excluded.disclosure_flag,
             num_disclosures=excluded.num_disclosures,
             cur_firm_crd=excluded.cur_firm_crd,
             cur_firm_name=excluded.cur_firm_name, cur_city=excluded.cur_city,
             cur_state=excluded.cur_state, detailed=1""",
        (b["crd"], b.get("first_name"), b.get("middle_name"), b.get("last_name"),
         b.get("scope"), b.get("ia_scope"), b.get("other_names"),
         b.get("days_in_industry"), b.get("exams"), b.get("exam_count"),
         b.get("registered_states"), b.get("registered_sros"),
         b.get("disclosure_flag"), b.get("num_disclosures"),
         b.get("cur_firm_crd"), b.get("cur_firm_name"),
         b.get("cur_city"), b.get("cur_state")))


def save_reg(conn, broker_crd, firm_crd, firm_name, begin, end, current,
             city=None, state=None):
    conn.execute(
        """INSERT OR REPLACE INTO bc_registration
             (broker_crd,firm_crd,firm_name,begin_date,end_date,current,city,state)
           VALUES(?,?,?,?,?,?,?,?)""",
        (broker_crd, firm_crd or 0, firm_name, begin or "", end, current,
         city, state))


# ---- crawls ---------------------------------------------------------------

def resolve_underwriters(conn):
    """Map every firm in the deal DB's underwriter table to its FINRA CRD."""
    names = [r[0] for r in conn.execute(
        "SELECT DISTINCT name FROM underwriter ORDER BY name")]
    n = 0
    for name in names:
        if name in PINS:
            f = firm_by_crd(PINS[name])         # authoritative pinned entity
        else:
            f = firm_search(name)
        if f:
            save_firm(conn, f, "underwriter")
            n += 1
            tag = " (pinned)" if name in PINS else ""
            print(f"  {name:<28} -> CRD {f['crd']}  {f['name']}{tag}")
        else:
            print(f"  {name:<28} -> (no match)")
        time.sleep(0.2)
    conn.commit()
    return n


def seed_prop_firms(conn):
    """Add the prop/HFT/quant trading firms by pinned BD CRD."""
    n = 0
    for name, crd in PROP_FIRMS.items():
        f = firm_by_crd(crd)
        if f:
            save_firm(conn, f, "search")
            n += 1
            print(f"  {name:<26} -> CRD {crd}  {f['name']}")
        else:
            print(f"  {name:<26} -> CRD {crd}  (not found)")
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


def crawl_firms_parallel(conn, lo, hi, workers):
    """Walk the firm CRD space with a thread pool: workers fetch concurrently,
    the main thread is the only DB writer. Resumable via bc_progress."""
    row = conn.execute("SELECT last_crd FROM bc_progress WHERE kind='firm_walk'").fetchone()
    if row and row[0] and row[0] >= lo:
        lo = row[0] + 1
        print(f"resuming firm walk at CRD {lo}")
    found = 0
    crds = range(lo, hi + 1)
    with ThreadPoolExecutor(max_workers=workers) as ex:
        last = lo
        for crd, f in zip(crds, ex.map(firm_by_crd, crds, chunksize=64)):
            last = crd
            if f:
                save_firm(conn, f, "crawl")
                found += 1
                if found % 100 == 0:
                    print(f"  ...{found} firms (at CRD {crd})", flush=True)
            if crd % 500 == 0:
                conn.execute(
                    """INSERT INTO bc_progress(kind,last_crd,ts)
                       VALUES('firm_walk',?,datetime('now'))
                       ON CONFLICT(kind) DO UPDATE SET last_crd=excluded.last_crd,
                         ts=excluded.ts""", (crd,))
                conn.commit()
        conn.execute(
            """INSERT INTO bc_progress(kind,last_crd,ts)
               VALUES('firm_walk',?,datetime('now'))
               ON CONFLICT(kind) DO UPDATE SET last_crd=excluded.last_crd,
                 ts=excluded.ts""", (last,))
        conn.commit()
    return found


def crawl_individuals_parallel(conn, lo, hi, workers):
    """Walk the INDIVIDUAL CRD integer space directly — the definitive 'all
    brokers' crawl. Unlike rostering (active individuals at active firms only),
    this resolves every CRD via the individual-detail endpoint, so it captures
    inactive/former reps and people whose only registrations were at now-defunct
    firms. Each hit is stored with full detail (identity, scopes, exams,
    registration history, disclosures). Resumable via bc_progress 'indiv_walk'."""
    row = conn.execute(
        "SELECT last_crd FROM bc_progress WHERE kind='indiv_walk'").fetchone()
    if row and row[0] and row[0] >= lo:
        lo = row[0] + 1
        print(f"resuming individual walk at CRD {lo}", flush=True)
    found = 0

    def _mark(crd):
        conn.execute(
            """INSERT INTO bc_progress(kind,last_crd,ts)
               VALUES('indiv_walk',?,datetime('now'))
               ON CONFLICT(kind) DO UPDATE SET last_crd=excluded.last_crd,
                 ts=excluded.ts""", (crd,))
        conn.commit()

    # Process in bounded windows: ThreadPoolExecutor.map eagerly submits its
    # whole input, so mapping all 7.5M CRDs at once builds millions of futures
    # and never yields. A window keeps the in-flight set small and lets progress
    # (and resumability) advance steadily.
    WINDOW = 20000
    with ThreadPoolExecutor(max_workers=workers) as ex:
        for base in range(lo, hi + 1, WINDOW):
            crds = range(base, min(base + WINDOW, hi + 1))
            last = base
            for crd, d in zip(crds, ex.map(broker_detail, crds, chunksize=32)):
                last = crd
                if not d:
                    continue
                for attempt in range(12):
                    try:
                        _store_detail(conn, d)
                        _commit_retry(conn)
                        break
                    except sqlite3.OperationalError as e:
                        if "locked" in str(e) or "busy" in str(e):
                            conn.rollback(); time.sleep(0.5 * (attempt + 1)); continue
                        raise
                found += 1
                if found % 200 == 0:
                    print(f"  ...{found} individuals (at CRD {crd})", flush=True)
            _mark(last)
            print(f"  window done @ CRD {last}  (found {found} so far)", flush=True)
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
        q += " AND bd_scope='ACTIVE'"
    firms = conn.execute(q).fetchall()
    print(f"rostering {len(firms)} firms")
    grand = 0
    for crd, name in firms:
        n, total = roster_firm(conn, crd, sleep)
        grand += n
        print(f"  CRD {crd:<8} {name or '?':<40} {n}/{total}")
    return grand


def _fetch_full_roster(firm_crd, page=100):
    """Worker: pull every active individual for a firm (all pages). No DB."""
    start, total, rows = 0, None, []
    while True:
        t, page_rows = firm_roster(firm_crd, start, page)
        if total is None:
            total = t
        if not page_rows:
            break
        rows.extend(page_rows)
        start += len(page_rows)
        if start >= (total or 0):
            break
    return firm_crd, (total or 0), rows


def roster_all_parallel(conn, workers, only_bd=True):
    """Roster every not-yet-rostered firm; workers fetch rosters concurrently,
    the main thread writes."""
    q = "SELECT crd,name FROM bc_firm WHERE rostered=0"
    if only_bd:
        q += " AND bd_scope='ACTIVE'"
    firms = conn.execute(q).fetchall()
    names = {crd: name for crd, name in firms}
    print(f"rostering {len(firms)} firms with {workers} workers", flush=True)
    grand = 0
    with ThreadPoolExecutor(max_workers=workers) as ex:
        for firm_crd, total, rows in ex.map(_fetch_full_roster, list(names)):
            fname = names.get(firm_crd)
            for attempt in range(12):
                try:
                    for b in rows:
                        save_broker(conn, b)
                        save_reg(conn, b["crd"], firm_crd, fname, None, None, 1)
                    conn.execute(
                        "UPDATE bc_firm SET roster_total=?, rostered=1 WHERE crd=?",
                        (total, firm_crd))
                    _commit_retry(conn)
                    break
                except sqlite3.OperationalError as e:
                    if "locked" in str(e) or "busy" in str(e):
                        conn.rollback()
                        time.sleep(0.5 * (attempt + 1))
                        continue
                    raise
            grand += len(rows)
            print(f"  CRD {firm_crd:<8} {fname or '?':<40} {len(rows)}/{total}", flush=True)
    return grand


def roster_loop(conn, workers, until_crd=0, idle_passes=3, idle_sleep=45):
    """Drain every firm to rostered=1, re-querying each pass so firms the
    discovery crawl adds mid-run get picked up too. While the crawl is still
    walking (firm_walk progress < until_crd) an empty pass just waits — the
    idle-exit only counts once discovery has reached its target."""
    total, idle = 0, 0
    while True:
        remaining = conn.execute(
            "SELECT COUNT(*) FROM bc_firm WHERE rostered=0 AND bd_scope='ACTIVE'"
        ).fetchone()[0]
        if remaining == 0:
            row = conn.execute(
                "SELECT last_crd FROM bc_progress WHERE kind='firm_walk'").fetchone()
            crawl_done = bool(row and row[0] and row[0] >= until_crd)
            if crawl_done:
                idle += 1
                print(f"  nothing left, crawl done (idle {idle}/{idle_passes})", flush=True)
                if idle >= idle_passes:
                    break
            else:
                at = row[0] if row else 0
                print(f"  caught up; waiting on crawl (at CRD {at}/{until_crd})", flush=True)
            time.sleep(idle_sleep)
            continue
        idle = 0
        print(f"== roster pass: {remaining} firms unrostered ==", flush=True)
        total += roster_all_parallel(conn, workers, only_bd=True)
    return total


def _store_detail(conn, d):
    save_broker_detail(conn, d)
    for firm_crd, firm_name, begin, end, current, city, state in d["regs"]:
        save_reg(conn, d["crd"], firm_crd, firm_name, begin, end, current,
                 city, state)
    if d.get("disclosures"):
        conn.execute("DELETE FROM bc_disclosure WHERE broker_crd=?", (d["crd"],))
        for dc in d["disclosures"]:
            conn.execute(
                """INSERT INTO bc_disclosure(broker_crd,type,event_date,resolution,detail)
                   VALUES(?,?,?,?,?)""",
                (d["crd"], dc.get("disclosureType"), dc.get("eventDate"),
                 dc.get("disclosureResolution"), json.dumps(dc)))
    if d.get("exam_cats"):
        conn.execute("DELETE FROM bc_exam WHERE broker_crd=?", (d["crd"],))
        for cat, name, scope, taken in d["exam_cats"]:
            conn.execute(
                """INSERT OR REPLACE INTO bc_exam(broker_crd,exam_category,
                       exam_name,exam_scope,taken_date) VALUES(?,?,?,?,?)""",
                (d["crd"], cat, name, scope, taken))


def fetch_detail(conn, broker_crd):
    """Pull and store one broker's full detail record."""
    d = broker_detail(broker_crd)
    if not d:
        return None
    _store_detail(conn, d)
    conn.commit()
    return d


def detail_all(conn, workers, limit=None):
    """Pull the full detail record for every broker not yet detailed. Workers
    fetch concurrently; the main thread writes. Resumable via bc_broker.detailed."""
    q = "SELECT crd FROM bc_broker WHERE detailed=0 OR detailed IS NULL"
    if limit:
        q += f" LIMIT {int(limit)}"
    crds = [r[0] for r in conn.execute(q)]
    print(f"detailing {len(crds)} brokers with {workers} workers", flush=True)
    done = 0
    with ThreadPoolExecutor(max_workers=workers) as ex:
        for d in ex.map(broker_detail, crds):
            if not d:
                continue
            for attempt in range(12):
                try:
                    _store_detail(conn, d)
                    _commit_retry(conn)
                    break
                except sqlite3.OperationalError as e:
                    if "locked" in str(e) or "busy" in str(e):
                        conn.rollback()
                        time.sleep(0.5 * (attempt + 1))
                        continue
                    raise
            done += 1
            if done % 500 == 0:
                print(f"  ...detailed {done}/{len(crds)}", flush=True)
    return done


def backfill_scope(conn, workers, only_missing=True):
    """Re-fetch bd_scope (FINRA bcScope) for stored firms. The earlier parser
    read the wrong key (bdScope) so every firm landed with NULL bd_scope; this
    repopulates ACTIVE/INACTIVE so the roster filter and phantom purge work."""
    q = "SELECT crd FROM bc_firm"
    if only_missing:
        q += " WHERE bd_scope IS NULL OR bd_scope=''"
    crds = [r[0] for r in conn.execute(q)]
    print(f"backfilling scope for {len(crds)} firms with {workers} workers",
          flush=True)
    done = 0
    with ThreadPoolExecutor(max_workers=workers) as ex:
        for f in ex.map(firm_by_crd, crds):
            if not f:
                continue
            for attempt in range(12):
                try:
                    conn.execute(
                        "UPDATE bc_firm SET bd_scope=?, ia_scope=? WHERE crd=?",
                        (f.get("bd_scope"), f.get("ia_scope"), f["crd"]))
                    _commit_retry(conn)
                    break
                except sqlite3.OperationalError as e:
                    if "locked" in str(e) or "busy" in str(e):
                        conn.rollback(); time.sleep(0.5 * (attempt + 1)); continue
                    raise
            done += 1
            if done % 500 == 0:
                print(f"  ...scoped {done}/{len(crds)}", flush=True)
    return done


def purge_phantoms(conn):
    """Delete phantom roster edges. FINRA's &firm= filter silently returns a
    slice of the global active population for terminated/expelled firm CRDs, so
    rostering a defunct firm wrote a current=1, blank-begin_date edge for each.
    Real edges come from broker detail (populated begin_date). We drop only the
    roster-origin edges (begin_date='') that point at non-ACTIVE firms; detail
    history and edges to live firms are kept."""
    n = conn.execute(
        """DELETE FROM bc_registration
           WHERE begin_date=''
             AND firm_crd IN (SELECT crd FROM bc_firm
                              WHERE bd_scope IS NOT NULL AND bd_scope!='ACTIVE')"""
    ).rowcount
    conn.execute(
        """UPDATE bc_firm SET roster_total=0
           WHERE bd_scope IS NOT NULL AND bd_scope!='ACTIVE'""")
    _commit_retry(conn)
    print(f"purged {n} phantom registration edges")
    return n


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
    ap.add_argument("--seed-prop", action="store_true")
    ap.add_argument("--firm", type=int, metavar="CRD")
    ap.add_argument("--firm-name", metavar="NAME")
    ap.add_argument("--crawl-firms", action="store_true")
    ap.add_argument("--crawl-individuals", action="store_true",
                    help="walk the individual CRD space for every registered "
                         "human (defaults --to 7500000)")
    ap.add_argument("--from", dest="lo", type=int, default=1)
    ap.add_argument("--to", dest="hi", type=int, default=350000)
    ap.add_argument("--roster", type=int, metavar="CRD")
    ap.add_argument("--roster-all", action="store_true")
    ap.add_argument("--roster-loop", action="store_true",
                    help="drain all firms to rostered, re-querying each pass")
    ap.add_argument("--detail", type=int, metavar="CRD")
    ap.add_argument("--detail-all", action="store_true",
                    help="pull full detail for every not-yet-detailed broker")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--backfill-scope", action="store_true",
                    help="re-fetch bd_scope (bcScope) for firms missing it")
    ap.add_argument("--rescope-all", action="store_true",
                    help="re-fetch bd_scope for every firm, not just missing")
    ap.add_argument("--purge-phantoms", action="store_true",
                    help="delete phantom roster edges to non-active firms")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--sleep", type=float, default=0.15)
    ap.add_argument("--workers", type=int, default=1)
    args = ap.parse_args()

    conn = sqlite3.connect(args.db, timeout=60)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=120000")
    ensure_bc(conn)

    if args.resolve_underwriters:
        n = resolve_underwriters(conn)
        print(f"resolved {n} firms")
    elif args.seed_prop:
        n = seed_prop_firms(conn)
        print(f"seeded {n} prop firms")
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
        if args.workers > 1:
            n = crawl_firms_parallel(conn, args.lo, args.hi, args.workers)
        else:
            n = crawl_firms(conn, args.lo, args.hi, args.sleep)
        print(f"found {n} firms in CRD [{args.lo},{args.hi}]")
    elif args.crawl_individuals:
        hi = 7500000 if args.hi == 350000 else args.hi
        n = crawl_individuals_parallel(conn, args.lo, hi, max(args.workers, 2))
        print(f"found {n} individuals in CRD [{args.lo},{hi}]")
    elif args.roster is not None:
        if not conn.execute("SELECT 1 FROM bc_firm WHERE crd=?", (args.roster,)).fetchone():
            f = firm_by_crd(args.roster)
            if f:
                save_firm(conn, f, "search"); conn.commit()
        n, total = roster_firm(conn, args.roster, args.sleep)
        print(f"stored {n}/{total} brokers for firm CRD {args.roster}")
    elif args.roster_all:
        if args.workers > 1:
            n = roster_all_parallel(conn, args.workers)
        else:
            n = roster_all(conn, args.sleep)
        print(f"stored {n} broker rows")
    elif args.roster_loop:
        n = roster_loop(conn, max(args.workers, 2), until_crd=args.hi)
        print(f"roster-loop done; stored {n} broker rows")
    elif args.detail is not None:
        d = fetch_detail(conn, args.detail)
        print(json.dumps(d, indent=2) if d else "no record")
    elif args.detail_all:
        n = detail_all(conn, max(args.workers, 2), args.limit)
        print(f"detailed {n} brokers")
    elif args.backfill_scope or args.rescope_all:
        n = backfill_scope(conn, max(args.workers, 2),
                           only_missing=not args.rescope_all)
        print(f"scoped {n} firms")
    elif args.purge_phantoms:
        purge_phantoms(conn)
    elif args.stats:
        stats(conn)
    else:
        ap.error("pick an action; see --help")


if __name__ == "__main__":
    main()
