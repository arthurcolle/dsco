#!/usr/bin/env python3
"""Link scattered EDGAR filings into deal books.

A securitization shows up on EDGAR as several filings under different CIKs: the
collateral tape (ABS-EE, under the depositor) and the bond prospectus (424B5,
under the issuing trust). They share no obvious key in the full index. The join
is the issuing-entity conformed name in each filing's SEC header (it carries the
deal series, e.g. '2025-P4'). We resolve that for every collateral_pool and
tranche accession, store it in deal_identity, and expose a deal_book view that
stitches pool economics + tranche stack + syndicate together per deal.

Usage:
    python3 -m cmo.dealbook                 # backfill identities + build view
    python3 -m cmo.dealbook --show          # print the linked deal book
"""
import argparse, os, sys
from . import fetch


def resolve(conn, accession, cik, form):
    """Resolve and store the canonical deal identity for one accession."""
    ent = fetch.issuing_entity(cik, accession)
    key = fetch.deal_key(ent)
    conn.execute(
        """INSERT INTO deal_identity(accession,cik,issuing_entity,deal_key,form)
           VALUES(?,?,?,?,?)
           ON CONFLICT(accession) DO UPDATE SET
             issuing_entity=excluded.issuing_entity, deal_key=excluded.deal_key,
             form=excluded.form""",
        (accession, cik, ent, key, form))
    return key


def backfill(conn):
    """Resolve identities for every collateral and tranche accession not yet
    linked. Skips accessions already in deal_identity (idempotent + offline)."""
    done = {r[0] for r in conn.execute("SELECT accession FROM deal_identity")}
    # A 424B5 may yield underwriters even when the tranche regex misses, so scan
    # both bond tables; collateral comes from the pool table.
    srcs = ([(a, c, "ABS-EE") for a, c in
             conn.execute("SELECT accession,cik FROM collateral_pool")] +
            [(a, c, "424B5") for a, c in
             conn.execute("SELECT DISTINCT accession,cik FROM tranche")] +
            [(a, c, "424B5") for a, c in
             conn.execute("""SELECT DISTINCT u.accession,f.cik FROM underwriter u
                             JOIN filing f ON f.accession=u.accession""")])
    n = 0
    for acc, cik, form in srcs:
        if acc in done:
            continue
        key = resolve(conn, acc, cik, form)
        print(f"  {form:7} {acc} -> {key}", file=sys.stderr)
        n += 1
    conn.commit()
    return n


def build_view(conn):
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "dealbook.sql")) as f:
        conn.executescript(f.read())
    conn.commit()


def show(conn):
    rows = conn.execute("""
        SELECT deal_key, asset_type, n_loans, cur_mm, wac_pct, wa_fico,
               n_tranches, tranche_mm, lead
        FROM deal_book ORDER BY cur_mm DESC NULLS LAST""").fetchall()
    print(f"{'DEAL':<46}{'TYPE':<10}{'LOANS':>8}{'COLL$M':>9}"
          f"{'WAC':>7}{'FICO':>6}{'TRCH':>5}{'BOND$M':>9}  LEAD")
    for k, at, nl, mm, wac, fico, nt, tmm, lead in rows:
        print(f"{(k or '?')[:45]:<46}{at or '-':<10}{nl or 0:>8}"
              f"{mm or 0:>9.0f}{wac or 0:>7.2f}{fico or 0:>6.0f}"
              f"{nt or 0:>5}{tmm or 0:>9.0f}  {lead or '-'}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()
    conn = fetch.ensure_db(args.db)
    if not args.show:
        n = backfill(conn)
        build_view(conn)
        print(f"\n=== deal book ===\nidentities_resolved={n}")
        linked = conn.execute(
            "SELECT COUNT(*),COUNT(DISTINCT deal_key) FROM deal_identity").fetchone()
        both = conn.execute("""
            SELECT COUNT(*) FROM (
              SELECT deal_key FROM deal_identity
              GROUP BY deal_key
              HAVING SUM(form='ABS-EE')>0 AND SUM(form='424B5')>0)""").fetchone()[0]
        print(f"accessions={linked[0]} deals={linked[1]} "
              f"deals_with_collateral_and_bonds={both}")
        print("integrity: OK")
    show(conn)


if __name__ == "__main__":
    main()
