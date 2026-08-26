#!/usr/bin/env python3
"""Cross-reference FINRA-registered people by the products/specialties they hold.

Each person's qualifications live in bc_exam (one row per exam category, with the
full exam name and scope IA/BC). This lets us ask the questions a flat broker
table can't: who is actually qualified to do X, who holds a given *combination*
of qualifications, and who are a given person's true peers (same specialty set).
Every result resolves to a real FINRA individualId (CRD) — never synthesized.

Modes:
  --catalog                 the menu: every specialty with a head-count
  --cat "Series 79"         people holding a specialty (by category code)
  --specialty "Investment"  people holding a specialty (substring of exam name)
  --holds "Series 24,79"    people holding ALL of a set (intersection / combo)
  --peers CRD               people whose specialty set best overlaps this person

People are ordered by the same competence/reliability score as rank_people, so
the most reliable qualified individual surfaces first.

Run: python3 -m cmo.cross_ref --cat "Series 79" [--limit N] [--active]
"""
import argparse
import sqlite3

from .rank_people import score_row

_SEL = ("SELECT crd, first_name, middle_name, last_name, scope, exams, "
        "exam_count, days_in_industry, registered_states, num_disclosures, "
        "cur_firm_name FROM bc_broker")


def _name(r):
    return " ".join(x for x in (r["first_name"], r["middle_name"],
                                r["last_name"]) if x)


def _ranked(conn, crds, limit, active):
    """Score + sort a set of CRDs, returning the top `limit` rows."""
    if not crds:
        return []
    out = []
    for crd in crds:
        r = conn.execute(_SEL + " WHERE crd=?", (crd,)).fetchone()
        if not r:
            continue
        if active and r["scope"] != "Active":
            continue
        s = score_row(conn, r)
        out.append((s[0], r, s))
    out.sort(key=lambda x: x[0], reverse=True)
    return out[:limit]


def _print(conn, ranked):
    print(f"{'#':>3}  {'CRD':>8}  {'score':>7}  {'ex':>3} {'yrs':>4}  "
          f"name — current firm  [specialties]")
    for i, (sc, r, s) in enumerate(ranked, 1):
        _, comp, pen, nex, prin, ten, nst = s
        cats = [x[0] for x in conn.execute(
            "SELECT exam_category FROM bc_exam WHERE broker_crd=? ORDER BY exam_category",
            (r["crd"],)).fetchall()]
        print(f"{i:>3}  {r['crd']:>8}  {sc:>7.1f}  {nex:>3} {ten:>4.0f}  "
              f"{_name(r)} — {r['cur_firm_name'] or '?'}  "
              f"[{', '.join(cats)}]")


def catalog(conn, limit):
    rows = conn.execute(
        """SELECT exam_category, exam_name, exam_scope, COUNT(*) n
           FROM bc_exam GROUP BY exam_category
           ORDER BY n DESC LIMIT ?""", (limit,)).fetchall()
    print(f"{'people':>7}  {'scope':>5}  category — specialty")
    for cat, name, scope, n in rows:
        print(f"{n:>7}  {scope or '?':>5}  {cat} — {name or '?'}")


def by_category(conn, cat, limit, active):
    crds = [r[0] for r in conn.execute(
        "SELECT DISTINCT broker_crd FROM bc_exam WHERE exam_category=?",
        (cat,)).fetchall()]
    print(f"== {len(crds)} people hold '{cat}' ==")
    _print(conn, _ranked(conn, crds, limit, active))


def by_specialty(conn, term, limit, active):
    crds = [r[0] for r in conn.execute(
        "SELECT DISTINCT broker_crd FROM bc_exam WHERE exam_name LIKE ?",
        (f"%{term}%",)).fetchall()]
    print(f"== {len(crds)} people hold a specialty matching '{term}' ==")
    _print(conn, _ranked(conn, crds, limit, active))


def holds_all(conn, cats, limit, active):
    """People holding EVERY category in `cats` (the combination query)."""
    q = ("SELECT broker_crd FROM bc_exam WHERE exam_category IN (%s) "
         "GROUP BY broker_crd HAVING COUNT(DISTINCT exam_category)=?" %
         ",".join("?" * len(cats)))
    crds = [r[0] for r in conn.execute(q, (*cats, len(cats))).fetchall()]
    print(f"== {len(crds)} people hold ALL of {cats} ==")
    _print(conn, _ranked(conn, crds, limit, active))


def peers(conn, crd, limit, active):
    """People whose specialty set overlaps this person's the most (true peers)."""
    mine = [r[0] for r in conn.execute(
        "SELECT exam_category FROM bc_exam WHERE broker_crd=?", (crd,)).fetchall()]
    if not mine:
        print(f"CRD {crd} has no recorded specialties (re-detail it first).")
        return
    who = conn.execute(_SEL + " WHERE crd=?", (crd,)).fetchone()
    print(f"== peers of {crd} {_name(who) if who else ''} — "
          f"specialties [{', '.join(mine)}] ==")
    rows = conn.execute(
        """SELECT broker_crd, COUNT(*) shared FROM bc_exam
           WHERE exam_category IN (%s) AND broker_crd!=?
           GROUP BY broker_crd ORDER BY shared DESC LIMIT ?""" %
        ",".join("?" * len(mine)),
        (*mine, crd, max(limit * 4, 40))).fetchall()
    overlap = {c: n for c, n in rows}
    ranked = _ranked(conn, list(overlap), limit, active)
    print(f"{'#':>3}  {'CRD':>8}  {'shared':>6}  {'score':>7}  name — firm")
    for i, (sc, r, s) in enumerate(ranked, 1):
        print(f"{i:>3}  {r['crd']:>8}  {overlap.get(r['crd'],0):>6}  {sc:>7.1f}  "
              f"{_name(r)} — {r['cur_firm_name'] or '?'}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--limit", type=int, default=30)
    ap.add_argument("--active", action="store_true", help="only currently-active")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--catalog", action="store_true",
                   help="list every specialty with a head-count")
    g.add_argument("--cat", help="people holding a category code, e.g. 'Series 79'")
    g.add_argument("--specialty", help="people holding a specialty (name substring)")
    g.add_argument("--holds", help="people holding ALL of a comma-list of categories")
    g.add_argument("--peers", type=int, help="CRD: people sharing its specialty set")
    args = ap.parse_args()

    conn = sqlite3.connect(args.db, timeout=60)
    conn.execute("PRAGMA busy_timeout=120000")
    conn.row_factory = sqlite3.Row

    if args.catalog:
        catalog(conn, args.limit)
    elif args.cat:
        by_category(conn, args.cat, args.limit, args.active)
    elif args.specialty:
        by_specialty(conn, args.specialty, args.limit, args.active)
    elif args.holds:
        holds_all(conn, [c.strip() for c in args.holds.split(",") if c.strip()],
                  args.limit, args.active)
    elif args.peers:
        peers(conn, args.peers, args.limit, args.active)


if __name__ == "__main__":
    main()
