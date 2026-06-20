#!/usr/bin/env python3
"""Rank FINRA-registered people by competence + reliability.

This is the financial-services shard of the competent-people index. Raw coverage
isn't the point — surfacing *reliable, competent* individuals is. Every signal
here is first-party FINRA public data (Rule 8312), so a score is defensible and
each row resolves to a real individualId (CRD).

Competence (higher = more):
  - exams passed (breadth of qualification), with a bonus for principal exams
    (Series 24/26/27/28/4/9/10/51/53/23 — supervisory/management quals)
  - tenure in years (industry start date -> now)
  - breadth of state registration

Reliability (penalty, severity-weighted disclosures):
  Criminal/Regulatory worst, then Employment-Separation-After-Allegations,
  Financial/Judgment-Lien, Customer Dispute, Civil/Investigation.

Run: python3 -m cmo.rank_people [--limit N] [--active] [--state XX] [--min-exams K]
"""
import argparse
import datetime as _dt
import sqlite3

PRINCIPAL = {"24", "26", "27", "28", "4", "9", "10", "51", "53", "23", "39"}

DISC_WEIGHT = {
    "Criminal": 8.0,
    "Regulatory": 6.0,
    "Employment Separation After Allegations": 5.0,
    "Financial": 3.0,
    "Judgment / Lien": 3.0,
    "Customer Dispute": 2.0,
    "Civil": 2.0,
    "Investigation": 1.0,
}


def _tenure_years(date_str):
    if not date_str:
        return 0.0
    for fmt in ("%m/%d/%Y", "%Y-%m-%d"):
        try:
            d = _dt.datetime.strptime(date_str.strip(), fmt)
            return max(0.0, (_dt.datetime.now() - d).days / 365.25)
        except ValueError:
            continue
    return 0.0


def _exam_score(exams):
    if not exams:
        return 0, False
    parts = [e.strip() for e in exams.split(";") if e.strip()]
    seen, principal = set(), False
    for e in parts:
        # "Series 7TO" -> number 7 ; "SIE"/"PC" kept as-is
        token = e.replace("Series", "").strip()
        num = ""
        for ch in token:
            if ch.isdigit():
                num += ch
            else:
                break
        key = num if num else e
        seen.add(key)
        if num and num in PRINCIPAL:
            principal = True
    return len(seen), principal


def _disclosure_penalty(conn, crd):
    rows = conn.execute(
        "SELECT type, COUNT(*) FROM bc_disclosure WHERE broker_crd=? GROUP BY type",
        (crd,)).fetchall()
    return sum(DISC_WEIGHT.get(t, 2.0) * n for t, n in rows)


def score_row(conn, r):
    n_exams, principal = _exam_score(r["exams"])
    tenure = _tenure_years(r["days_in_industry"])
    n_states = len([s for s in (r["registered_states"] or "").split(";") if s])
    competence = (n_exams * 1.0
                  + (3.0 if principal else 0.0)
                  + min(tenure, 45.0) * 0.5
                  + min(n_states, 53) * 0.1)
    penalty = _disclosure_penalty(conn, r["crd"]) if r["num_disclosures"] else 0.0
    return competence - penalty, competence, penalty, n_exams, principal, tenure, n_states


def rank(conn, limit, active, state, min_exams):
    conn.row_factory = sqlite3.Row
    q = ("SELECT crd, first_name, middle_name, last_name, scope, exams, "
         "exam_count, days_in_industry, registered_states, num_disclosures, "
         "cur_firm_name FROM bc_broker WHERE detailed=1")
    args = []
    if active:
        q += " AND scope='Active'"
    if state:
        q += " AND registered_states LIKE ?"
        args.append(f"%{state}%")
    if min_exams:
        q += " AND exam_count>=?"
        args.append(min_exams)
    rows = conn.execute(q, args).fetchall()
    scored = []
    for r in rows:
        s = score_row(conn, r)
        scored.append((s[0], r, s))
    scored.sort(key=lambda x: x[0], reverse=True)
    return scored[:limit]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--active", action="store_true",
                    help="only currently-active reps")
    ap.add_argument("--state", help="filter to a state (e.g. NY or 'New York')")
    ap.add_argument("--min-exams", type=int, default=0)
    args = ap.parse_args()

    conn = sqlite3.connect(args.db, timeout=60)
    conn.execute("PRAGMA busy_timeout=120000")
    top = rank(conn, args.limit, args.active, args.state, args.min_exams)

    print(f"{'#':>3}  {'CRD':>8}  {'score':>7}  {'comp':>6}  {'pen':>6}  "
          f"{'ex':>3} {'P':>1} {'yrs':>4} {'st':>3}  name / current firm")
    for i, (sc, r, s) in enumerate(top, 1):
        _, comp, pen, nex, prin, ten, nst = s
        nm = " ".join(x for x in (r["first_name"], r["middle_name"],
                                  r["last_name"]) if x)
        print(f"{i:>3}  {r['crd']:>8}  {sc:>7.1f}  {comp:>6.1f}  {pen:>6.1f}  "
              f"{nex:>3} {'Y' if prin else '.':>1} {ten:>4.0f} {nst:>3}  "
              f"{nm} — {r['cur_firm_name'] or '?'}")


if __name__ == "__main__":
    main()
