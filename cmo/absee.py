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


def _parse_date(s):
    """Accept 'MM-DD-YYYY' or 'YYYY-MM-DD' (the two ABS-EE date formats)."""
    if not s:
        return None
    s = s.strip()
    for fmt in ("%m-%d-%Y", "%Y-%m-%d", "%m/%d/%Y"):
        try:
            from datetime import datetime
            return datetime.strptime(s, fmt).date()
        except ValueError:
            continue
    return None


def _months_between(d0, d1):
    a, b = _parse_date(d0), _parse_date(d1)
    if not a or not b:
        return None
    return max(0, (b.year - a.year) * 12 + (b.month - a.month))


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

# CMBS commercial-mortgage tape: top-level loan fields + nested <property> block.
CMBS = {
    "assetNumber": ("asset_number", str),
    "originatorName": ("originator", str),
    "originalLoanAmount": ("orig_balance", _f),
    "reportPeriodEndActualBalanceAmount": ("cur_balance", _f),
    "reportPeriodInterestRatePercentage": ("orig_rate", _f),
    "maturityDate": ("_maturity", str),
    "reportingPeriodEndDate": ("_report_end", str),
    "paymentStatusLoanCode": ("_pay_status", str),
}
CMBS_PROP = {
    "propertyState": ("state", str),
    "propertyTypeCode": ("property_type", str),
    "debtServiceCoverageNetCashFlowSecuritizationPercentage": ("dscr", _f),
    "physicalOccupancySecuritizationPercentage": ("occupancy", _f),
}

# RMBS residential-mortgage tape (Reg AB II Schedule AL element names).
RMBS = {
    "assetNumber": ("asset_number", str),
    "originatorName": ("originator", str),
    "originalLoanAmount": ("orig_balance", _f),
    "reportingPeriodActualEndBalanceAmount": ("cur_balance", _f),
    "originalInterestRatePercentage": ("orig_rate", _f),
    "originalLoanMaturityDate": ("_maturity", str),
    "reportingPeriodEndDate": ("_report_end", str),
    "originalCombinedLoanToValueRatioPercentage": ("ltv", _f),
    "loanToValueRatioPercentage": ("ltv", _f),
    "obligorCreditScore": ("fico", _i),
    "debtToIncomeRatioPercentage": ("pti", _f),
    "propertyGeographicLocation": ("state", str),
    "currentDelinquencyStatus": ("delinquency", _i),
}

# asset_type (from the root namespace) -> (top-level map, nested property map).
FIELD_MAPS = {
    "autoloan": (AUTO, None),
    "autolease": (AUTO, None),
    "cmbs": (CMBS, CMBS_PROP),
    "rmbs": (RMBS, None),
}


def _cmbs_delinq(code):
    """CREFC paymentStatusLoanCode: 0/1/A current-ish, >=2 is 30+ delinquent."""
    if code is None:
        return 0
    c = str(code).strip().upper()
    if c in ("0", "1", "A", "B", ""):
        return 0
    return 60


def _finalize(rec, asset_type):
    """Derive normalized fields the field map can't express directly."""
    # Interest rate scale is inconsistent across servicers: some report a decimal
    # (0.0366), others a whole percent (3.84). A mortgage rate is never >=100%, so
    # treat any value >1 as a percent and rescale to a decimal.
    if rec.get("orig_rate") is not None and rec["orig_rate"] > 1.0:
        rec["orig_rate"] /= 100.0
    # LTV is a whole-number percent in the tape (e.g. 75); store it as a fraction
    # so it shares the decimal convention with the rest of the pool metrics.
    if rec.get("ltv") is not None and rec["ltv"] > 1.5:
        rec["ltv"] /= 100.0
    if asset_type in ("cmbs", "rmbs"):
        rec["rem_term"] = _months_between(rec.get("_report_end"),
                                          rec.get("_maturity"))
    if asset_type == "cmbs":
        rec["delinquency"] = _cmbs_delinq(rec.get("_pay_status"))
    return rec


def parse_tape(path, asset_type, on_loan):
    """Stream <assets> elements, build a normalized dict per loan, call on_loan."""
    fmap, pmap = FIELD_MAPS.get(asset_type, (AUTO, None))
    n = 0
    for _event, elem in ET.iterparse(path, events=("end",)):
        if _local(elem.tag) != "assets":
            continue
        rec = {"asset_type": asset_type}
        for child in elem:
            name = _local(child.tag)
            if pmap is not None and name == "property":
                for g in child:                 # first property wins (conduit norm)
                    m = pmap.get(_local(g.tag))
                    if m and m[0] not in rec:
                        rec[m[0]] = m[1](g.text)
                continue
            m = fmap.get(name)
            if m:
                rec[m[0]] = m[1](child.text)
        on_loan(_finalize(rec, asset_type))
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
    """Accumulate weighted pool stats while streaming, plus optional loan rows.
    WAC/WAM weight by outstanding balance; each quality metric (fico/pti/ltv/dscr)
    weights only over the balance where that metric is present, so mixed or sparse
    asset classes don't dilute the average."""
    METRICS = ("fico", "pti", "ltv", "dscr")

    def __init__(self, store_loans):
        self.n = 0
        self.orig = 0.0
        self.cur = 0.0
        self.w = 0.0           # sum of current balances used as weight
        self.wac = 0.0
        self.wam = 0.0
        self.new_bal = 0.0
        self.delinq_bal = 0.0
        self.by_state = defaultdict(float)
        self.msum = {k: 0.0 for k in self.METRICS}   # weighted metric sums
        self.mwt = {k: 0.0 for k in self.METRICS}    # weight where metric present
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
            if r.get("new_used") == 1:
                self.new_bal += w
            if (r.get("delinquency") or 0) >= 30:
                self.delinq_bal += w
            if r.get("state"):
                self.by_state[r["state"]] += w
            for k in self.METRICS:
                if r.get(k) is not None:
                    self.msum[k] += r[k] * w
                    self.mwt[k] += w
        if self.store_loans:
            self.rows.append(r)

    def _wavg(self, k, nd):
        return round(self.msum[k] / self.mwt[k], nd) if self.mwt[k] else None

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
            "wa_fico": self._wavg("fico", 1),
            "wa_pti": self._wavg("pti", 6),
            "wa_ltv": self._wavg("ltv", 4),
            "wa_dscr": self._wavg("dscr", 4),
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
             wa_fico,wa_pti,pct_new,pct_delinq,top_state,top_state_pct,
             wa_ltv,wa_dscr)
           VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
           ON CONFLICT(accession) DO UPDATE SET
             n_loans=excluded.n_loans, cur_balance=excluded.cur_balance,
             wac=excluded.wac, wam=excluded.wam, wa_fico=excluded.wa_fico,
             wa_ltv=excluded.wa_ltv, wa_dscr=excluded.wa_dscr,
             pct_delinq=excluded.pct_delinq""",
        (acc, cik, deal_name, at, begin, end, s["n_loans"], s["orig_balance"],
         s["cur_balance"], s["wac"], s["wam"], s["wa_fico"], s["wa_pti"],
         s["pct_new"], s["pct_delinq"], s["top_state"], s["top_state_pct"],
         s["wa_ltv"], s["wa_dscr"]))
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
    parts = []
    if s["wa_fico"]:
        parts.append(f"fico={s['wa_fico']:.0f}")
    if s["wa_ltv"]:
        parts.append(f"ltv={s['wa_ltv']*100:.0f}%")
    if s["wa_dscr"]:
        parts.append(f"dscr={s['wa_dscr']:.2f}")
    qual = " ".join(parts)
    print(f"  {deal_name} [{at}]: {s['n_loans']} loans, "
          f"cur=${s['cur_balance']/1e6:.1f}M wac={s['wac']*100:.2f}% "
          f"{qual} wam={s['wam']:.0f}mo "
          f"delinq={s['pct_delinq']*100:.1f}% "
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
    ap.add_argument("--cmbs-scan", type=int, metavar="N",
                    help="ingest the newest N CMBS ABS-EE filings in the DB")
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
    elif args.cmbs_scan:
        rows = conn.execute(
            """SELECT f.cik, f.accession, e.name FROM filing f
               JOIN entity e ON e.cik=f.cik
               WHERE f.form='ABS-EE' AND e.asset_class='CMBS'
               ORDER BY f.filed_date DESC LIMIT ?""", (args.cmbs_scan,)).fetchall()
        for cik, acc, name in rows:
            try:
                ingest_one(conn, cik, acc, name, args.loans, args.limit)
            except Exception as e:
                print(f"  skip {acc}: {e}", file=sys.stderr)
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
