#!/usr/bin/env python3
"""Unified competent-people graph: join the FINRA shard to the HN shard.

Two indexed populations, each grounded in a real first-party identity:
  - FINRA shard (data/cmo/cmo.db): bc_broker / bc_exam / bc_disclosure,
    keyed by FINRA individual CRD.
  - HN shard (dsco-trajectory-management jobs.db): candidates (extracted_name)
    and personas (canonical_name), keyed by hn_username.

The cross-domain join is by normalized real name (first+last token), with US
state corroboration as the confidence tier: a person whose HN location state
matches a state on their FINRA record is a far stronger identity match than a
bare name collision. Output always carries BOTH ids (CRD and hn_username) plus
the evidence, so nothing is asserted beyond what the records support.

Modes:
  --stats           shard sizes and join-key coverage
  --overlap         people present in BOTH shards (finance x tech)
  --find "name"     search both shards for a person by name

Run: python3 -m cmo.people_graph --overlap [--corroborated] [--limit N]
"""
import argparse
import os
import re
import sqlite3
from collections import defaultdict

HN_DB = os.path.expanduser(
    "~/Dsco/dsco-trajectory-management/data/jobs.db")

_STATES = {
    "alabama": "AL", "alaska": "AK", "arizona": "AZ", "arkansas": "AR",
    "california": "CA", "colorado": "CO", "connecticut": "CT", "delaware": "DE",
    "florida": "FL", "georgia": "GA", "hawaii": "HI", "idaho": "ID",
    "illinois": "IL", "indiana": "IN", "iowa": "IA", "kansas": "KS",
    "kentucky": "KY", "louisiana": "LA", "maine": "ME", "maryland": "MD",
    "massachusetts": "MA", "michigan": "MI", "minnesota": "MN",
    "mississippi": "MS", "missouri": "MO", "montana": "MT", "nebraska": "NE",
    "nevada": "NV", "new hampshire": "NH", "new jersey": "NJ",
    "new mexico": "NM", "new york": "NY", "north carolina": "NC",
    "north dakota": "ND", "ohio": "OH", "oklahoma": "OK", "oregon": "OR",
    "pennsylvania": "PA", "rhode island": "RI", "south carolina": "SC",
    "south dakota": "SD", "tennessee": "TN", "texas": "TX", "utah": "UT",
    "vermont": "VT", "virginia": "VA", "washington": "WA",
    "west virginia": "WV", "wisconsin": "WI", "wyoming": "WY",
    "nyc": "NY", "sf": "CA",
}
_ABBR = set(_STATES.values())


def name_key(name):
    """(first, last) lowered — first token + last alpha token, or None."""
    if not name:
        return None
    toks = [t for t in re.split(r"[^A-Za-z]+", name) if t]
    if len(toks) < 2:
        return None
    first, last = toks[0].lower(), toks[-1].lower()
    if len(first) < 2 or len(last) < 2:   # initials can't identify a person
        return None
    return first, last


def loc_states(loc):
    """US state codes mentioned in a free-text HN location string."""
    if not loc:
        return set()
    out = set()
    low = loc.lower()
    for full, ab in _STATES.items():
        if re.search(r"\b" + re.escape(full) + r"\b", low):
            out.add(ab)
    for m in re.finditer(r"\b([A-Z]{2})\b", loc):
        if m.group(1) in _ABBR:
            out.add(m.group(1))
    return out


def load_hn(hn_db):
    """name_key -> [(hn_username, name, location, title, skills, src)]"""
    conn = sqlite3.connect(f"file:{hn_db}?mode=ro", uri=True, timeout=60)
    idx = defaultdict(list)
    n = 0
    for u, nm, loc, title, sk in conn.execute(
            """SELECT hn_username, canonical_name, location, title,
                      technologies FROM personas
               WHERE canonical_name IS NOT NULL AND canonical_name!=''"""):
        k = name_key(nm)
        if k:
            idx[k].append((u, nm, loc, title, sk, "persona")); n += 1
    seen = {(e[0] or "").lower() for v in idx.values() for e in v}
    for u, nm, loc, title, sk in conn.execute(
            """SELECT hn_username, extracted_name, location, title,
                      technologies FROM candidates
               WHERE extracted_name IS NOT NULL AND extracted_name!=''
               GROUP BY hn_username HAVING MAX(last_seen)"""):
        if (u or "").lower() in seen:
            continue
        k = name_key(nm)
        if k:
            idx[k].append((u, nm, loc, title, sk, "candidate")); n += 1
    conn.close()
    return idx, n


def broker_states(r):
    out = set()
    if r["cur_state"]:
        out.add(r["cur_state"])
    out |= {s for s in (r["registered_states"] or "").split(";") if s in _ABBR}
    return out


def overlap(conn, idx, limit, corroborated_only):
    rows = conn.execute(
        """SELECT crd, first_name, middle_name, last_name, cur_state,
                  registered_states, cur_firm_name, exams, scope
           FROM bc_broker WHERE first_name IS NOT NULL""").fetchall()
    hits = []
    for r in rows:
        k = name_key(f"{r['first_name']} {r['last_name']}")
        if not k or k not in idx:
            continue
        bstates = broker_states(r)
        for u, nm, loc, title, sk, src in idx[k]:
            shared = bstates & loc_states(loc)
            tier = "name+state" if shared else "name"
            if corroborated_only and not shared:
                continue
            hits.append((1 if shared else 0, r, u, nm, loc, title, sk, shared))
    hits.sort(key=lambda h: h[0], reverse=True)
    print(f"== {len(hits)} cross-domain matches "
          f"({sum(1 for h in hits if h[0])} state-corroborated) ==")
    for c, r, u, nm, loc, title, sk, shared in hits[:limit]:
        nmb = " ".join(x for x in (r["first_name"], r["middle_name"],
                                   r["last_name"]) if x)
        tier = f"name+{'/'.join(sorted(shared))}" if shared else "name-only"
        print(f"[{tier}]")
        print(f"  FINRA  CRD {r['crd']}: {nmb} ({r['scope']}) — "
              f"{r['cur_firm_name'] or '?'}  exams[{r['exams'] or ''}]")
        print(f"  HN     {u}: {nm} — {loc or '?'}  {title or ''}")
        if sk:
            print(f"         skills: {sk[:90]}")
    return hits


def find(conn, idx, name):
    k = name_key(name)
    if not k:
        print("need a first and last name"); return
    print(f"== FINRA shard: {k[0]} {k[1]} ==")
    for r in conn.execute(
            """SELECT crd, first_name, middle_name, last_name, scope,
                      cur_firm_name, cur_state, exams, num_disclosures
               FROM bc_broker
               WHERE lower(first_name)=? AND lower(last_name)=?""", k):
        nm = " ".join(x for x in (r["first_name"], r["middle_name"],
                                  r["last_name"]) if x)
        print(f"  CRD {r['crd']}: {nm} ({r['scope']}) {r['cur_state'] or ''} — "
              f"{r['cur_firm_name'] or '?'}  exams[{r['exams'] or ''}]  "
              f"disclosures={r['num_disclosures'] or 0}")
    print(f"== HN shard: {k[0]} {k[1]} ==")
    for u, nm, loc, title, sk, src in idx.get(k, []):
        print(f"  {u} ({src}): {nm} — {loc or '?'}  {title or ''}")
        if sk:
            print(f"    skills: {sk[:90]}")


def stats(conn, idx, n_hn):
    nb = conn.execute("SELECT COUNT(*) FROM bc_broker").fetchone()[0]
    nd = conn.execute(
        "SELECT COUNT(*) FROM bc_broker WHERE detailed=1").fetchone()[0]
    nx = conn.execute(
        "SELECT COUNT(DISTINCT broker_crd) FROM bc_exam").fetchone()[0]
    print(f"FINRA shard : {nb} brokers ({nd} detailed, {nx} with specialty rows)")
    print(f"HN shard    : {n_hn} named people across {len(idx)} name keys")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--hn-db", default=HN_DB)
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--corroborated", action="store_true",
                    help="overlap: only name+state matches")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--stats", action="store_true")
    g.add_argument("--overlap", action="store_true")
    g.add_argument("--find", help="search both shards by 'First Last'")
    args = ap.parse_args()

    conn = sqlite3.connect(args.db, timeout=60)
    conn.execute("PRAGMA busy_timeout=120000")
    conn.row_factory = sqlite3.Row
    idx, n_hn = load_hn(args.hn_db)

    if args.stats:
        stats(conn, idx, n_hn)
    elif args.overlap:
        overlap(conn, idx, args.limit, args.corroborated)
    elif args.find:
        find(conn, idx, args.find)


if __name__ == "__main__":
    main()
