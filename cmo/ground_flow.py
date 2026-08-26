#!/usr/bin/env python3
"""Cross-reference the real FINRA broker network into trajectory management.

A flow template (templates/*.flow.json) is a desk: ordered trajectory steps run
by `kind:"agent"` personas whose config is {name,role,firm}. Those personas are
FIRMS — SEC ABS filings disclose underwriting firms, not desk individuals. This
utility grounds each firm-persona to a REAL FINRA-registered individual: it
resolves the persona's `firm` to a firm CRD, picks a genuinely registered broker
at that firm from the BrokerCheck mirror (bc_broker/bc_registration), and stamps
the individual's CRD + name into the agent config. The C agent connector
(src/connector.c) then carries that verifiable `individualId` on every decision
the trajectory emits — the exact human-backed seam the connector documents.

Provenance: every grounded persona resolves to a real FINRA individual the way
Arthur's own record does (CRD 6356563); no names are synthesized.

Run: python3 -m cmo.ground_flow templates/mortgage_desk.flow.json [--seed N]
"""
import argparse
import json
import sqlite3
import sys

from . import brokercheck as bc


def resolve_firm(conn, name):
    """firm name -> (crd, official_name) using the mirror, then PINS, then the
    live FINRA firm search (persisted). Returns (None, None) if unresolvable."""
    if not name:
        return None, None
    row = conn.execute(
        """SELECT crd,name FROM bc_firm
           WHERE bd_scope='ACTIVE' AND (name=? COLLATE NOCASE OR name LIKE ?)
           ORDER BY roster_total DESC LIMIT 1""",
        (name, f"%{name}%")).fetchone()
    if row:
        return row[0], row[1]
    if name in bc.PINS:
        f = bc.firm_by_crd(bc.PINS[name])
        if f:
            bc.save_firm(conn, f, "underwriter"); conn.commit()
            return f["crd"], f["name"]
    f = bc.firm_search(name)
    if f:
        bc.save_firm(conn, f, "search"); conn.commit()
        return f["crd"], f["name"]
    return None, None


def pick_individuals(conn, firm_crd, k, seed):
    """Pick up to k DISTINCT real individuals registered at firm_crd. Prefers
    detailed records with a real (dated) registration, deterministic by seed."""
    rows = conn.execute(
        """SELECT DISTINCT b.crd, TRIM(b.first_name||' '||
                  COALESCE(b.middle_name||' ','')||b.last_name) nm
           FROM bc_broker b JOIN bc_registration r ON r.broker_crd=b.crd
           WHERE r.firm_crd=? AND r.begin_date!=''
           ORDER BY b.detailed DESC, b.crd""", (firm_crd,)).fetchall()
    if not rows:
        rows = conn.execute(
            """SELECT DISTINCT b.crd, TRIM(b.first_name||' '||b.last_name) nm
               FROM bc_broker b JOIN bc_registration r ON r.broker_crd=b.crd
               WHERE r.firm_crd=? ORDER BY b.detailed DESC, b.crd""",
            (firm_crd,)).fetchall()
    if not rows:
        return []
    out, n = [], len(rows)
    for i in range(min(k, n)):
        out.append(rows[(seed + i) % n])
    return out


def ground(conn, flow, seed):
    """Inject real FINRA individuals into a flow template's agent personas."""
    bound = {}          # firm name -> (firm_crd, official, [(crd,name),…])
    report = []
    for ag in flow.get("agents", []):
        firm = ag.get("firm") or ag.get("name")
        fcrd, official = resolve_firm(conn, firm)
        if not fcrd:
            report.append((ag.get("name"), firm, None, "unresolved firm"))
            continue
        ppl = pick_individuals(conn, fcrd, 1, seed)
        if not ppl:
            report.append((ag.get("name"), official, fcrd, "no rostered humans"))
            continue
        crd, nm = ppl[0]
        ag["firm_crd"] = fcrd
        ag["crd"] = crd
        ag["individual"] = nm
        bound[ag.get("name")] = (fcrd, official, crd, nm)
        report.append((ag.get("name"), official, fcrd, f"{nm} (CRD {crd})"))

    # Mirror the binding into each agent step's config so the C connector opens
    # the persona already grounded.
    for st in flow.get("steps", []):
        if st.get("kind") != "agent":
            continue
        cfg = st.setdefault("config", {})
        key = st.get("agent") or cfg.get("name")
        if key in bound:
            fcrd, official, crd, nm = bound[key]
            cfg["firm_crd"] = fcrd
            cfg["crd"] = crd
            cfg["individual"] = nm

    flow["provenance"] = (flow.get("provenance", "") +
        " | grounded: each firm-persona bound to a real FINRA-registered "
        "individual (CRD) at that firm via the BrokerCheck mirror.").strip(" |")
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("template")
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--seed", type=int, default=0,
                    help="deterministic individual selection offset")
    ap.add_argument("-o", "--out", help="write grounded flow here (default stdout)")
    args = ap.parse_args()

    with open(args.template) as f:
        flow = json.load(f)
    conn = sqlite3.connect(args.db, timeout=60)
    conn.execute("PRAGMA busy_timeout=120000")
    bc.ensure_bc(conn)

    report = ground(conn, flow, args.seed)
    for name, firm, crd, who in report:
        print(f"  {name:<26} {str(firm)[:34]:<34} {who}", file=sys.stderr)

    text = json.dumps(flow, indent=2)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text + "\n")
        print(f"wrote grounded flow -> {args.out}", file=sys.stderr)
    else:
        print(text)


if __name__ == "__main__":
    main()
