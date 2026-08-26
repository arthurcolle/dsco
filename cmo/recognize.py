#!/usr/bin/env python3
"""Person recognition engine — fires at contact time.

Given any combination of contact signals, fans out across all CMO shards in
parallel, scores matches, and returns a confidence-tiered unified person record.

Confidence tiers:
  CONFIRMED  — unique external ID match (NPI, CRD, OFAC, arXiv ID)
  PROBABLE   — name + corroborating field (state, credential, employer)
  POSSIBLE   — name-only match, multiple candidates
  UNKNOWN    — no match found

Usage (CLI):
    python3 -m cmo.recognize --name "Jane Smith" --state NY
    python3 -m cmo.recognize --npi 1234567890
    python3 -m cmo.recognize --crd 1234567
    python3 -m cmo.recognize --email jane@example.com
    python3 -m cmo.recognize --github janesmith
    python3 -m cmo.recognize --name "Jane Smith" --company "Goldman Sachs"

Usage (library):
    from cmo.recognize import recognize
    result = recognize(name="Jane Smith", state="NY", company="Goldman Sachs")
    print(result["tier"], result["matches"])

Session integration:
    Called at session start when any user identity signal is present.
    Result is attached to session context and passed to the C runtime.
"""

import argparse
import json
import os
import re
import sqlite3
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field, asdict
from typing import Any

# ── Paths ────────────────────────────────────────────────────────────────────

_HERE     = os.path.dirname(__file__)
_ROOT     = os.path.dirname(_HERE)
_CMO_DB   = os.path.join(_ROOT, "data", "cmo", "cmo.db")
_NPI_DB   = os.path.join(_ROOT, "data", "people", "npi.db")
_OFAC_DB  = os.path.join(_ROOT, "data", "people", "ofac.db")
_OIG_DB   = os.path.join(_ROOT, "data", "people", "oig_leie.db")
_PCAOB_DB = os.path.join(_ROOT, "data", "people", "pcaob.db")
_ARXIV_DB = os.path.join(_ROOT, "data", "people", "arxiv.db")

# ── Scoring weights ──────────────────────────────────────────────────────────

W_EXACT_ID      = 1.00   # NPI / CRD / unique external ID — certainty
W_NAME_FULL     = 0.50   # exact first+last match
W_NAME_NORM     = 0.35   # normalized (no middle, lowercase)
W_STATE         = 0.20   # state corroboration
W_EMPLOYER      = 0.25   # employer / firm corroboration
W_CREDENTIAL    = 0.15   # credential / specialty corroboration
W_EMAIL_DOMAIN  = 0.30   # employer domain matches email domain
W_GITHUB        = 0.40   # GitHub username direct lookup (future shard)

TIER_CONFIRMED  = "CONFIRMED"
TIER_PROBABLE   = "PROBABLE"
TIER_POSSIBLE   = "POSSIBLE"
TIER_UNKNOWN    = "UNKNOWN"

# ── Data types ───────────────────────────────────────────────────────────────

@dataclass
class Match:
    shard:      str           # e.g. "FINRA/BrokerCheck"
    source_id:  str           # the shard's native key (CRD, NPI, …)
    first_name: str = ""
    last_name:  str = ""
    middle:     str = ""
    state:      str = ""
    employer:   str = ""
    credential: str = ""
    domain:     str = ""      # specialty / domain descriptor
    score:      float = 0.0
    signals:    list = field(default_factory=list)  # which signals matched
    raw:        dict = field(default_factory=dict)  # full row from DB

@dataclass
class RecognitionResult:
    tier:           str
    score:          float
    query:          dict
    matches:        list      # list[Match]
    elapsed_ms:     float
    shards_searched: list

# ── Normalization helpers ─────────────────────────────────────────────────────

def _norm(s: str) -> str:
    """Lowercase, strip accents-ish, collapse whitespace."""
    if not s:
        return ""
    return re.sub(r"\s+", " ", s.strip().lower())

def _name_tokens(name: str):
    """Return (first, last) normalized tokens from a full name string."""
    parts = _norm(name).split()
    if len(parts) >= 2:
        return parts[0], parts[-1]
    if parts:
        return "", parts[0]
    return "", ""

def _email_domain(email: str) -> str:
    if email and "@" in email:
        return email.split("@", 1)[1].lower()
    return ""

def _open(path: str):
    """Open SQLite read-only; return None if file missing."""
    if not os.path.exists(path):
        return None
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row
    return con

# ── Shard searchers ───────────────────────────────────────────────────────────

def _search_finra(q: dict) -> list:
    """FINRA BrokerCheck — bc_broker table."""
    con = _open(_CMO_DB)
    if con is None:
        return []
    matches = []
    try:
        cur = con.cursor()

        # 1. Exact CRD lookup
        if q.get("crd"):
            rows = cur.execute(
                "SELECT * FROM bc_broker WHERE individual_id = ?", (q["crd"],)
            ).fetchall()
            for r in rows:
                m = _bc_row_to_match(r)
                m.score += W_EXACT_ID
                m.signals.append("crd_exact")
                matches.append(m)

        # 2. Name search
        if q.get("first") and q.get("last") and not matches:
            rows = cur.execute(
                """SELECT * FROM bc_broker
                   WHERE lower(first_name)=? AND lower(last_name)=?""",
                (q["first"], q["last"])
            ).fetchall()
            for r in rows:
                m = _bc_row_to_match(r)
                m.score += W_NAME_FULL
                m.signals.append("name_full")
                # Corroborate state
                if q.get("state") and _norm(r["state_of_residence"]) == _norm(q["state"]):
                    m.score += W_STATE
                    m.signals.append("state")
                # Corroborate employer
                if q.get("company"):
                    emp = _norm(q["company"])
                    # Check bc_registration for firm name
                    firms = cur.execute(
                        """SELECT f.name FROM bc_registration reg
                           JOIN bc_firm f ON f.crd = reg.firm_crd
                           WHERE reg.individual_id = ?""",
                        (r["individual_id"],)
                    ).fetchall()
                    for firm in firms:
                        if emp in _norm(firm["name"]):
                            m.score += W_EMPLOYER
                            m.signals.append("employer")
                            break
                matches.append(m)

        # 3. Last-name only fallback (if no first name given)
        if q.get("last") and not q.get("first") and not matches:
            rows = cur.execute(
                "SELECT * FROM bc_broker WHERE lower(last_name)=? LIMIT 50",
                (q["last"],)
            ).fetchall()
            for r in rows:
                m = _bc_row_to_match(r)
                m.score += W_NAME_NORM
                m.signals.append("name_last")
                matches.append(m)

    finally:
        con.close()
    return matches

def _bc_row_to_match(r) -> Match:
    return Match(
        shard="FINRA/BrokerCheck",
        source_id=str(r["individual_id"]),
        first_name=r["first_name"] or "",
        last_name=r["last_name"] or "",
        middle=r["middle_name"] or "",
        state=r["state_of_residence"] or "",
        raw=dict(r),
    )

def _search_npi(q: dict) -> list:
    """CMS NPPES — npi_person table."""
    con = _open(_NPI_DB) or _open(_CMO_DB)
    if con is None:
        return []
    matches = []
    try:
        cur = con.cursor()

        # Check table exists
        tables = {r[0] for r in cur.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        ).fetchall()}
        if "npi_person" not in tables:
            return []

        # 1. Exact NPI
        if q.get("npi"):
            rows = cur.execute(
                "SELECT * FROM npi_person WHERE npi=?", (q["npi"],)
            ).fetchall()
            for r in rows:
                m = _npi_row_to_match(r)
                m.score += W_EXACT_ID
                m.signals.append("npi_exact")
                matches.append(m)

        # 2. Name + state
        if q.get("first") and q.get("last") and not matches:
            sql = """SELECT * FROM npi_person
                     WHERE lower(first_name)=? AND lower(last_name)=?"""
            args = [q["first"], q["last"]]
            if q.get("state"):
                sql += " AND lower(state)=?"
                args.append(_norm(q["state"]))
            sql += " LIMIT 50"
            rows = cur.execute(sql, args).fetchall()
            for r in rows:
                m = _npi_row_to_match(r)
                m.score += W_NAME_FULL
                m.signals.append("name_full")
                if q.get("state"):
                    m.score += W_STATE
                    m.signals.append("state")
                if q.get("credential") and _norm(q["credential"]) in _norm(r["credential"] or ""):
                    m.score += W_CREDENTIAL
                    m.signals.append("credential")
                matches.append(m)

    finally:
        con.close()
    return matches

def _npi_row_to_match(r) -> Match:
    return Match(
        shard="NPI/NPPES",
        source_id=str(r["npi"]),
        first_name=r["first_name"] or "",
        last_name=r["last_name"] or "",
        middle=r["middle_name"] or "",
        state=r["state"] or "",
        credential=r["credential"] or "",
        raw=dict(r),
    )

def _search_ofac(q: dict) -> list:
    """OFAC SDN — sanctions cross-check."""
    con = _open(_CMO_DB)
    if con is None:
        return []
    matches = []
    try:
        cur = con.cursor()
        tables = {r[0] for r in cur.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        ).fetchall()}
        if "sdn_entry" not in tables and "ofac_sdn" not in tables:
            return []
        tbl = "sdn_entry" if "sdn_entry" in tables else "ofac_sdn"

        if q.get("last"):
            # OFAC stores names as "LAST, FIRST" or full name
            rows = cur.execute(
                f"SELECT * FROM {tbl} WHERE lower(name) LIKE ? AND type='individual' LIMIT 20",
                (f"%{q['last']}%",)
            ).fetchall()
            for r in rows:
                m = Match(
                    shard="OFAC/SDN",
                    source_id=str(r["sdn_uid"] if "sdn_uid" in r.keys() else r["rowid"]),
                    last_name=q.get("last", ""),
                    domain="SANCTIONS",
                    raw=dict(r),
                )
                m.score += W_NAME_NORM
                m.signals.append("name_sanctions")
                if q.get("first") and q["first"] in _norm(r["name"] or ""):
                    m.score += 0.15
                    m.signals.append("name_first_partial")
                matches.append(m)
    finally:
        con.close()
    return matches

def _search_oig(q: dict) -> list:
    """OIG LEIE — excluded healthcare providers."""
    con = _open(_CMO_DB)
    if con is None:
        return []
    matches = []
    try:
        cur = con.cursor()
        tables = {r[0] for r in cur.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        ).fetchall()}
        if "leie_individual" not in tables:
            return []
        if q.get("first") and q.get("last"):
            rows = cur.execute(
                """SELECT * FROM leie_individual
                   WHERE lower(lastname)=? AND lower(firstname)=? LIMIT 20""",
                (q["last"], q["first"])
            ).fetchall()
            for r in rows:
                m = Match(
                    shard="OIG/LEIE",
                    source_id=str(r["npi"] if "npi" in r.keys() else r["rowid"]),
                    first_name=r["firstname"] or "",
                    last_name=r["lastname"] or "",
                    state=r["state"] or "" if "state" in r.keys() else "",
                    domain="EXCLUDED",
                    raw=dict(r),
                )
                m.score += W_NAME_FULL
                m.signals.append("name_excluded")
                matches.append(m)
    finally:
        con.close()
    return matches

def _search_pcaob(q: dict) -> list:
    """PCAOB — registered auditors."""
    con = _open(_CMO_DB)
    if con is None:
        return []
    matches = []
    try:
        cur = con.cursor()
        tables = {r[0] for r in cur.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        ).fetchall()}
        if "pcaob_person" not in tables:
            return []
        if q.get("first") and q.get("last"):
            rows = cur.execute(
                """SELECT * FROM pcaob_person
                   WHERE lower(first_name)=? AND lower(last_name)=? LIMIT 20""",
                (q["first"], q["last"])
            ).fetchall()
            for r in rows:
                m = Match(
                    shard="PCAOB",
                    source_id=str(r["person_id"] if "person_id" in r.keys() else r["rowid"]),
                    first_name=r["first_name"] or "",
                    last_name=r["last_name"] or "",
                    employer=r["firm_name"] or "" if "firm_name" in r.keys() else "",
                    domain="AUDIT",
                    raw=dict(r),
                )
                m.score += W_NAME_FULL
                m.signals.append("name_full")
                if q.get("company") and _norm(q["company"]) in _norm(m.employer):
                    m.score += W_EMPLOYER
                    m.signals.append("employer")
                matches.append(m)
    finally:
        con.close()
    return matches

def _search_arxiv(q: dict) -> list:
    """arXiv researcher index."""
    con = _open(_CMO_DB)
    if con is None:
        return []
    matches = []
    try:
        cur = con.cursor()
        tables = {r[0] for r in cur.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        ).fetchall()}
        tbl = None
        for candidate in ("arxiv_author", "arxiv_person", "arxiv_researcher"):
            if candidate in tables:
                tbl = candidate
                break
        if not tbl:
            return []
        if q.get("last"):
            rows = cur.execute(
                f"SELECT * FROM {tbl} WHERE lower(name) LIKE ? LIMIT 30",
                (f"%{q['last']}%",)
            ).fetchall()
            for r in rows:
                m = Match(
                    shard="arXiv",
                    source_id=str(r["author_id"] if "author_id" in r.keys() else r["rowid"]),
                    domain="RESEARCH",
                    raw=dict(r),
                )
                name_col = r["name"] if "name" in r.keys() else ""
                parts = name_col.split() if name_col else []
                if parts:
                    m.first_name = parts[0]
                    m.last_name = parts[-1]
                m.score += W_NAME_NORM
                m.signals.append("name_partial")
                if q.get("first") and q["first"] in _norm(name_col):
                    m.score += 0.15
                    m.signals.append("name_first")
                matches.append(m)
    finally:
        con.close()
    return matches

def _search_nfa(q: dict) -> list:
    """NFA Basic — futures/derivatives registrants."""
    con = _open(_CMO_DB)
    if con is None:
        return []
    matches = []
    try:
        cur = con.cursor()
        tables = {r[0] for r in cur.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        ).fetchall()}
        if "nfa_individual" not in tables:
            return []
        if q.get("first") and q.get("last"):
            rows = cur.execute(
                """SELECT * FROM nfa_individual
                   WHERE lower(first_name)=? AND lower(last_name)=? LIMIT 20""",
                (q["first"], q["last"])
            ).fetchall()
            for r in rows:
                m = Match(
                    shard="NFA",
                    source_id=str(r["nfa_id"] if "nfa_id" in r.keys() else r["rowid"]),
                    first_name=r["first_name"] or "",
                    last_name=r["last_name"] or "",
                    state=r["state"] or "" if "state" in r.keys() else "",
                    domain="FUTURES",
                    raw=dict(r),
                )
                m.score += W_NAME_FULL
                m.signals.append("name_full")
                if q.get("state") and _norm(q["state"]) == _norm(m.state):
                    m.score += W_STATE
                    m.signals.append("state")
                matches.append(m)
    finally:
        con.close()
    return matches

# ── Shard registry ────────────────────────────────────────────────────────────

_SHARDS = [
    ("FINRA/BrokerCheck", _search_finra),
    ("NPI/NPPES",         _search_npi),
    ("OFAC/SDN",          _search_ofac),
    ("OIG/LEIE",          _search_oig),
    ("PCAOB",             _search_pcaob),
    ("arXiv",             _search_arxiv),
    ("NFA",               _search_nfa),
]

# ── Core recognition function ─────────────────────────────────────────────────

def recognize(
    name:       str  = "",
    first:      str  = "",
    last:       str  = "",
    email:      str  = "",
    phone:      str  = "",
    state:      str  = "",
    company:    str  = "",
    credential: str  = "",
    npi:        str  = "",
    crd:        str  = "",
    github:     str  = "",
    arxiv_id:   str  = "",
    limit:      int  = 10,
) -> dict:
    """
    Recognize a person from any combination of signals.
    Returns a RecognitionResult serialized as dict.
    """
    t0 = time.perf_counter()

    # ── Build normalized query ────────────────────────────────────────────────
    q_first, q_last = "", ""
    if name:
        q_first, q_last = _name_tokens(name)
    if first:
        q_first = _norm(first)
    if last:
        q_last = _norm(last)

    q = {
        "first":      q_first,
        "last":       q_last,
        "email":      _norm(email),
        "email_domain": _email_domain(email),
        "phone":      re.sub(r"\D", "", phone),
        "state":      _norm(state),
        "company":    _norm(company),
        "credential": _norm(credential),
        "npi":        npi.strip() if npi else "",
        "crd":        crd.strip() if crd else "",
        "github":     github.strip().lower() if github else "",
        "arxiv_id":   arxiv_id.strip() if arxiv_id else "",
    }

    # ── Fan out to shards in parallel ─────────────────────────────────────────
    all_matches = []
    shards_searched = []
    with ThreadPoolExecutor(max_workers=len(_SHARDS)) as pool:
        futures = {
            pool.submit(fn, q): name_
            for name_, fn in _SHARDS
        }
        for fut in as_completed(futures):
            shard_name = futures[fut]
            shards_searched.append(shard_name)
            try:
                results = fut.result(timeout=5)
                all_matches.extend(results)
            except Exception as e:
                pass  # shard unavailable — skip silently

    # ── Sort by score descending, cap ─────────────────────────────────────────
    all_matches.sort(key=lambda m: m.score, reverse=True)
    top = all_matches[:limit]

    # ── Determine tier ────────────────────────────────────────────────────────
    best_score = top[0].score if top else 0.0
    if best_score >= W_EXACT_ID:
        tier = TIER_CONFIRMED
    elif best_score >= 0.60:
        tier = TIER_PROBABLE
    elif best_score >= 0.30:
        tier = TIER_POSSIBLE
    else:
        tier = TIER_UNKNOWN

    elapsed_ms = (time.perf_counter() - t0) * 1000

    result = RecognitionResult(
        tier=tier,
        score=round(best_score, 3),
        query={k: v for k, v in q.items() if v},
        matches=[asdict(m) for m in top],
        elapsed_ms=round(elapsed_ms, 1),
        shards_searched=shards_searched,
    )
    return asdict(result)

# ── Session hook — called at runtime contact time ─────────────────────────────

def recognize_session(session_context: dict) -> dict:
    """
    Called at session start. Extracts signals from session_context,
    runs recognition, attaches result.

    session_context keys used:
        user_name, user_email, user_github, user_company,
        user_state, user_npi, user_crd
    """
    result = recognize(
        name=session_context.get("user_name", ""),
        email=session_context.get("user_email", ""),
        github=session_context.get("user_github", ""),
        company=session_context.get("user_company", ""),
        state=session_context.get("user_state", ""),
        npi=session_context.get("user_npi", ""),
        crd=session_context.get("user_crd", ""),
    )
    session_context["recognition"] = result
    return session_context

# ── CLI ───────────────────────────────────────────────────────────────────────

def _cli():
    p = argparse.ArgumentParser(description="CMO person recognition engine")
    p.add_argument("--name",       help="Full name")
    p.add_argument("--first",      help="First name")
    p.add_argument("--last",       help="Last name")
    p.add_argument("--email",      help="Email address")
    p.add_argument("--phone",      help="Phone number")
    p.add_argument("--state",      help="US state (2-letter)")
    p.add_argument("--company",    help="Employer / firm name")
    p.add_argument("--credential", help="Credential (e.g. MD, CPA)")
    p.add_argument("--npi",        help="CMS NPI number")
    p.add_argument("--crd",        help="FINRA CRD number")
    p.add_argument("--github",     help="GitHub username")
    p.add_argument("--arxiv-id",   help="arXiv author ID")
    p.add_argument("--limit",      type=int, default=10, help="Max matches (default 10)")
    p.add_argument("--json",       action="store_true", help="Raw JSON output")
    args = p.parse_args()

    result = recognize(
        name=args.name or "",
        first=args.first or "",
        last=args.last or "",
        email=args.email or "",
        phone=args.phone or "",
        state=args.state or "",
        company=args.company or "",
        credential=args.credential or "",
        npi=args.npi or "",
        crd=args.crd or "",
        github=args.github or "",
        arxiv_id=args.arxiv_id or "",
        limit=args.limit,
    )

    if args.json:
        print(json.dumps(result, indent=2))
        return

    # Human-readable output
    print(f"\n{'='*60}")
    print(f"  RECOGNITION RESULT")
    print(f"{'='*60}")
    print(f"  Tier:     {result['tier']}")
    print(f"  Score:    {result['score']}")
    print(f"  Elapsed:  {result['elapsed_ms']}ms")
    print(f"  Shards:   {len(result['shards_searched'])}")
    print(f"  Matches:  {len(result['matches'])}")
    if result['query']:
        print(f"\n  Signals used:")
        for k, v in result['query'].items():
            print(f"    {k:<16} {v}")

    if result['matches']:
        print(f"\n  Top matches:")
        for i, m in enumerate(result['matches'], 1):
            print(f"\n  [{i}] {m['shard']}  (score={m['score']:.3f})")
            print(f"      ID:    {m['source_id']}")
            name_parts = " ".join(filter(None, [m['first_name'], m['middle'], m['last_name']]))
            if name_parts:
                print(f"      Name:  {name_parts}")
            if m['state']:
                print(f"      State: {m['state']}")
            if m['employer']:
                print(f"      Firm:  {m['employer']}")
            if m['credential']:
                print(f"      Cred:  {m['credential']}")
            if m['domain']:
                print(f"      Domain:{m['domain']}")
            if m['signals']:
                print(f"      Via:   {', '.join(m['signals'])}")
    else:
        print(f"\n  No matches found in {len(result['shards_searched'])} shards.")

    print(f"\n{'='*60}\n")

if __name__ == "__main__":
    _cli()
