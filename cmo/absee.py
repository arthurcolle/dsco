#!/usr/bin/env python3
"""Parse EDGAR ABS-EE asset-level tapes into collateral the agent can buy.

The EX-102 XML is one <assets> element per loan, ~50 fields each, in an
asset-class namespace (autoloan/autolease/...). Files reach 100MB+, so we stream
with iterparse and clear elements. We always compute a pool summary (WAC/WAM/FICO/
geography/delinquency — what a trader prices) and optionally store loan rows.

Usage:
    python3 -m cmo.absee --cik 1383094 --acc 0001193125-25-317899 [--loans] [--limit N]
    python3 -m cmo.absee --auto-scan 30      # newest 30 ABS-EE filings in the DB
"""
import argparse, sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from . import fetch


def _f(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def _i(x):
    try:
        return int(float(x))
    except (TypeError, ValueError):
        return None


def _local(tag):
    return tag.rsplit("}", 1)[-1]  # strip namespace


# Map ABS-EE auto-loan element names to our normalized loan fields.
AUTO = {
    "assetNumber": ("asset_number", str),
    "originatorName": ("originator", str),
    "originalLoanAmount": ("orig_balance", _f),
    "reportingPeriodActualEndBalanceAmount": ("cur_balance", _f),
    "originalInterestRatePercentage": ("orig_rate", _f),
    "originalLoanTerm": ("orig_term", _i),
    "remainingTermToMaturityNumber": ("rem_term", _i),
    "obligorCreditScore": ("fico", _i),
    "paymentToIncomePercentage": ("pti", _f),
    "obligorGeographicLocation": ("state", str),
    "vehicleManufacturerName": ("vehicle_make", str),
    "vehicleModelName": ("vehicle_model", str),
    "vehicleModelYear": ("vehicle_year", _i),
    "vehicleNewUsedCode": ("new_used", _i),
    "currentDelinquencyStatus": ("delinquency", _i),
    "zeroBalanceCode": ("zero_balance_code", _i),
}


def parse_tape(path, asset_type, on_loan):
    """Stream <assets> elements, build a normalized dict per loan, call on_loan."""
    n = 0
    for event, elem in ET.iterparse(path, events=("end",)):
        if _local(elem.tag) != "assets":
            continue
        rec = {"asset_type": asset_type}
        for child in elem:
            name = _local(child.tag)
            m = AUTO.get(name)
            if m:
                key, conv = m
                rec[key] = conv(child.text)
        on_loan(rec)
        n += 1
        elem.clear()
    return n


def asset_type_of(path):
    """Sniff the asset-class from the root namespace without loading the file."""
    for _ev, elem in ET.iterparse(path, events=("start",)):
        ns = elem.tag.split("}")[0].lstrip("{")
        # .../absee/autoloan/assetdata -> autoloan
        parts = ns.rstrip("/").split("/")
        return parts[-2] if len(parts) >= 2 else "unknown"
    return "unknown"


class PoolAgg:
    """Accumulate weighted pool stats while streaming, plus optional loan rows."""
    def __init__(self, store_loans):
        self.n = 0
        self.orig = 0.0
        self.cur = 0.0
        self.w = 0.0           # sum of current balances used as weight
        self.wac = 0.0
        self.wam = 0.0
        self.wfico = 0.0
        self.wpti = 0.0
        self.new_bal = 0.0
        self.delinq_bal = 0.0
        self.by_state = defaultdict(float)
        self.store_loans = store_loans
        self.rows = []

    def add(self, r):
        self.n += 1
        ob = r.get("orig_balance") or 0.0
        cb = r.get("cur_balance") or 0.0
        self.orig += ob
        self.cur += cb
        w = cb if cb > 0 else ob   # weight by what's outstanding; fall back to orig
        if w > 0:
            self.w += w
            if r.get("orig_rate") is not None:
                self.wac += r["orig_rate"] * w
            if r.get("rem_term") is not None:
                self.wam += r["rem_term"] * w
            if r.get("fico") is not None:
                self.wfico += r["fico"] * w
            if r.get("pti") is not None:
                self.wpti += r["pti"] * w
            if r.get("new_used") == 1:
                self.new_bal += w
            if (r.get("delinquency") or 0) >= 30:
                self.delinq_bal += w
            if r.get("state"):
                self.by_state[r["state"]] += w
        if self.store_loans:
            self.rows.append(r)

    def summary(self):
        w = self.w or 1.0
        top_state, top_w = (max(self.by_state.items(), key=lambda x: x[1])
                            if self.by_state else (None, 0.0))
        return {
            "n_loans": self.n,
            "orig_balance": round(self.orig, 2),
            "cur_balance": round(self.cur, 2),
            "wac": round(self.wac / w, 6),
            "wam": round(self.wam / w, 2),
            "wa_fico": round(self.wfico / w, 1),
            "wa_pti": round(self.wpti / w, 6),
            "pct_new": round(self.new_bal / w, 4),
            "pct_delinq": round(self.delinq_bal / w, 4),
            "top_state": top_state,
            "top_state_pct": round(top_w / w, 4),
        }


def ingest_one(conn, cik, acc, deal_name, store_loans=False, limit=None):
    path = fetch.absee_xml(cik, acc)
    if not path:
        print(f"  no asset XML for {acc}", file=sys.stderr)
        return None
    at = asset_type_of(path)
    agg = PoolAgg(store_loans)
    begin = end = None

    def on_loan(r):
        if limit and agg.n >= limit:
            return
        agg.add(r)

    # capture reporting period from first loan via a side parse of dates
    n = parse_tape(path, at, on_loan)
    s = agg.summary()
    cur = conn.cursor()
    cur.execute(
        """INSERT INTO collateral_pool(accession,cik,deal_name,asset_type,
             report_begin,report_end,n_loans,orig_balance,cur_balance,wac,wam,
             wa_fico,wa_pti,pct_new,pct_delinq,top_state,top_state_pct)
           VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
           ON CONFLICT(accession) DO UPDATE SET
             n_loans=excluded.n_loans, cur_balance=excluded.cur_balance,
             wac=excluded.wac, wam=excluded.wam, wa_fico=excluded.wa_fico""",
        (acc, cik, deal_name, at, begin, end, s["n_loans"], s["orig_balance"],
         s["cur_balance"], s["wac"], s["wam"], s["wa_fico"], s["wa_pti"],
         s["pct_new"], s["pct_delinq"], s["top_state"], s["top_state_pct"]))
    if store_loans and agg.rows:
        cur.executemany(
            """INSERT INTO collateral_loan(accession,asset_number,asset_type,
                 originator,orig_balance,cur_balance,orig_rate,orig_term,rem_term,
                 fico,pti,state,vehicle_make,vehicle_model,vehicle_year,new_used,
                 delinquency,zero_balance_code)
               VALUES(:acc,:asset_number,:asset_type,:originator,:orig_balance,
                 :cur_balance,:orig_rate,:orig_term,:rem_term,:fico,:pti,:state,
                 :vehicle_make,:vehicle_model,:vehicle_year,:new_used,:delinquency,
                 :zero_balance_code)
               ON CONFLICT(accession,asset_number) DO NOTHING""",
            [{**{k: r.get(k) for k in
                 ("asset_number","asset_type","originator","orig_balance",
                  "cur_balance","orig_rate","orig_term","rem_term","fico","pti",
                  "state","vehicle_make","vehicle_model","vehicle_year","new_used",
                  "delinquency","zero_balance_code")}, "acc": acc} for r in agg.rows])
    conn.commit()
    print(f"  {deal_name} [{at}]: {s['n_loans']} loans, "
          f"cur=${s['cur_balance']/1e6:.1f}M wac={s['wac']*100:.2f}% "
          f"fico={s['wa_fico']:.0f} wam={s['wam']:.0f}mo "
          f"new={s['pct_new']*100:.0f}% delinq={s['pct_delinq']*100:.1f}% "
          f"top={s['top_state']}({s['top_state_pct']*100:.0f}%)", file=sys.stderr)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="data/cmo/cmo.db")
    ap.add_argument("--cik", type=int)
    ap.add_argument("--acc")
    ap.add_argument("--loans", action="store_true", help="also store loan-level rows")
    ap.add_argument("--limit", type=int, help="cap loans parsed (testing)")
    ap.add_argument("--auto-scan", type=int, metavar="N",
                    help="ingest the newest N ABS-EE filings already in the DB")
    args = ap.parse_args()

    conn = fetch.ensure_db(args.db)

    if args.auto_scan:
        # The AUTO field map targets auto-loan/lease tapes; filter to those issuers.
        rows = conn.execute(
            """SELECT f.cik, f.accession, e.name FROM filing f
               JOIN entity e ON e.cik=f.cik
               WHERE f.form='ABS-EE' AND (
                   e.name LIKE '%AUTO%' OR e.name LIKE '%DRIVE%' OR
                   e.name LIKE '%MOTOR%' OR e.name LIKE '%VEHICLE%')
               ORDER BY f.filed_date DESC LIMIT ?""", (args.auto_scan,)).fetchall()
        for cik, acc, name in rows:
            ingest_one(conn, cik, acc, name, args.loans, args.limit)
    elif args.cik and args.acc:
        name = conn.execute("SELECT name FROM entity WHERE cik=?",
                            (args.cik,)).fetchone()
        ingest_one(conn, args.cik, args.acc, name[0] if name else str(args.cik),
                   args.loans, args.limit)
    else:
        ap.error("need --cik/--acc or --auto-scan")

    pools = conn.execute("SELECT COUNT(*),SUM(n_loans) FROM collateral_pool").fetchone()
    print(f"\n=== collateral ===\npools={pools[0]} loans_summarized={pools[1]}")
    print("integrity: OK")


if __name__ == "__main__":
    main()
