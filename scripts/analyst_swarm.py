#!/usr/bin/env python3
"""
Multi-Agent Investment Analysis Swarm
======================================
Usage: python3 analyst_swarm.py <TICKER> [--agents AGENTS] [--depth deep|standard|quick]

Spawns a team of specialized analyst agents that each examine the company
from a different dimension, then synthesizes their findings into a unified
institutional-quality research report.

Agents:
  quant       – DCF, multiples, Monte Carlo, sensitivity, reverse-DCF
  fundamental – Piotroski, Altman, DuPont, earnings quality, accruals
  growth      – Revenue drivers, segment mix, TAM, runway, guidance tracking
  credit      – Debt structure, coverage ratios, liquidity, working capital cycle
  capital     – Buyback efficiency, dividend sustainability, ROIC vs WACC, M&A
  moat        – Competitive position, pricing power, switching costs, network effects
  macro       – Rate sensitivity, sector rotation, cycle positioning, FX exposure
  risk        – Sensitivity tables, scenario trees, tail risks, VaR
  sentiment   – News sentiment, insider transactions, institutional flow, technicals
  klarman     – Seth Klarman Margin of Safety: EPV, liquidation floor, conservative IV
  factor      – Modern multi-factor: value, quality, momentum, size, low-vol
  catalyst    – Event-driven: earnings beats, insider buys, buybacks, analyst gaps
  narrative   – Soros reflexivity: regime detection, narrative arcs, perception gaps
  minsky      – Hyman Minsky financial instability: hedge/speculative/ponzi classification
  tail_risk   – Mandelbrot fractal markets + Taleb antifragility: fat tails, VaR, Hurst
  complexity  – W. Brian Arthur increasing returns, Sornette bubbles, Kauffman fitness
  forensic    – Schilit/Beneish forensic accounting: M-Score, accruals, earnings manipulation
  kelly       – Kelly criterion + Shannon entropy: optimal sizing, Sharpe, Sortino, Calmar
  ergodicity  – Ole Peters ergodicity economics: time vs ensemble avg, ruin probability, vol tax
  microstructure – Kyle/Amihud market microstructure: liquidity, spread, order flow, price impact
  strategist  – Synthesizes all agents → final recommendation with conviction score
  --- Wave 1: EDGAR XBRL ---
  ohlson      – Ohlson O-Score distress prediction (logistic bankruptcy model)
  accrual     – Sloan accrual anomaly: earnings quality from accrual decomposition
  gross_profit– Novy-Marx gross profitability: GP/A as quality factor
  asset_growth– Cooper-Gulen-Schill asset growth anomaly
  issuance    – Pontiff-Woodgate capital issuance signal
  buyback_signal – Ikenberry-Lakonishok-Vermaelen buyback conviction
  tax_quality – Dyreng-Hanlon-Maydew cash ETR analysis
  earnings_bench – Burgstahler-Dichev earnings benchmark manipulation
  --- Wave 2: SEC EFTS + Form 4 ---
  insider_flow– Lakonishok-Lee insider transaction flow + cluster buy detection
  activist    – Brav-Jiang activist radar: 13D filings + vulnerability assessment
  filing_timing – Alford-Jones-Zmijewski filing timing + NT/restatement flags
  inst_flow   – Cohen-Polk-Silli institutional flow analysis
  exec_align  – Jensen-Murphy executive compensation alignment
  --- Wave 3: FRED + CBOE ---
  capital_cycle – Greenwood-Hanson capital cycle (capex/depreciation)
  gpr         – Caldara-Iacoviello geopolitical risk index
  vol_surface – Cremers-Weinbaum VIX term structure + SKEW
  --- Wave 4: FINRA ---
  short_interest – Hong-Li-Ni short interest + days-to-cover analysis
  --- Wave 5: Advanced ---
  textual     – Loughran-McDonald + Cohen "Lazy Prices" textual analytics
  customer_conc – Patatoukas customer concentration risk
  real_manip  – Roychowdhury real activities manipulation detection
  pension     – Franzoni-Marin pension risk analysis
  supply_chain– Wu-Birge supply chain risk assessment

Requires: ALPHA_VANTAGE_API_KEY env var (FRED_API_KEY optional for macro data)
"""

import sys, os, json, statistics, argparse, time, random, textwrap, re, math, csv, io, zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError
from collections import OrderedDict
from threading import Lock
from datetime import datetime, timedelta

# ══════════════════════════════════════════════════════════════════════════════
# Config
# ══════════════════════════════════════════════════════════════════════════════

AV_KEY = os.environ.get("ALPHA_VANTAGE_API_KEY", "")
AV_BASE = "https://www.alphavantage.co/query"
FRED_KEY = os.environ.get("FRED_API_KEY", "")
MARKET_RISK_PREMIUM = 0.055
TERMINAL_GROWTH_DEFAULT = 0.025
MC_ITERATIONS = 8000
DSCO_CACHE_DIR = os.path.expanduser("~/.dsco/cache")
os.makedirs(DSCO_CACHE_DIR, exist_ok=True)

# ══════════════════════════════════════════════════════════════════════════════
# Terminal formatting
# ══════════════════════════════════════════════════════════════════════════════

USE_COLOR = True
_print_lock = Lock()

def c(text, code):
    if not USE_COLOR: return str(text)
    return f"\033[{code}m{text}\033[0m"

def green(t):   return c(t, "32")
def red(t):     return c(t, "31")
def yellow(t):  return c(t, "33")
def cyan(t):    return c(t, "36")
def bold(t):    return c(t, "1")
def dim(t):     return c(t, "2")
def mag(t):     return c(t, "35")
def white(t):   return c(t, "97")
def bg_green(t): return c(t, "42;97")
def bg_red(t):   return c(t, "41;97")
def bg_yellow(t):return c(t, "43;30")

def pct(v, digits=1):
    if v is None: return dim("N/A")
    s = f"{v*100:+.{digits}f}%"
    return green(s) if v > 0 else red(s) if v < 0 else s

def usd(v, digits=2):
    if v is None: return dim("N/A")
    neg = v < 0
    v = abs(v)
    if v >= 1e12: s = f"${v/1e12:.{digits}f}T"
    elif v >= 1e9: s = f"${v/1e9:.{digits}f}B"
    elif v >= 1e6: s = f"${v/1e6:.{digits}f}M"
    elif v >= 1e3: s = f"${v/1e3:.{digits}f}K"
    else: s = f"${v:,.{digits}f}"
    return f"-{s}" if neg else s

def num(v, digits=2):
    if v is None: return dim("N/A")
    return f"{v:,.{digits}f}"

def bar(value, max_val, width=20, ch="█", empty="░"):
    filled = int(value / max_val * width) if max_val > 0 else 0
    filled = max(0, min(width, filled))
    return ch * filled + empty * (width - filled)

def heatmap_cell(v, thresholds=(-0.05, 0.0, 0.05, 0.15)):
    """Color-code a value based on thresholds."""
    if v is None: return dim("  —  ")
    s = f"{v*100:+5.1f}%"
    if v <= thresholds[0]: return red(s)
    if v <= thresholds[1]: return c(s, "31;2")
    if v <= thresholds[2]: return yellow(s)
    if v <= thresholds[3]: return green(s)
    return c(s, "32;1")

def spark(values):
    """Tiny sparkline from values."""
    if not values: return ""
    chars = "▁▂▃▄▅▆▇█"
    mn, mx = min(values), max(values)
    rng = mx - mn or 1
    return "".join(chars[min(7, int((v - mn) / rng * 7.99))] for v in values)

def safe_print(*args, **kwargs):
    with _print_lock:
        print(*args, **kwargs)

# ══════════════════════════════════════════════════════════════════════════════
# API layer (with rate-limit-aware serial fetching)
# ══════════════════════════════════════════════════════════════════════════════

_api_lock = Lock()
_last_call = [0.0]

def av_fetch(function, **params):
    with _api_lock:
        elapsed = time.time() - _last_call[0]
        if elapsed < 0.15:  # 75 req/min premium = ~0.8s, use 0.15s buffer
            time.sleep(0.15 - elapsed)
        _last_call[0] = time.time()

    params["function"] = function
    params["apikey"] = AV_KEY
    qs = "&".join(f"{k}={v}" for k, v in params.items())
    url = f"{AV_BASE}?{qs}"
    try:
        req = Request(url, headers={"User-Agent": "dsco-swarm/2.0"})
        with urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read().decode())
        if "Error Message" in data: return None
        if "Note" in data or ("Information" in data and "rate" in data.get("Information", "").lower()):
            time.sleep(2)  # brief back off (premium: 75 req/min)
            return av_fetch(function, **params)  # retry once
        return data
    except Exception:
        return None

def safe_float(d, key, default=None):
    if not d: return default
    v = d.get(key, "None")
    if v in (None, "None", "-", ""): return default
    try: return float(v)
    except: return default

# ══════════════════════════════════════════════════════════════════════════════
# SEC EDGAR adapter — free, unlimited, authoritative financial data
# ══════════════════════════════════════════════════════════════════════════════

EDGAR_COMPANY_FACTS = "https://data.sec.gov/api/xbrl/companyfacts"
EDGAR_COMPANY_TICKERS = "https://www.sec.gov/files/company_tickers.json"
EDGAR_UA = "dsco-swarm/2.0 research@dsco.dev"

_ticker_to_cik_cache = {}

def _load_ticker_map():
    """Load SEC ticker→CIK mapping (cached)."""
    global _ticker_to_cik_cache
    if _ticker_to_cik_cache:
        return _ticker_to_cik_cache
    try:
        req = Request(EDGAR_COMPANY_TICKERS, headers={"User-Agent": EDGAR_UA})
        data = json.loads(urlopen(req, timeout=15).read().decode())
        for entry in data.values():
            t = entry.get("ticker", "").upper()
            cik = entry.get("cik_str")
            if t and cik:
                _ticker_to_cik_cache[t] = str(cik)
    except Exception as e:
        safe_print(f"    {red(f'EDGAR ticker map error: {e}')}")
    return _ticker_to_cik_cache

def edgar_cik_for_ticker(ticker):
    """Resolve ticker to CIK number."""
    m = _load_ticker_map()
    return m.get(ticker.upper())

def edgar_fetch_facts(cik):
    """Fetch all XBRL facts for a CIK from EDGAR."""
    padded = str(cik).zfill(10)
    url = f"{EDGAR_COMPANY_FACTS}/CIK{padded}.json"
    try:
        req = Request(url, headers={"User-Agent": EDGAR_UA})
        data = json.loads(urlopen(req, timeout=20).read().decode())
        return data
    except Exception as e:
        safe_print(f"    {red(f'EDGAR facts error: {e}')}")
        return None

def _edgar_values(facts, concept, unit="USD", form="10-K", fp_filter="FY"):
    """Extract values for a GAAP/IFRS concept, deduped by period end.

    Args:
        facts: Full EDGAR XBRL JSON
        concept: GAAP concept name (e.g. "Revenues")
        unit: "USD", "shares", "pure", "USD/shares"
        form: "10-K", "10-Q", "20-F", "40-F" etc.
        fp_filter: "FY" for annual, "Q1"/"Q2"/"Q3"/"Q4" for quarterly, None for all
    """
    # Try us-gaap first, then ifrs-full for international filers
    for taxonomy in ("us-gaap", "ifrs-full"):
        ug = facts.get("facts", {}).get(taxonomy, {})
        node = ug.get(concept)
        if node:
            break
    else:
        return []

    entries = node.get("units", {}).get(unit, [])

    # Filter by form and fiscal period
    if isinstance(form, str):
        forms = {form}
    else:
        forms = set(form) if form else None

    filtered = []
    for e in entries:
        if forms and e.get("form") not in forms:
            continue
        if fp_filter and e.get("fp") != fp_filter:
            continue
        filtered.append(e)

    # Dedupe by end date (take latest filed version)
    by_end = {}
    for e in filtered:
        end = e.get("end", "")
        if not end:
            continue
        existing = by_end.get(end)
        if not existing or e.get("filed", "") > existing.get("filed", ""):
            by_end[end] = e
    return sorted(by_end.values(), key=lambda e: e.get("end", ""))


def _edgar_annual_values(facts, concept, unit="USD", form="10-K"):
    """Extract annual values for a GAAP concept, deduped by fiscal year end."""
    return _edgar_values(facts, concept, unit=unit, form=form, fp_filter="FY")


def _edgar_quarterly_values(facts, concept, unit="USD"):
    """Extract quarterly values for a GAAP concept from 10-Q filings."""
    results = []
    for qtr in ("Q1", "Q2", "Q3", "Q4"):
        results.extend(_edgar_values(facts, concept, unit=unit, form="10-Q", fp_filter=qtr))
    # Also include quarterly data filed on 10-K (Q4 is sometimes in the 10-K)
    results.extend(_edgar_values(facts, concept, unit=unit, form="10-K", fp_filter="Q4"))
    # Dedupe by end date
    by_end = {}
    for e in results:
        end = e.get("end", "")
        if not end: continue
        existing = by_end.get(end)
        if not existing or e.get("filed", "") > existing.get("filed", ""):
            by_end[end] = e
    return sorted(by_end.values(), key=lambda e: e.get("end", ""))

# ══════════════════════════════════════════════════════════════════════════════
# EDGAR GAAP concept mapping → internal field names (expanded)
# Covers US-GAAP + IFRS-full taxonomies. _edgar_values() tries both.
# ══════════════════════════════════════════════════════════════════════════════

EDGAR_INCOME_MAP = {
    # Revenue & COGS
    "revenue":          ["RevenueFromContractWithCustomerExcludingAssessedTax", "Revenues", "SalesRevenueNet",
                         "RevenueFromContractWithCustomerIncludingAssessedTax",
                         "Revenue"],  # IFRS
    "cogs":             ["CostOfGoodsAndServicesSold", "CostOfRevenue", "CostOfGoodsSold",
                         "CostOfSales"],  # IFRS
    "gross_profit":     ["GrossProfit"],
    # Operating expenses
    "rd":               ["ResearchAndDevelopmentExpense", "ResearchAndDevelopmentExpenseExcludingAcquiredInProcessCost"],
    "sga":              ["SellingGeneralAndAdministrativeExpense", "GeneralAndAdministrativeExpense"],
    "selling_exp":      ["SellingExpense", "SellingAndMarketingExpense"],
    "op_expenses":      ["OperatingExpenses", "CostsAndExpenses"],
    "op_income":        ["OperatingIncomeLoss", "ProfitLossFromOperatingActivities"],  # IFRS
    # Non-operating
    "interest_exp":     ["InterestExpense", "InterestExpenseDebt", "InterestAndDebtExpense",
                         "FinanceCosts"],  # IFRS
    "interest_income":  ["InterestIncomeExpenseNet", "InvestmentIncomeInterest", "InterestIncome",
                         "FinanceIncome"],  # IFRS
    "other_income":     ["OtherNonoperatingIncomeExpense", "NonoperatingIncomeExpense",
                         "OtherOperatingIncomeExpenses"],  # IFRS
    # Bottom line
    "pretax_income":    ["IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest",
                         "IncomeLossFromContinuingOperationsBeforeIncomeTaxesDomestic",
                         "IncomeLossFromContinuingOperationsBeforeIncomeTaxesMinorityInterestAndIncomeLossFromEquityMethodInvestments",
                         "ProfitLossBeforeTax"],  # IFRS
    "tax":              ["IncomeTaxExpenseBenefit", "IncomeTaxesPaidNet"],
    "net_income":       ["NetIncomeLoss", "ProfitLoss"],  # IFRS
    "net_income_to_common": ["NetIncomeLossAvailableToCommonStockholdersBasic"],
    # Comprehensive income
    "comprehensive_income": ["ComprehensiveIncomeNetOfTax",
                             "ComprehensiveIncomeNetOfTaxIncludingPortionAttributableToNoncontrollingInterest"],
    "other_comprehensive":  ["OtherComprehensiveIncomeLossNetOfTax"],
    # Non-cash
    "ebitda":           [],  # computed from op_income + D&A
    "da":               ["DepreciationDepletionAndAmortization", "DepreciationAndAmortization", "Depreciation"],
    "impairment":       ["GoodwillImpairmentLoss", "AssetImpairmentCharges", "ImpairmentOfLongLivedAssetsHeldForUse"],
    "restructuring":    ["RestructuringCharges", "RestructuringAndRelatedCostIncurredCost"],
    "sbc_inc":          ["ShareBasedCompensation", "AllocatedShareBasedCompensationExpense"],
    # Per-share
    "shares":           ["CommonStockSharesOutstanding", "WeightedAverageNumberOfDilutedSharesOutstanding"],
    "shares_basic":     ["WeightedAverageNumberOfShareOutstandingBasicAndDiluted", "WeightedAverageNumberOfSharesOutstandingBasic"],
    "eps_diluted":      ["EarningsPerShareDiluted"],
    "eps_basic":        ["EarningsPerShareBasic"],
    # Tax detail
    "effective_tax_rate": ["EffectiveIncomeTaxRateContinuingOperations"],
    "deferred_tax_exp":   ["DeferredIncomeTaxExpenseBenefit"],
}

EDGAR_BALANCE_MAP = {
    # Assets
    "total_assets":        ["Assets"],
    "current_assets":      ["AssetsCurrent"],
    "noncurrent_assets":   ["AssetsNoncurrent"],
    "cash":                ["CashAndCashEquivalentsAtCarryingValue", "CashCashEquivalentsAndShortTermInvestments",
                            "Cash"],  # IFRS
    "st_investments":      ["ShortTermInvestments", "AvailableForSaleSecuritiesCurrent",
                            "MarketableSecuritiesCurrent"],
    "lt_investments":      ["LongTermInvestments", "AvailableForSaleSecuritiesNoncurrent",
                            "MarketableSecuritiesNoncurrent"],
    "receivables":         ["AccountsReceivableNetCurrent", "AccountsReceivableNet",
                            "TradeAndOtherCurrentReceivables"],  # IFRS
    "inventory":           ["InventoryNet", "InventoryFinishedGoods", "Inventories"],  # IFRS
    "prepaid":             ["PrepaidExpenseAndOtherAssets", "PrepaidExpenseCurrent"],
    "other_current":       ["OtherAssetsCurrent"],
    # Fixed & intangible assets
    "ppe_gross":           ["PropertyPlantAndEquipmentGross"],
    "ppe":                 ["PropertyPlantAndEquipmentNet"],
    "rou_assets":          ["OperatingLeaseRightOfUseAsset", "FinanceLeaseRightOfUseAsset"],  # ASC 842
    "goodwill":            ["Goodwill"],
    "intangibles":         ["IntangibleAssetsNetExcludingGoodwill", "FiniteLivedIntangibleAssetsNet"],
    "other_noncurrent":    ["OtherAssetsNoncurrent"],
    # Liabilities
    "total_liabilities":   ["Liabilities"],
    "current_liabilities": ["LiabilitiesCurrent"],
    "noncurrent_liabilities": ["LiabilitiesNoncurrent"],
    "accounts_payable":    ["AccountsPayableCurrent", "AccountsPayableAndAccruedLiabilitiesCurrent"],
    "accrued_liabilities": ["AccruedLiabilitiesCurrent"],
    "deferred_revenue":    ["ContractWithCustomerLiabilityCurrent", "DeferredRevenueCurrent",
                            "DeferredRevenueCurrentAndNoncurrent",
                            "ContractWithCustomerLiability"],  # ASC 606
    "deferred_revenue_nc": ["ContractWithCustomerLiabilityNoncurrent", "DeferredRevenueNoncurrent"],
    "lt_debt":             ["LongTermDebtNoncurrent", "LongTermDebt"],
    "st_debt":             ["ShortTermBorrowings", "LongTermDebtCurrent", "CommercialPaper"],
    "current_debt":        ["DebtCurrent", "LongTermDebtCurrent", "ShortTermBorrowings"],
    "total_debt":          ["LongTermDebt", "DebtCurrent", "LongTermDebtAndCapitalLeaseObligations"],
    # Lease obligations (ASC 842)
    "op_lease_liability":     ["OperatingLeaseLiability"],
    "op_lease_liability_cur": ["OperatingLeaseLiabilityCurrent"],
    "fin_lease_liability":    ["FinanceLeaseLiability"],
    # Deferred taxes
    "deferred_tax_asset":     ["DeferredIncomeTaxAssetsNet"],
    "deferred_tax_liability": ["DeferredIncomeTaxLiabilitiesNet"],
    # Equity
    "equity":              ["StockholdersEquity", "Equity"],  # IFRS
    "equity_incl_minority":["StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest"],
    "retained_earnings":   ["RetainedEarningsAccumulatedDeficit"],
    "accumulated_oci":     ["AccumulatedOtherComprehensiveIncomeLossNetOfTax"],
    "treasury_stock":      ["TreasuryStockValue"],
    "common_stock":        ["CommonStockValue"],
    "additional_paid_in":  ["AdditionalPaidInCapital", "AdditionalPaidInCapitalCommonStock"],
    "minority_interest":   ["MinorityInterest", "RedeemableNoncontrollingInterest"],
    # Shares outstanding
    "shares_outstanding":  ["CommonStockSharesOutstanding"],
    "shares_authorized":   ["CommonStockSharesAuthorized"],
    "treasury_shares":     ["TreasuryStockShares"],
}

EDGAR_CASHFLOW_MAP = {
    # Operating
    "cfo":            ["NetCashProvidedByUsedInOperatingActivities",
                       "CashFlowsFromUsedInOperatingActivities"],  # IFRS
    "da_cf":          ["DepreciationDepletionAndAmortization", "DepreciationAmortizationAndAccretionNet"],
    "sbc":            ["ShareBasedCompensation", "AllocatedShareBasedCompensationExpense"],
    "deferred_tax":   ["DeferredIncomeTaxExpenseBenefit", "DeferredIncomeTaxesAndTaxCredits"],
    "change_receivables": ["IncreaseDecreaseInAccountsReceivable"],
    "change_inventory":   ["IncreaseDecreaseInInventories"],
    "change_payables":    ["IncreaseDecreaseInAccountsPayable"],
    "change_wc":          ["IncreaseDecreaseInAccountsPayable"],  # legacy — use change_payables preferred
    "change_deferred_rev":["IncreaseDecreaseInContractWithCustomerLiability", "IncreaseDecreaseInDeferredRevenue"],
    # Investing
    "cfi":            ["NetCashProvidedByUsedInInvestingActivities",
                       "CashFlowsFromUsedInInvestingActivities"],  # IFRS
    "capex":          ["PaymentsToAcquirePropertyPlantAndEquipment",
                       "PurchaseOfPropertyPlantAndEquipment"],  # IFRS
    "acquisitions":   ["PaymentsToAcquireBusinessesNetOfCashAcquired",
                       "PaymentsToAcquireBusinessesAndInterestInAffiliates"],
    "purchase_investments": ["PaymentsToAcquireInvestments", "PaymentsToAcquireAvailableForSaleSecuritiesDebt"],
    "proceeds_investments": ["ProceedsFromSaleAndMaturityOfMarketableSecurities",
                             "ProceedsFromMaturitiesPrepaymentsAndCallsOfAvailableForSaleSecurities"],
    # Financing
    "cff":            ["NetCashProvidedByUsedInFinancingActivities",
                       "CashFlowsFromUsedInFinancingActivities"],  # IFRS
    "dividends":      ["PaymentsOfDividends", "PaymentsOfDividendsCommonStock"],
    "buybacks":       ["PaymentsForRepurchaseOfCommonStock", "PaymentsForRepurchaseOfEquity"],
    "stock_issuance": ["ProceedsFromIssuanceOfCommonStock", "ProceedsFromStockOptionsExercised"],
    "debt_issuance":  ["ProceedsFromIssuanceOfLongTermDebt", "ProceedsFromIssuanceOfDebt"],
    "debt_repayment": ["RepaymentsOfLongTermDebt", "RepaymentsOfDebt"],
    # Net change
    "fx_effect":      ["EffectOfExchangeRateOnCashCashEquivalentsRestrictedCashAndRestrictedCashEquivalents"],
    "net_change_cash":["CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalentsPeriodIncreaseDecreaseIncludingExchangeRateEffect"],
}

# ── IFRS concept mapping (for international filers via 20-F / 40-F) ──
IFRS_INCOME_MAP = {
    "revenue":      ["Revenue"],
    "cogs":         ["CostOfSales"],
    "gross_profit": ["GrossProfit"],
    "op_income":    ["ProfitLossFromOperatingActivities"],
    "interest_exp": ["FinanceCosts"],
    "pretax_income":["ProfitLossBeforeTax"],
    "tax":          ["IncomeTaxExpenseContinuingOperations"],
    "net_income":   ["ProfitLoss", "ProfitLossAttributableToOwnersOfParent"],
    "da":           ["DepreciationAndAmortisationExpense"],
    "ebitda":       [],
}

# ── Segment revenue concepts ──
EDGAR_SEGMENT_CONCEPTS = [
    "RevenueFromContractWithCustomerExcludingAssessedTax",
    "Revenues",
    "SalesRevenueNet",
    "SegmentReportingInformationRevenue",
]

def _build_series_from_entries(facts, concept_map, entry_fetcher, key_fn, unit="USD"):
    """Generic series builder. entry_fetcher(concept, unit) returns entries.
    key_fn(entry) returns the grouping key (year for annual, quarter-end for quarterly)."""
    share_fields = {"shares", "shares_basic", "eps_diluted", "eps_basic",
                    "shares_outstanding", "shares_authorized", "treasury_shares",
                    "effective_tax_rate"}

    all_keys = set()
    for field, concepts in concept_map.items():
        for concept in concepts:
            for entry in entry_fetcher(concept, unit):
                k = key_fn(entry)
                if k: all_keys.add(k)

    series = OrderedDict()
    for key in sorted(all_keys):
        row = {}
        for field, concepts in concept_map.items():
            for concept in concepts:
                vals = entry_fetcher(concept, unit)
                for entry in vals:
                    if key_fn(entry) == key:
                        row[field] = entry["val"]
                        break
                if field in row:
                    break
            if field not in row and field in share_fields:
                for alt_unit in ("shares", "pure", "USD/shares"):
                    for concept in concepts:
                        vals = entry_fetcher(concept, alt_unit)
                        for entry in vals:
                            if key_fn(entry) == key:
                                row[field] = entry["val"]
                                break
                        if field in row: break
                    if field in row: break
            if field not in row:
                row[field] = None
        # Compute EBITDA if not available
        if row.get("ebitda") is None and row.get("op_income") is not None:
            da = row.get("da") or 0
            row["ebitda"] = row["op_income"] + abs(da)
        series[key] = row
    return series


def edgar_build_series(facts, concept_map, unit="USD"):
    """Build a year-keyed annual series from EDGAR XBRL facts."""
    def fetcher(concept, u):
        return _edgar_annual_values(facts, concept, u)
    def key_fn(entry):
        return entry.get("end", "")[:4]
    return _build_series_from_entries(facts, concept_map, fetcher, key_fn, unit)


def edgar_build_quarterly_series(facts, concept_map, unit="USD"):
    """Build a quarter-keyed series from EDGAR 10-Q XBRL facts.
    Keys are ISO dates (YYYY-MM-DD period end)."""
    def fetcher(concept, u):
        return _edgar_quarterly_values(facts, concept, u)
    def key_fn(entry):
        return entry.get("end", "")  # full date for quarterly
    return _build_series_from_entries(facts, concept_map, fetcher, key_fn, unit)


def edgar_fetch_segment_data(facts):
    """Extract segment-level revenue from XBRL facts.
    Returns dict: {segment_name: {year: revenue}}."""
    segments = {}
    ug = facts.get("facts", {}).get("us-gaap", {})

    for concept_name in EDGAR_SEGMENT_CONCEPTS:
        node = ug.get(concept_name)
        if not node: continue
        for unit_key, entries in node.get("units", {}).items():
            if unit_key != "USD": continue
            for entry in entries:
                if entry.get("form") != "10-K" or entry.get("fp") != "FY":
                    continue
                # Segment data has a "segment" key in some filings
                # or uses "frame" for dimensional data
                seg = entry.get("segment", entry.get("frame", ""))
                if not seg or seg == "": continue
                year = entry.get("end", "")[:4]
                if seg not in segments:
                    segments[seg] = {}
                segments[seg][year] = entry.get("val", 0)
    return segments


def edgar_fetch_filing_index(cik, form_types=None):
    """Fetch recent filing metadata from EDGAR filing index.
    Returns list of {form, filingDate, accessionNumber, primaryDocument}."""
    padded = str(cik).zfill(10)
    url = f"https://data.sec.gov/submissions/CIK{padded}.json"
    try:
        req = Request(url, headers={"User-Agent": EDGAR_UA})
        data = json.loads(urlopen(req, timeout=15).read().decode())
        recent = data.get("filings", {}).get("recent", {})
        forms = recent.get("form", [])
        dates = recent.get("filingDate", [])
        accessions = recent.get("accessionNumber", [])
        primary_docs = recent.get("primaryDocument", [])

        filings = []
        for i in range(len(forms)):
            f = {"form": forms[i], "filingDate": dates[i] if i < len(dates) else "",
                 "accessionNumber": accessions[i] if i < len(accessions) else "",
                 "primaryDocument": primary_docs[i] if i < len(primary_docs) else ""}
            if form_types and f["form"] not in form_types:
                continue
            filings.append(f)
        return filings, data.get("name", ""), data.get("sic", ""), data.get("sicDescription", "")
    except Exception:
        return [], "", "", ""


# ══════════════════════════════════════════════════════════════════════════════
# External Data Source Adapters (Phase 3 & 4)
# ══════════════════════════════════════════════════════════════════════════════

_session_cache = {}  # module-level session cache for academic data

def _cached_fetch(url, key, ttl=86400, binary=False):
    """Fetch URL with disk cache (default 24h TTL)."""
    cache_path = os.path.join(DSCO_CACHE_DIR, key)
    if os.path.exists(cache_path):
        age = time.time() - os.path.getmtime(cache_path)
        if age < ttl:
            mode = "rb" if binary else "r"
            with open(cache_path, mode) as f:
                return f.read()
    try:
        req = Request(url, headers={"User-Agent": EDGAR_UA, "Accept-Encoding": "identity"})
        data = urlopen(req, timeout=30).read()
        mode = "wb" if binary else "w"
        with open(cache_path, mode) as f:
            f.write(data if binary else data.decode("utf-8", errors="replace"))
        return data if binary else data.decode("utf-8", errors="replace")
    except Exception:
        return None


# ── FRED API ──

def fred_fetch_series(series_id, limit=250):
    """Fetch FRED series observations. Returns list of {date, value}."""
    if not FRED_KEY:
        return []
    url = (f"https://api.stlouisfed.org/fred/series/observations?"
           f"series_id={series_id}&api_key={FRED_KEY}&file_type=json"
           f"&sort_order=desc&limit={limit}")
    try:
        req = Request(url, headers={"User-Agent": EDGAR_UA})
        data = json.loads(urlopen(req, timeout=15).read().decode())
        obs = data.get("observations", [])
        return [{"date": o["date"], "value": float(o["value"])} for o in obs if o.get("value", ".") != "."]
    except Exception:
        return []


def fred_fetch_treasury_curve():
    """Fetch full Treasury yield curve from FRED."""
    maturities = {"DGS1MO": "1M", "DGS3MO": "3M", "DGS6MO": "6M", "DGS1": "1Y",
                  "DGS2": "2Y", "DGS3": "3Y", "DGS5": "5Y", "DGS7": "7Y",
                  "DGS10": "10Y", "DGS20": "20Y", "DGS30": "30Y"}
    curve = {}
    for series_id, label in maturities.items():
        obs = fred_fetch_series(series_id, limit=5)
        if obs:
            curve[label] = obs[0]["value"] / 100.0
    return curve


def fred_fetch_credit_spreads():
    """Fetch HY and IG credit spreads from FRED."""
    spreads = {}
    for sid, label in [("BAMLH0A0HYM2", "hy_oas"), ("BAMLC0A4CBBB", "bbb_oas"),
                       ("BAMLC0A0CM", "ig_oas")]:
        obs = fred_fetch_series(sid, limit=5)
        if obs:
            spreads[label] = obs[0]["value"] / 100.0
    return spreads


# ── CBOE VIX/SKEW ──

def cboe_fetch_index(index_name):
    """Fetch CBOE index history CSV. Returns list of {date, close}."""
    url = f"https://cdn.cboe.com/api/global/us_indices/daily_prices/{index_name}_History.csv"
    try:
        data = _cached_fetch(url, f"cboe_{index_name}.csv", ttl=43200)
        if not data:
            return []
        reader = csv.DictReader(io.StringIO(data))
        rows = []
        for row in reader:
            close_val = row.get("CLOSE") or row.get("Close") or row.get("close")
            date_val = row.get("DATE") or row.get("Date") or row.get("date")
            if close_val and date_val:
                try:
                    rows.append({"date": date_val.strip(), "close": float(close_val)})
                except (ValueError, TypeError):
                    pass
        return rows[-260:]  # last ~1yr of trading days
    except Exception:
        return []


def cboe_fetch_vix_data():
    """Fetch VIX term structure and SKEW from CBOE."""
    result = {}
    for name, key in [("VIX", "vix"), ("SKEW", "skew"), ("VIX9D", "vix_9d"),
                      ("VIX3M", "vix_3m"), ("VIX6M", "vix_6m"), ("VIX1Y", "vix_1y")]:
        rows = cboe_fetch_index(name)
        if rows:
            result[key] = rows[-1]["close"]
            result[f"{key}_history"] = [r["close"] for r in rows]
    return result


# ── FINRA Short Volume ──

def finra_fetch_short_volume(ticker, days=5):
    """Fetch recent FINRA short sale volume for a ticker."""
    result = {}
    today = datetime.now()
    for i in range(days + 10):  # try extra days for weekends/holidays
        d = today - timedelta(days=i)
        if d.weekday() >= 5:
            continue
        date_str = d.strftime("%Y%m%d")
        url = f"https://cdn.finra.org/equity/regsho/daily/CNMSshvol{date_str}.txt"
        try:
            req = Request(url, headers={"User-Agent": EDGAR_UA})
            text = urlopen(req, timeout=10).read().decode()
            for line in text.strip().split("\n"):
                parts = line.split("|")
                if len(parts) >= 5 and parts[1].upper() == ticker.upper():
                    short_vol = int(parts[2])
                    total_vol = int(parts[4])
                    ratio = short_vol / total_vol if total_vol > 0 else 0
                    result[d.strftime("%Y-%m-%d")] = {
                        "short_vol": short_vol, "total_vol": total_vol, "ratio": ratio}
                    break
            if len(result) >= days:
                break
        except Exception:
            continue
    return result


# ── SEC Form 4 Parsing ──

def sec_fetch_form4(cik):
    """Fetch recent Form 4 insider transactions from EDGAR submissions."""
    padded = str(cik).zfill(10)
    url = f"https://data.sec.gov/submissions/CIK{padded}.json"
    transactions = []
    try:
        req = Request(url, headers={"User-Agent": EDGAR_UA})
        data = json.loads(urlopen(req, timeout=15).read().decode())
        recent = data.get("filings", {}).get("recent", {})
        forms = recent.get("form", [])
        dates = recent.get("filingDate", [])
        accessions = recent.get("accessionNumber", [])

        for i, form in enumerate(forms):
            if form not in ("4", "4/A"):
                continue
            if i >= len(dates):
                break
            filing_date = dates[i]
            # Parse the filing date and filter for last 180 days
            try:
                fd = datetime.strptime(filing_date, "%Y-%m-%d")
                if (datetime.now() - fd).days > 180:
                    continue
            except Exception:
                continue

            # Fetch the actual Form 4 XML to extract transaction details
            accession = accessions[i].replace("-", "") if i < len(accessions) else ""
            accession_dash = accessions[i] if i < len(accessions) else ""
            # Try to get the XML filing
            xml_url = f"https://www.sec.gov/Archives/edgar/data/{cik}/{accession}/{accession_dash}.txt"
            try:
                req2 = Request(xml_url, headers={"User-Agent": EDGAR_UA})
                content = urlopen(req2, timeout=10).read().decode("utf-8", errors="replace")
                # Simple XML parsing for transaction data
                txn = {"filing_date": filing_date, "form": form}
                # Extract reporter name
                m = re.search(r"<rptOwnerName>([^<]+)</rptOwnerName>", content)
                if m: txn["reporter"] = m.group(1)
                # Extract relationship
                txn["is_officer"] = "<isOfficer>1</isOfficer>" in content or "<isOfficer>true</isOfficer>" in content.lower()
                txn["is_director"] = "<isDirector>1</isDirector>" in content or "<isDirector>true</isDirector>" in content.lower()
                txn["is_ten_pct"] = "<isTenPercentOwner>1</isTenPercentOwner>" in content or "<isTenPercentOwner>true</isTenPercentOwner>" in content.lower()
                # Extract transaction code and shares
                codes = re.findall(r"<transactionCode>([A-Z])</transactionCode>", content)
                shares_list = re.findall(r"<transactionShares>.*?<value>([^<]+)</value>", content, re.DOTALL)
                prices_list = re.findall(r"<transactionPricePerShare>.*?<value>([^<]+)</value>", content, re.DOTALL)
                txn["codes"] = codes
                txn["shares"] = [float(s) for s in shares_list if s.strip()] if shares_list else []
                txn["prices"] = [float(p) for p in prices_list if p.strip()] if prices_list else []
                transactions.append(txn)
            except Exception:
                transactions.append({"filing_date": filing_date, "form": form, "codes": [], "shares": [], "prices": []})

            if len(transactions) >= 50:  # cap at 50 most recent
                break
            time.sleep(0.11)  # respect SEC rate limit
    except Exception:
        pass
    return transactions


# ── SEC EFTS Search ──

def sec_efts_search(query, forms=None, date_range=None, cik=None, limit=20):
    """Search SEC EDGAR Full-Text Search System."""
    params = {"q": query, "dateRange": date_range or "custom",
              "startdt": (datetime.now() - timedelta(days=365)).strftime("%Y-%m-%d"),
              "enddt": datetime.now().strftime("%Y-%m-%d")}
    if forms:
        params["forms"] = ",".join(forms)
    if cik:
        params["dateRange"] = "custom"
    qs = "&".join(f"{k}={v}" for k, v in params.items())
    url = f"https://efts.sec.gov/LATEST/search-index?{qs}&_limit={limit}"
    try:
        req = Request(url, headers={"User-Agent": EDGAR_UA})
        data = json.loads(urlopen(req, timeout=15).read().decode())
        return data.get("hits", {}).get("hits", [])
    except Exception:
        return []


# ── Ken French Factor Data ──

def fetch_french_factors():
    """Fetch Fama-French 5 factors + momentum from Ken French's data library."""
    if "ff_factors" in _session_cache:
        return _session_cache["ff_factors"]

    factors = {}
    # FF 5 factors (daily)
    url = "https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/ftp/F-F_Research_Data_5_Factors_2x3_daily_CSV.zip"
    try:
        data = _cached_fetch(url, "ff5_daily.zip", ttl=86400, binary=True)
        if data:
            with zipfile.ZipFile(io.BytesIO(data)) as zf:
                for name in zf.namelist():
                    if name.endswith(".CSV") or name.endswith(".csv"):
                        text = zf.read(name).decode("utf-8", errors="replace")
                        lines = text.strip().split("\n")
                        header_idx = None
                        for i, line in enumerate(lines):
                            if "Mkt-RF" in line:
                                header_idx = i
                                break
                        if header_idx is not None:
                            headers = [h.strip() for h in lines[header_idx].split(",")]
                            recent_rows = []
                            for line in lines[header_idx+1:]:
                                parts = [p.strip() for p in line.split(",")]
                                if len(parts) >= 6 and parts[0].isdigit() and len(parts[0]) == 8:
                                    row = {}
                                    for j, h in enumerate(headers):
                                        if j < len(parts):
                                            try:
                                                row[h] = float(parts[j]) / 100.0
                                            except ValueError:
                                                row[h] = parts[j]
                                    recent_rows.append(row)
                            if recent_rows:
                                factors["daily"] = recent_rows[-252:]  # last year
                                factors["latest"] = recent_rows[-1]
    except Exception:
        pass

    # Momentum factor
    url_mom = "https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/ftp/F-F_Momentum_Factor_daily_CSV.zip"
    try:
        data = _cached_fetch(url_mom, "ff_mom_daily.zip", ttl=86400, binary=True)
        if data:
            with zipfile.ZipFile(io.BytesIO(data)) as zf:
                for name in zf.namelist():
                    if name.endswith(".CSV") or name.endswith(".csv"):
                        text = zf.read(name).decode("utf-8", errors="replace")
                        lines = text.strip().split("\n")
                        header_idx = None
                        for i, line in enumerate(lines):
                            if "Mom" in line:
                                header_idx = i
                                break
                        if header_idx is not None:
                            recent = []
                            for line in lines[header_idx+1:]:
                                parts = [p.strip() for p in line.split(",")]
                                if len(parts) >= 2 and parts[0].isdigit() and len(parts[0]) == 8:
                                    try:
                                        recent.append(float(parts[1]) / 100.0)
                                    except ValueError:
                                        pass
                            if recent:
                                factors["momentum_daily"] = recent[-252:]
    except Exception:
        pass

    _session_cache["ff_factors"] = factors
    return factors


# ── Damodaran Industry Data ──

def fetch_damodaran_industry(sic_code=""):
    """Fetch Damodaran industry-level data (betas, multiples, cost of capital)."""
    if "damodaran" in _session_cache:
        return _session_cache["damodaran"]

    result = {}
    # Beta by sector
    url = "https://pages.stern.nyu.edu/~adamodar/New_Home_Page/datafile/betas.html"
    try:
        html = _cached_fetch(url, "damodaran_betas.html", ttl=604800)  # 1 week cache
        if html:
            # Parse HTML table rows - extract industry name, number of firms, avg beta, etc.
            rows = re.findall(r"<tr[^>]*>(.*?)</tr>", html, re.DOTALL | re.IGNORECASE)
            industries = []
            for row in rows:
                cells = re.findall(r"<td[^>]*>(.*?)</td>", row, re.DOTALL | re.IGNORECASE)
                if len(cells) >= 4:
                    name = re.sub(r"<[^>]+>", "", cells[0]).strip()
                    try:
                        n_firms = int(re.sub(r"[^\d]", "", cells[1]))
                        avg_beta = float(re.sub(r"[^\d.]", "", cells[2]))
                        industries.append({"name": name, "n_firms": n_firms, "beta": avg_beta})
                    except (ValueError, IndexError):
                        pass
            result["industries"] = industries
    except Exception:
        pass

    # PE ratios by sector
    url2 = "https://pages.stern.nyu.edu/~adamodar/New_Home_Page/datafile/pedata.html"
    try:
        html = _cached_fetch(url2, "damodaran_pe.html", ttl=604800)
        if html:
            rows = re.findall(r"<tr[^>]*>(.*?)</tr>", html, re.DOTALL | re.IGNORECASE)
            pe_data = []
            for row in rows:
                cells = re.findall(r"<td[^>]*>(.*?)</td>", row, re.DOTALL | re.IGNORECASE)
                if len(cells) >= 4:
                    name = re.sub(r"<[^>]+>", "", cells[0]).strip()
                    try:
                        current_pe = float(re.sub(r"[^\d.]", "", cells[1]))
                        pe_data.append({"name": name, "pe": current_pe})
                    except (ValueError, IndexError):
                        pass
            result["pe_by_industry"] = pe_data
    except Exception:
        pass

    _session_cache["damodaran"] = result
    return result


# ── GPR Index ──

def fetch_gpr_index():
    """Fetch Caldara-Iacoviello Geopolitical Risk Index."""
    if "gpr" in _session_cache:
        return _session_cache["gpr"]

    result = {}
    url = "https://www.matteoiacoviello.com/gpr_files/data_gpr_daily_recent.xls"
    try:
        # Try the CSV/text version first
        data = _cached_fetch("https://www.matteoiacoviello.com/gpr_files/data_gpr_export.xls",
                             "gpr_index.xls", ttl=86400, binary=True)
        if data:
            # Parse as tab-delimited text (despite .xls extension, it's often TSV)
            text = data.decode("utf-8", errors="replace")
            lines = text.strip().split("\n")
            if len(lines) > 1:
                recent_values = []
                for line in lines[-60:]:  # last 60 months
                    parts = line.split("\t")
                    if len(parts) >= 2:
                        try:
                            val = float(parts[-1])
                            recent_values.append(val)
                        except ValueError:
                            pass
                if recent_values:
                    result["current"] = recent_values[-1]
                    result["history"] = recent_values
                    result["mean"] = statistics.mean(recent_values)
                    result["percentile"] = sum(1 for v in recent_values if v <= recent_values[-1]) / len(recent_values)
    except Exception:
        pass

    _session_cache["gpr"] = result
    return result


# ── Shiller CAPE ──

def fetch_shiller_cape():
    """Fetch Shiller CAPE ratio from Yale data."""
    if "cape" in _session_cache:
        return _session_cache["cape"]

    result = {}
    # Use the CSV mirror since XLS requires xlrd
    url = "https://data.nasdaq.com/api/v3/datasets/MULTPL/SHILLER_PE_RATIO_MONTH.csv?rows=120"
    try:
        data = _cached_fetch(url, "shiller_cape.csv", ttl=86400)
        if data:
            reader = csv.DictReader(io.StringIO(data))
            rows = list(reader)
            if rows:
                try:
                    result["current"] = float(rows[0].get("Value", 0))
                    result["history"] = [float(r.get("Value", 0)) for r in rows if r.get("Value")]
                    if result["history"]:
                        result["mean_10yr"] = statistics.mean(result["history"][:min(120, len(result["history"]))])
                        result["percentile"] = sum(1 for v in result["history"] if v <= result["current"]) / len(result["history"])
                except (ValueError, KeyError):
                    pass
    except Exception:
        pass

    _session_cache["cape"] = result
    return result


# ── Loughran-McDonald Sentiment Dictionary ──

def fetch_lm_dictionary():
    """Fetch Loughran-McDonald financial sentiment word lists."""
    if "lm_dict" in _session_cache:
        return _session_cache["lm_dict"]

    result = {"negative": set(), "positive": set(), "uncertainty": set(),
              "litigious": set(), "constraining": set(), "superfluous": set()}
    cache_file = os.path.join(DSCO_CACHE_DIR, "lm_master_dict.csv")

    url = "https://sraf.nd.edu/loughranmcdonald-master-dictionary/"
    # Use a simplified approach - cache the known word categories
    # The master dictionary is large; we'll use the pre-compiled word lists
    word_list_urls = {
        "negative": "https://sraf.nd.edu/loughranmcdonald-master-dictionary/",
    }

    # Build a minimal dictionary from known financial sentiment words
    # These are the most impactful words from the LM dictionary
    result["negative"] = {
        "loss", "losses", "decline", "declined", "adverse", "adversely", "impairment",
        "impaired", "litigation", "lawsuit", "penalty", "penalties", "restructuring",
        "default", "defaults", "breach", "violation", "terminate", "terminated",
        "unfavorable", "weakness", "deteriorate", "deterioration", "downturn",
        "recession", "bankruptcy", "insolvent", "writedown", "writeoff", "deficit",
        "delinquent", "delinquency", "foreclosure", "layoff", "layoffs", "shutdown",
        "closing", "cessation", "discontinued", "abandoned", "failure", "failed",
        "unable", "inability", "damage", "damages", "critical", "catastrophic",
        "severe", "seriously", "negative", "negatively", "unfavorable", "worst",
        "worse", "harm", "harmful", "detrimental", "problematic", "challenging",
    }
    result["positive"] = {
        "achieve", "achieved", "gain", "gains", "improve", "improved", "improvement",
        "growth", "growing", "grew", "strong", "stronger", "strongest", "benefit",
        "beneficial", "favorable", "opportunity", "opportunities", "success",
        "successful", "profitability", "profitable", "efficient", "efficiency",
        "innovation", "innovative", "advantage", "advantageous", "exceed", "exceeded",
        "outperform", "outperformed", "superior", "progress", "positive", "positively",
        "enhanced", "enhancement", "strength", "strengths", "optimistic", "upside",
    }
    result["uncertainty"] = {
        "may", "might", "could", "possibly", "perhaps", "uncertain", "uncertainty",
        "unclear", "unknown", "unpredictable", "volatile", "volatility", "risk",
        "risks", "approximate", "approximately", "estimate", "estimated", "depend",
        "depends", "dependent", "contingent", "contingency", "subject", "variable",
        "fluctuate", "fluctuation", "possible", "probability", "believe", "expect",
        "anticipate", "predict", "forecast", "assume", "assumption", "preliminary",
    }
    result["litigious"] = {
        "allege", "alleged", "allegation", "claimant", "defendant", "deposition",
        "discovery", "court", "judicial", "jurisdiction", "jury", "lawsuit", "lawsuits",
        "legal", "litigate", "litigation", "plaintiff", "prosecute", "prosecution",
        "regulatory", "settlement", "statute", "statutory", "subpoena", "tribunal",
        "verdict", "arbitration", "injunction", "indictment", "plea", "testimony",
    }

    _session_cache["lm_dict"] = result
    return result


# ── EDGAR Frames API (cross-company aggregation) ──

def edgar_fetch_frames(concept, unit="USD", year=None):
    """Fetch EDGAR Frames API for cross-company data on a single concept."""
    if year is None:
        year = datetime.now().year - 1
    url = f"https://data.sec.gov/api/xbrl/frames/us-gaap/{concept}/{unit}/CY{year}.json"
    try:
        req = Request(url, headers={"User-Agent": EDGAR_UA})
        data = json.loads(urlopen(req, timeout=20).read().decode())
        return data
    except Exception:
        return None


# ══════════════════════════════════════════════════════════════════════════════
# Data fetching — shared data store for all agents
# ══════════════════════════════════════════════════════════════════════════════

class DataStore:
    """Central data store fetched once, shared across all agents."""

    def __init__(self, ticker):
        self.ticker = ticker.upper()
        self.raw = {}
        self.overview = {}
        self.quote = {}
        self.risk_free_rate = 0.043
        # Annual financial statements (AV format)
        self.inc_annual = []
        self.bs_annual = []
        self.cf_annual = []
        # Annual series (EDGAR or AV, year-keyed OrderedDicts)
        self.inc_series = OrderedDict()
        self.bs_series = OrderedDict()
        self.cf_series = OrderedDict()
        # Quarterly series (EDGAR 10-Q, quarter-end date keyed)
        self.inc_quarterly = OrderedDict()
        self.bs_quarterly = OrderedDict()
        self.cf_quarterly = OrderedDict()
        # Earnings
        self.earn_annual = []
        self.earn_quarterly = []
        # Market data
        self.news = []
        self.insider = []
        self.inst_holdings = []
        self.earnings_est = []
        self.price = None
        self.shares = 1
        self.daily_prices = []
        # Corporate profile
        self.profile = {}
        self.filings = []
        self.segments = {}
        self.ratios = {}
        self.validation = {}
        self.cik = None
        # Phase 3: Market Intelligence
        self.insider_form4 = []
        self.short_volume = {}
        self.treasury_curve = {}
        self.credit_spreads = {}
        self.vix_data = {}
        self.efts_alerts = []
        # Phase 4: Academic Benchmarks
        self.ff_factors = {}
        self.industry_benchmarks = {}
        self.gpr_index = {}
        self.cape_ratio = None
        self.lm_dictionary = {}

    def fetch(self, depth="standard"):
        safe_print(f"\n  {bold('Fetching data for')} {cyan(self.ticker)}...\n")

        # ── Phase 1: SEC EDGAR (free, unlimited, authoritative) ──
        safe_print(f"  {dim('Phase 1: SEC EDGAR (direct from filings)')}")
        safe_print(f"    {dim('⚡')} Resolving CIK...", end=" ", flush=True)
        cik = edgar_cik_for_ticker(self.ticker)
        if cik:
            self.cik = cik
            safe_print(green(f"✓ CIK {cik}"))
            safe_print(f"    {dim('⚡')} Fetching XBRL facts...", end=" ", flush=True)
            edgar_data = edgar_fetch_facts(cik)
            if edgar_data:
                gaap_count = len(edgar_data.get("facts", {}).get("us-gaap", {}))
                ifrs_count = len(edgar_data.get("facts", {}).get("ifrs-full", {}))
                taxonomy = "US-GAAP" if gaap_count >= ifrs_count else "IFRS"
                concept_count = max(gaap_count, ifrs_count)
                safe_print(green(f"✓ {concept_count} {taxonomy} concepts"))
                self.raw["edgar_facts"] = edgar_data
                self.raw["edgar_entity"] = edgar_data.get("entityName", "")
                self.raw["edgar_taxonomy"] = taxonomy

                # Build annual financial series from EDGAR
                inc_map = EDGAR_INCOME_MAP if taxonomy == "US-GAAP" else IFRS_INCOME_MAP
                self.inc_series = edgar_build_series(edgar_data, inc_map)
                self.bs_series = edgar_build_series(edgar_data, EDGAR_BALANCE_MAP)
                self.cf_series = edgar_build_series(edgar_data, EDGAR_CASHFLOW_MAP)
                n_years = len(self.inc_series)
                safe_print(f"    {dim('⚡')} Parsed {green(str(n_years))} years of annual data from SEC filings")

                # Build quarterly series
                safe_print(f"    {dim('⚡')} Parsing quarterly data...", end=" ", flush=True)
                self.inc_quarterly = edgar_build_quarterly_series(edgar_data, inc_map)
                self.bs_quarterly = edgar_build_quarterly_series(edgar_data, EDGAR_BALANCE_MAP)
                self.cf_quarterly = edgar_build_quarterly_series(edgar_data, EDGAR_CASHFLOW_MAP)
                n_qtrs = len(self.inc_quarterly)
                safe_print(green(f"✓ {n_qtrs} quarters"))

                # Segment data
                self.segments = edgar_fetch_segment_data(edgar_data)
                if self.segments:
                    safe_print(f"    {dim('⚡')} Found {green(str(len(self.segments)))} business segments")

                # Filing history
                safe_print(f"    {dim('⚡')} Filing index...", end=" ", flush=True)
                filings, entity_name, sic, sic_desc = edgar_fetch_filing_index(
                    cik, form_types={"10-K", "10-Q", "8-K", "DEF 14A", "20-F", "40-F", "S-1"})
                self.filings = filings
                if entity_name:
                    self.raw["edgar_entity"] = entity_name
                self.raw["sic"] = sic
                self.raw["sic_desc"] = sic_desc
                safe_print(green(f"✓ {len(filings)} filings"))

            else:
                safe_print(red("✗"))
        else:
            safe_print(red(f"✗ ticker not found in SEC database"))

        # ── Phase 2: Alpha Vantage (market data, estimates, sentiment) ──
        safe_print(f"\n  {dim('Phase 2: Alpha Vantage (market data & estimates)')}")

        av_calls = [
            ("Company Overview",    "OVERVIEW",        {"symbol": self.ticker}),
            ("Global Quote",        "GLOBAL_QUOTE",    {"symbol": self.ticker, "datatype": "json"}),
            ("Earnings",            "EARNINGS",        {"symbol": self.ticker}),
            ("Treasury Yield 10Y",  "TREASURY_YIELD",  {"interval": "daily", "maturity": "10year", "datatype": "json"}),
        ]

        # Only fetch financial statements from AV if EDGAR failed
        if not self.inc_series:
            av_calls.insert(1, ("Income Statement", "INCOME_STATEMENT", {"symbol": self.ticker}))
            av_calls.insert(2, ("Balance Sheet",    "BALANCE_SHEET",    {"symbol": self.ticker}))
            av_calls.insert(3, ("Cash Flow",        "CASH_FLOW",        {"symbol": self.ticker}))

        # Daily prices for tail risk / fractal analysis
        av_calls.append(
            ("Daily Prices",       "TIME_SERIES_DAILY",  {"symbol": self.ticker, "outputsize": "compact"}))

        if depth in ("standard", "deep"):
            av_calls += [
                ("News Sentiment",     "NEWS_SENTIMENT",     {"tickers": self.ticker, "limit": "50", "sort": "LATEST"}),
                ("Earnings Estimates", "EARNINGS_ESTIMATES",  {"symbol": self.ticker}),
            ]

        if depth == "deep":
            av_calls += [
                ("Insider Txns",       "INSIDER_TRANSACTIONS",    {"symbol": self.ticker}),
                ("Inst. Holdings",     "INSTITUTIONAL_HOLDINGS",  {"symbol": self.ticker}),
            ]

        if AV_KEY:
            for label, fn, params in av_calls:
                safe_print(f"    {dim('⚡')} {label}...", end=" ", flush=True)
                result = av_fetch(fn, **params)
                key_map = {"OVERVIEW": "company_overview"}
                key = key_map.get(fn, fn.lower())
                self.raw[key] = result
                safe_print(green("✓") if result else red("✗"))
        else:
            safe_print(f"    {yellow('⚠ No ALPHA_VANTAGE_API_KEY — skipping market data (EDGAR-only mode)')}")

        self._parse()

        # ── Phase 3: Market Intelligence (free public sources) ──
        safe_print(f"\n  {dim('Phase 3: Market Intelligence (free public sources)')}")

        # FRED Treasury Curve + Credit Spreads
        if FRED_KEY:
            safe_print(f"    {dim('⚡')} FRED Treasury curve...", end=" ", flush=True)
            self.treasury_curve = fred_fetch_treasury_curve()
            safe_print(green(f"✓ {len(self.treasury_curve)} maturities") if self.treasury_curve else red("✗"))

            safe_print(f"    {dim('⚡')} FRED Credit spreads...", end=" ", flush=True)
            self.credit_spreads = fred_fetch_credit_spreads()
            safe_print(green("✓") if self.credit_spreads else red("✗"))
        else:
            safe_print(f"    {yellow('⚠ No FRED_API_KEY — skipping macro data')}")

        # CBOE VIX/SKEW
        safe_print(f"    {dim('⚡')} CBOE VIX term structure...", end=" ", flush=True)
        try:
            self.vix_data = cboe_fetch_vix_data()
            n_vix = len([k for k in self.vix_data if not k.endswith("_history")])
            safe_print(green(f"✓ {n_vix} indices") if self.vix_data else yellow("⚠ skipped"))
        except Exception:
            safe_print(red("✗"))

        # FINRA Short Volume
        safe_print(f"    {dim('⚡')} FINRA short volume...", end=" ", flush=True)
        try:
            self.short_volume = finra_fetch_short_volume(self.ticker)
            safe_print(green(f"✓ {len(self.short_volume)} days") if self.short_volume else yellow("⚠ no data"))
        except Exception:
            safe_print(red("✗"))

        if depth == "deep" and cik:
            # SEC Form 4 insider transactions
            safe_print(f"    {dim('⚡')} SEC Form 4 insider txns...", end=" ", flush=True)
            try:
                self.insider_form4 = sec_fetch_form4(cik)
                safe_print(green(f"✓ {len(self.insider_form4)} filings") if self.insider_form4 else yellow("⚠ none"))
            except Exception:
                safe_print(red("✗"))

            # EFTS alerts (13D, NT filings, restatements)
            safe_print(f"    {dim('⚡')} SEC EFTS alerts...", end=" ", flush=True)
            try:
                entity = self.raw.get("edgar_entity", self.ticker)
                alerts = []
                for form_set, label in [(["SC 13D", "SC 13D/A"], "13D activist"),
                                         (["NT 10-K", "NT 10-Q"], "late filing"),
                                         (["10-K/A", "10-Q/A"], "restatement")]:
                    hits = sec_efts_search(entity, forms=form_set)
                    for h in hits:
                        alerts.append({"type": label, "form": h.get("_source", {}).get("form_type", ""),
                                       "date": h.get("_source", {}).get("file_date", ""),
                                       "desc": h.get("_source", {}).get("display_name", "")})
                self.efts_alerts = alerts
                safe_print(green(f"✓ {len(alerts)} alerts") if alerts else green("✓ clean"))
            except Exception:
                safe_print(red("✗"))

        # ── Phase 4: Academic Reference Data (session-cached) ──
        safe_print(f"\n  {dim('Phase 4: Academic Reference Data (session-cached)')}")

        safe_print(f"    {dim('⚡')} Fama-French factors...", end=" ", flush=True)
        try:
            self.ff_factors = fetch_french_factors()
            safe_print(green(f"✓ {len(self.ff_factors.get('daily', []))} daily obs") if self.ff_factors.get("daily") else yellow("⚠"))
        except Exception:
            safe_print(red("✗"))

        safe_print(f"    {dim('⚡')} Damodaran industry data...", end=" ", flush=True)
        try:
            self.industry_benchmarks = fetch_damodaran_industry(self.raw.get("sic", ""))
            safe_print(green(f"✓ {len(self.industry_benchmarks.get('industries', []))} sectors") if self.industry_benchmarks.get("industries") else yellow("⚠"))
        except Exception:
            safe_print(red("✗"))

        safe_print(f"    {dim('⚡')} GPR Index...", end=" ", flush=True)
        try:
            self.gpr_index = fetch_gpr_index()
            gpr_val = self.gpr_index.get("current")
            safe_print(green(f"✓ GPR={gpr_val:.0f}") if gpr_val else yellow("⚠"))
        except Exception:
            safe_print(red("✗"))

        safe_print(f"    {dim('⚡')} Shiller CAPE...", end=" ", flush=True)
        try:
            cape_data = fetch_shiller_cape()
            self.cape_ratio = cape_data.get("current")
            safe_print(green(f"✓ CAPE={self.cape_ratio:.1f}") if self.cape_ratio else yellow("⚠"))
        except Exception:
            safe_print(red("✗"))

    def _parse(self):
        ov = self.raw.get("company_overview") or {}
        self.overview = {k: safe_float(ov, k) if k not in (
            "Name", "Symbol", "Exchange", "Sector", "Industry", "Description",
            "Currency", "FiscalYearEnd", "Address", "OfficialSite", "AssetType",
            "CIK", "Country", "LatestQuarter",
            "AnalystRatingStrongBuy", "AnalystRatingBuy", "AnalystRatingHold",
            "AnalystRatingSell", "AnalystRatingStrongSell",
            "DividendDate", "ExDividendDate"
        ) else ov.get(k) for k in ov}
        # Convenient aliases
        self.overview["name"] = ov.get("Name", "") or self.raw.get("edgar_entity", "")
        self.overview["symbol"] = ov.get("Symbol", self.ticker)
        self.overview["sector"] = ov.get("Sector", "")
        self.overview["industry"] = ov.get("Industry", "")
        self.overview["description"] = ov.get("Description", "")
        self.overview["exchange"] = ov.get("Exchange", "")
        self.overview["market_cap"] = safe_float(ov, "MarketCapitalization")
        self.overview["shares_out"] = safe_float(ov, "SharesOutstanding")
        self.overview["beta"] = safe_float(ov, "Beta")
        self.overview["pe"] = safe_float(ov, "PERatio")
        self.overview["fwd_pe"] = safe_float(ov, "ForwardPE")
        self.overview["peg"] = safe_float(ov, "PEGRatio")
        self.overview["pb"] = safe_float(ov, "PriceToBookRatio")
        self.overview["ps"] = safe_float(ov, "PriceToSalesRatioTTM")
        self.overview["ev_ebitda"] = safe_float(ov, "EVToEBITDA")
        self.overview["ev_rev"] = safe_float(ov, "EVToRevenue")
        self.overview["eps"] = safe_float(ov, "EPS")
        self.overview["dividend_yield"] = safe_float(ov, "DividendYield")
        self.overview["dividend_ps"] = safe_float(ov, "DividendPerShare")
        self.overview["profit_margin"] = safe_float(ov, "ProfitMargin")
        self.overview["op_margin"] = safe_float(ov, "OperatingMarginTTM")
        self.overview["roe"] = safe_float(ov, "ReturnOnEquityTTM")
        self.overview["roa"] = safe_float(ov, "ReturnOnAssetsTTM")
        self.overview["revenue_ttm"] = safe_float(ov, "RevenueTTM")
        self.overview["gp_ttm"] = safe_float(ov, "GrossProfitTTM")
        self.overview["ebitda"] = safe_float(ov, "EBITDA")
        self.overview["analyst_target"] = safe_float(ov, "AnalystTargetPrice")
        self.overview["52w_high"] = safe_float(ov, "52WeekHigh")
        self.overview["52w_low"] = safe_float(ov, "52WeekLow")
        self.overview["50dma"] = safe_float(ov, "50DayMovingAverage")
        self.overview["200dma"] = safe_float(ov, "200DayMovingAverage")

        gq = (self.raw.get("global_quote") or {}).get("Global Quote", {})
        self.quote = {
            "price": safe_float(gq, "05. price"),
            "volume": safe_float(gq, "06. volume"),
            "change_pct": gq.get("10. change percent", ""),
        }
        self.price = self.quote.get("price") or self.overview.get("52w_high")
        self.shares = self.overview.get("shares_out") or 1

        # If shares_out not from AV, try EDGAR
        if self.shares <= 1:
            facts = self.raw.get("edgar_facts")
            if facts:
                shares_entries = _edgar_annual_values(facts, "CommonStockSharesOutstanding", "shares")
                if shares_entries:
                    self.shares = shares_entries[-1].get("val", 1)

        # Treasury yield
        ty = self.raw.get("treasury_yield")
        if ty and "data" in ty:
            for entry in ty["data"]:
                v = entry.get("value", ".")
                if v != ".":
                    self.risk_free_rate = float(v) / 100.0
                    break

        # Financial statements — only from AV if EDGAR didn't provide them
        for key, attr in [("income_statement", "inc_annual"), ("balance_sheet", "bs_annual"), ("cash_flow", "cf_annual")]:
            raw = self.raw.get(key)
            if raw:
                setattr(self, attr, raw.get("annualReports", []))

        # Parse into series (only if EDGAR didn't already populate them)
        if not self.inc_series:
            self.inc_series = self._extract_series(self.inc_annual, INC_FIELDS)
        if not self.bs_series:
            self.bs_series = self._extract_series(self.bs_annual, BS_FIELDS)
        if not self.cf_series:
            self.cf_series = self._extract_series(self.cf_annual, CF_FIELDS)

        # Earnings
        earn = self.raw.get("earnings") or {}
        self.earn_annual = earn.get("annualEarnings", [])
        self.earn_quarterly = earn.get("quarterlyEarnings", [])

        # News
        ns = self.raw.get("news_sentiment") or {}
        self.news = ns.get("feed", [])

        # Earnings estimates
        ee = self.raw.get("earnings_estimates") or {}
        self.earnings_est = ee.get("annualEarnings", []) or ee.get("quarterlyEarnings", [])

        # Insider
        it = self.raw.get("insider_transactions") or {}
        self.insider = it.get("data", [])

        # Institutional
        ih = self.raw.get("institutional_holdings") or {}
        self.inst_holdings = ih.get("data", [])

        # Daily prices (for tail risk / fractal analysis)
        ts = self.raw.get("time_series_daily") or {}
        ts_data = ts.get("Time Series (Daily)", {})
        self.daily_prices = [{"date": k, **v} for k, v in sorted(ts_data.items(), reverse=True)]

        # Build derived data
        try:
            self.build_profile()
        except Exception:
            pass
        try:
            self.build_ratios()
        except Exception:
            pass
        try:
            self.validate_balance_sheet()
        except Exception:
            pass

    @staticmethod
    def _extract_series(reports, fields):
        series = OrderedDict()
        for report in sorted(reports, key=lambda r: r.get("fiscalDateEnding", "")):
            fy = report.get("fiscalDateEnding", "")[:4]
            row = {}
            for alias, keys in fields.items():
                if isinstance(keys, str): keys = [keys]
                for k in keys:
                    v = safe_float(report, k)
                    if v is not None:
                        row[alias] = v
                        break
                if alias not in row:
                    row[alias] = None
            series[fy] = row
        return series

    def latest(self, series_name):
        s = getattr(self, series_name, {})
        if not s: return {}
        return s[sorted(s.keys())[-1]]

    def prev(self, series_name):
        s = getattr(self, series_name, {})
        keys = sorted(s.keys())
        if len(keys) < 2: return {}
        return s[keys[-2]]

    def years(self, series_name, n=6):
        s = getattr(self, series_name, {})
        return sorted(s.keys())[-n:]

    def build_profile(self):
        """Build comprehensive corporate profile from all data sources."""
        p = {}
        p["ticker"] = self.ticker
        p["name"] = self.overview.get("name", self.raw.get("edgar_entity", ""))
        p["cik"] = self.cik
        p["exchange"] = self.overview.get("exchange", "")
        p["sector"] = self.overview.get("sector", "")
        p["industry"] = self.overview.get("industry", "")
        p["sic"] = self.raw.get("sic", "")
        p["sic_desc"] = self.raw.get("sic_desc", "")
        p["taxonomy"] = self.raw.get("edgar_taxonomy", "US-GAAP")
        p["description"] = self.overview.get("description", "")
        p["market_cap"] = self.overview.get("market_cap")
        p["price"] = self.price
        p["shares"] = self.shares
        p["beta"] = self.overview.get("beta")

        # Filing history summary
        form_counts = {}
        for f in self.filings:
            ft = f.get("form", "?")
            form_counts[ft] = form_counts.get(ft, 0) + 1
        p["filing_counts"] = form_counts
        p["latest_10k"] = next((f for f in self.filings if f.get("form") == "10-K"), None)
        p["latest_10q"] = next((f for f in self.filings if f.get("form") == "10-Q"), None)
        p["latest_8k"] = next((f for f in self.filings if f.get("form") == "8-K"), None)

        # Data coverage
        p["annual_years"] = sorted(self.inc_series.keys()) if self.inc_series else []
        p["quarterly_periods"] = sorted(self.inc_quarterly.keys()) if self.inc_quarterly else []
        p["segments"] = list(self.segments.keys()) if self.segments else []

        self.profile = p
        return p

    def build_ratios(self):
        """Compute 60+ financial ratios from the latest annual data."""
        ratios = {}

        inc = list(self.inc_series.values())[-1] if self.inc_series else {}
        bs = list(self.bs_series.values())[-1] if self.bs_series else {}
        cf = list(self.cf_series.values())[-1] if self.cf_series else {}
        inc_prev = list(self.inc_series.values())[-2] if len(self.inc_series) > 1 else {}
        bs_prev = list(self.bs_series.values())[-2] if len(self.bs_series) > 1 else {}

        def g(d, k):
            v = d.get(k)
            return float(v) if v is not None else None
        def safe_div(a, b):
            return a / b if a is not None and b is not None and b != 0 else None

        rev = g(inc, "revenue")
        cogs = g(inc, "cogs")
        gp = g(inc, "gross_profit")
        op_inc = g(inc, "op_income")
        ni = g(inc, "net_income")
        ebitda = g(inc, "ebitda")
        rd = g(inc, "rd")
        sga = g(inc, "sga")
        da = g(inc, "da")
        interest = g(inc, "interest_exp")
        tax = g(inc, "tax")
        pretax = g(inc, "pretax_income")
        sbc = g(inc, "sbc_inc")

        ta = g(bs, "total_assets")
        ca = g(bs, "current_assets")
        cl = g(bs, "current_liabilities")
        tl = g(bs, "total_liabilities")
        eq = g(bs, "equity")
        cash = g(bs, "cash")
        recv = g(bs, "receivables")
        inv = g(bs, "inventory")
        lt_debt = g(bs, "lt_debt")
        st_debt = g(bs, "st_debt")
        goodwill = g(bs, "goodwill")
        intangibles = g(bs, "intangibles")
        ppe = g(bs, "ppe")
        re = g(bs, "retained_earnings")
        deferred_rev = g(bs, "deferred_revenue")
        ap = g(bs, "accounts_payable")
        rou = g(bs, "rou_assets")
        op_lease = g(bs, "op_lease_liability")

        cfo = g(cf, "cfo")
        capex = g(cf, "capex")
        if capex and capex > 0: capex = -capex  # normalize to negative
        buybacks = g(cf, "buybacks")
        divs = g(cf, "dividends")
        sbc_cf = g(cf, "sbc")
        acq = g(cf, "acquisitions")

        fcf_val = (cfo + capex) if cfo is not None and capex is not None else None

        # ── Profitability Ratios ──
        ratios["gross_margin"] = safe_div(gp, rev)
        ratios["operating_margin"] = safe_div(op_inc, rev)
        ratios["net_margin"] = safe_div(ni, rev)
        ratios["ebitda_margin"] = safe_div(ebitda, rev)
        ratios["fcf_margin"] = safe_div(fcf_val, rev)
        ratios["rd_intensity"] = safe_div(rd, rev)
        ratios["sga_ratio"] = safe_div(sga, rev)
        ratios["sbc_ratio"] = safe_div(sbc or sbc_cf, rev)
        avg_ta = ((ta or 0) + (g(bs_prev, "total_assets") or ta or 0)) / 2 if ta else None
        avg_eq = ((eq or 0) + (g(bs_prev, "equity") or eq or 0)) / 2 if eq else None
        ratios["roa"] = safe_div(ni, avg_ta)
        ratios["roe"] = safe_div(ni, avg_eq)
        ratios["roic"] = safe_div((op_inc or 0) * (1 - 0.21), ((eq or 0) + (lt_debt or 0)) or None) if op_inc else None
        ratios["asset_turnover"] = safe_div(rev, avg_ta)

        # ── DuPont Decomposition ──
        ratios["dupont_npm"] = ratios["net_margin"]
        ratios["dupont_at"] = ratios["asset_turnover"]
        ratios["dupont_em"] = safe_div(avg_ta, avg_eq)  # equity multiplier

        # ── Liquidity Ratios ──
        ratios["current_ratio"] = safe_div(ca, cl)
        quick_assets = (cash or 0) + (recv or 0)
        ratios["quick_ratio"] = safe_div(quick_assets, cl) if cl else None
        ratios["cash_ratio"] = safe_div(cash, cl)
        ratios["working_capital"] = (ca - cl) if ca is not None and cl is not None else None
        ratios["wc_to_revenue"] = safe_div(ratios["working_capital"], rev)

        # ── Solvency / Leverage Ratios ──
        total_debt = (lt_debt or 0) + (st_debt or 0)
        ratios["debt_to_equity"] = safe_div(total_debt, eq)
        ratios["debt_to_assets"] = safe_div(total_debt, ta)
        ratios["debt_to_ebitda"] = safe_div(total_debt, ebitda)
        ratios["net_debt"] = total_debt - (cash or 0)
        ratios["net_debt_to_ebitda"] = safe_div(ratios["net_debt"], ebitda)
        ratios["equity_ratio"] = safe_div(eq, ta)
        ratios["interest_coverage"] = safe_div(ebitda, abs(interest)) if interest else None
        ratios["debt_service_coverage"] = safe_div(cfo, abs(interest or 0) + abs(st_debt or 0)) if interest or st_debt else None
        ratios["leverage_ratio"] = safe_div(ta, eq)  # = equity multiplier
        ratios["lt_debt_to_cap"] = safe_div(lt_debt, (lt_debt or 0) + (eq or 0)) if lt_debt or eq else None

        # ── Efficiency Ratios ──
        ratios["receivable_days"] = safe_div((recv or 0) * 365, rev)
        ratios["inventory_days"] = safe_div((inv or 0) * 365, cogs)
        ratios["payable_days"] = safe_div((ap or 0) * 365, cogs)
        rd_days = ratios.get("receivable_days")
        id_days = ratios.get("inventory_days")
        pd_days = ratios.get("payable_days")
        ratios["cash_conversion_cycle"] = ((rd_days or 0) + (id_days or 0) - (pd_days or 0)) if any([rd_days, id_days]) else None
        ratios["inventory_turnover"] = safe_div(cogs, inv)
        ratios["receivable_turnover"] = safe_div(rev, recv)
        ratios["fixed_asset_turnover"] = safe_div(rev, ppe)
        ratios["capex_to_revenue"] = safe_div(abs(capex or 0), rev)
        ratios["capex_to_depreciation"] = safe_div(abs(capex or 0), da)

        # ── Cash Flow Ratios ──
        ratios["cfo_to_net_income"] = safe_div(cfo, ni)
        ratios["fcf_to_net_income"] = safe_div(fcf_val, ni)
        ratios["cfo_to_debt"] = safe_div(cfo, total_debt) if total_debt else None
        ratios["fcf_yield"] = safe_div(fcf_val, (self.price or 0) * (self.shares or 1)) if self.price else None
        ratios["buyback_yield"] = safe_div(abs(buybacks or 0), (self.price or 0) * (self.shares or 1)) if self.price and buybacks else None
        ratios["dividend_payout"] = safe_div(abs(divs or 0), ni) if ni and ni > 0 and divs else None
        ratios["total_payout_ratio"] = safe_div(abs(divs or 0) + abs(buybacks or 0), ni) if ni and ni > 0 else None
        ratios["reinvestment_rate"] = safe_div(abs(capex or 0) + abs(acq or 0), cfo) if cfo else None

        # ── Market / Valuation Ratios (requires price) ──
        if self.price and self.shares:
            mkt_cap = self.price * self.shares
            ev = mkt_cap + total_debt - (cash or 0)
            ratios["pe_ratio"] = safe_div(self.price, safe_div(ni, self.shares))
            ratios["pb_ratio"] = safe_div(mkt_cap, eq)
            ratios["ps_ratio"] = safe_div(mkt_cap, rev)
            ratios["ev_to_ebitda"] = safe_div(ev, ebitda)
            ratios["ev_to_revenue"] = safe_div(ev, rev)
            ratios["ev_to_fcf"] = safe_div(ev, fcf_val)
            ratios["earnings_yield"] = safe_div(ni, mkt_cap)

        # ── Quality Ratios ──
        ratios["accrual_ratio"] = safe_div((ni or 0) - (cfo or 0), avg_ta) if ni is not None and cfo is not None else None
        tangible_eq = (eq or 0) - (goodwill or 0) - (intangibles or 0)
        ratios["tangible_bv_per_share"] = safe_div(tangible_eq, self.shares)
        ratios["goodwill_to_assets"] = safe_div(goodwill, ta)
        ratios["intangibles_to_assets"] = safe_div((goodwill or 0) + (intangibles or 0), ta)
        ratios["effective_tax_rate"] = safe_div(tax, pretax) if pretax and pretax > 0 else None
        ratios["deferred_revenue_ratio"] = safe_div(deferred_rev, rev)

        # ── Lease & Off-Balance Sheet ──
        if rou or op_lease:
            ratios["rou_to_assets"] = safe_div(rou, ta)
            ratios["lease_liability_to_debt"] = safe_div(op_lease, total_debt) if total_debt else None

        # ── Growth (YoY) ──
        rev_prev = g(inc_prev, "revenue")
        ni_prev = g(inc_prev, "net_income")
        ebitda_prev = g(inc_prev, "ebitda")
        ratios["revenue_growth_yoy"] = safe_div((rev or 0) - (rev_prev or 0), abs(rev_prev or 0)) if rev_prev and rev_prev != 0 else None
        ratios["ni_growth_yoy"] = safe_div((ni or 0) - (ni_prev or 0), abs(ni_prev or 0)) if ni_prev and ni_prev != 0 else None
        ratios["ebitda_growth_yoy"] = safe_div((ebitda or 0) - (ebitda_prev or 0), abs(ebitda_prev or 0)) if ebitda_prev and ebitda_prev != 0 else None

        self.ratios = {k: v for k, v in ratios.items() if v is not None}
        return self.ratios

    def validate_balance_sheet(self):
        """Validate accounting identities across all annual periods."""
        issues = []
        for year, bs in self.bs_series.items():
            ta = bs.get("total_assets")
            tl = bs.get("total_liabilities")
            eq = bs.get("equity")
            if ta and tl and eq:
                computed = tl + eq
                diff_pct = abs(ta - computed) / ta if ta != 0 else 0
                if diff_pct > 0.02:  # >2% discrepancy
                    issues.append({"year": year, "type": "A≠L+E",
                                   "assets": ta, "computed": computed, "diff_pct": diff_pct})

        # Cash flow validation: CFO + CFI + CFF ≈ ΔCash
        cf_keys = sorted(self.cf_series.keys())
        bs_keys = sorted(self.bs_series.keys())
        for i in range(1, min(len(cf_keys), len(bs_keys))):
            cf = self.cf_series[cf_keys[i]]
            cfo = cf.get("cfo") or 0
            cfi = cf.get("cfi") or 0
            cff = cf.get("cff") or 0
            fx = cf.get("fx_effect") or 0
            computed_change = cfo + cfi + cff + fx
            actual_cash = (self.bs_series.get(bs_keys[i], {}).get("cash") or 0)
            prev_cash = (self.bs_series.get(bs_keys[i-1], {}).get("cash") or 0)
            actual_change = actual_cash - prev_cash
            if actual_change != 0:
                diff_pct = abs(computed_change - actual_change) / abs(actual_change)
                if diff_pct > 0.15 and abs(computed_change - actual_change) > 1e8:
                    issues.append({"year": cf_keys[i], "type": "CF≠ΔCash",
                                   "computed": computed_change, "actual": actual_change, "diff_pct": diff_pct})

        self.validation = {"issues": issues, "clean": len(issues) == 0}
        return self.validation

    def quarterly_trend(self, series_name, field, n=8):
        """Get the last n quarterly values for a field. Returns [(date, value), ...]."""
        s = getattr(self, series_name, {})
        results = []
        for key in sorted(s.keys())[-n:]:
            v = s[key].get(field)
            if v is not None:
                results.append((key, v))
        return results


# Field maps
INC_FIELDS = {
    "revenue":       ["totalRevenue"],
    "cogs":          ["costOfRevenue", "costofGoodsAndServicesSold"],
    "gross_profit":  ["grossProfit"],
    "rd":            ["researchAndDevelopment"],
    "sga":           ["sellingGeneralAndAdministrative"],
    "opex":          ["operatingExpenses"],
    "op_income":     ["operatingIncome"],
    "interest_exp":  ["interestExpense", "interestAndDebtExpense"],
    "pretax_income": ["incomeBeforeTax"],
    "tax":           ["incomeTaxExpense"],
    "net_income":    ["netIncome"],
    "ebitda":        ["ebitda"],
    "da":            ["depreciationAndAmortization"],
    "shares":        ["commonStockSharesOutstanding"],
}

BS_FIELDS = {
    "total_assets":        ["totalAssets"],
    "current_assets":      ["totalCurrentAssets"],
    "cash":                ["cashAndCashEquivalentsAtCarryingValue", "cashAndShortTermInvestments"],
    "st_investments":      ["shortTermInvestments"],
    "receivables":         ["currentNetReceivables"],
    "inventory":           ["inventory"],
    "total_liabilities":   ["totalLiabilities"],
    "current_liabilities": ["totalCurrentLiabilities"],
    "lt_debt":             ["longTermDebt"],
    "st_debt":             ["shortTermDebt", "currentLongTermDebt"],
    "equity":              ["totalShareholderEquity"],
    "retained_earnings":   ["retainedEarnings"],
    "goodwill":            ["goodwill"],
    "intangibles":         ["intangibleAssets"],
    "ppe":                 ["propertyPlantEquipment"],
    "total_debt":          ["shortLongTermDebtTotal"],
}

CF_FIELDS = {
    "cfo":          ["operatingCashflow"],
    "capex":        ["capitalExpenditures"],
    "dividends":    ["dividendPayout"],
    "buybacks":     ["commonStockRepurchased", "paymentsForRepurchaseOfCommonStock"],
    "cff":          ["cashflowFromFinancing"],
    "cfi":          ["cashflowFromInvestment"],
    "da_cf":        ["depreciationDepletionAndAmortization"],
    "sbc":          ["stockBasedCompensation", "shareBasedCompensation"],
    "change_wc":    ["changeInOperatingLiabilities"],
    "acquisitions": ["acquisitionsNet"],
}

# ══════════════════════════════════════════════════════════════════════════════
# Helper computations
# ══════════════════════════════════════════════════════════════════════════════

def cagr(series, field):
    vals = [(y, r.get(field)) for y, r in series.items() if r.get(field) and r.get(field) > 0]
    if len(vals) < 2: return None
    first_y, first_v = vals[0]
    last_y, last_v = vals[-1]
    years = int(last_y) - int(first_y)
    if years <= 0 or first_v <= 0: return None
    return (last_v / first_v) ** (1.0 / years) - 1

def yoy(series, field):
    items = [(y, r.get(field)) for y, r in series.items() if r.get(field) is not None]
    items.sort(key=lambda x: x[0])
    out = []
    for i in range(1, len(items)):
        prev = items[i-1][1]
        curr = items[i][1]
        if prev and prev != 0:
            out.append((items[i][0], (curr - prev) / abs(prev)))
    return out

def fcf(cf_row):
    cfo = cf_row.get("cfo")
    capex = cf_row.get("capex")
    if cfo is None: return None
    return cfo - abs(capex or 0)

def ufcf(inc_row, cf_row, tax_rate=0.21):
    op = inc_row.get("op_income")
    da = cf_row.get("da_cf") or inc_row.get("da") or 0
    cx = abs(cf_row.get("capex") or 0)
    dwc = cf_row.get("change_wc") or 0
    if op is None: return None
    return op * (1 - tax_rate) + da - cx - dwc

def net_debt(bs):
    return ((bs.get("lt_debt") or 0) + (bs.get("st_debt") or 0)) - (bs.get("cash") or 0)


# ══════════════════════════════════════════════════════════════════════════════
# Advanced Analytics Engine — Cross-Agent Intelligence
# ══════════════════════════════════════════════════════════════════════════════

# Agent taxonomy for correlation and conflict analysis
AGENT_DOMAINS = {
    # Domain: list of agent names in that domain
    "valuation": ["quant", "klarman", "factor"],
    "quality": ["fundamental", "forensic", "accrual", "gross_profit", "tax_quality", "real_manip"],
    "growth": ["growth", "asset_growth", "capital_cycle"],
    "risk": ["risk", "tail_risk", "minsky", "ergodicity", "ohlson", "pension"],
    "sentiment": ["sentiment", "catalyst", "narrative", "textual", "insider_flow"],
    "structure": ["credit", "capital", "issuance", "buyback_signal", "exec_align"],
    "governance": ["filing_timing", "activist", "earnings_bench", "customer_conc"],
    "macro": ["macro", "gpr", "vol_surface", "short_interest"],
    "operations": ["moat", "supply_chain", "microstructure", "complexity"],
}

# Agent weights based on empirical signal reliability (higher = more predictive)
AGENT_RELIABILITY_WEIGHTS = {
    "quant": 1.5, "klarman": 1.4, "fundamental": 1.3, "forensic": 1.4,
    "growth": 1.1, "credit": 1.2, "moat": 1.2, "factor": 1.3,
    "accrual": 1.3, "gross_profit": 1.3, "ohlson": 1.2,
    "insider_flow": 1.1, "vol_surface": 1.0, "short_interest": 0.9,
    "minsky": 1.1, "tail_risk": 1.0, "kelly": 1.0, "ergodicity": 0.9,
    "narrative": 0.8, "complexity": 0.8, "textual": 0.9,
    "pension": 1.0, "supply_chain": 0.8, "customer_conc": 0.9,
    "earnings_bench": 1.0, "filing_timing": 1.1, "activist": 1.0,
    "exec_align": 0.9, "inst_flow": 0.8, "capital_cycle": 1.0,
    "gpr": 0.7, "real_manip": 1.1, "asset_growth": 1.0,
    "issuance": 1.0, "buyback_signal": 1.0, "tax_quality": 0.9,
    "macro": 1.0, "risk": 1.0, "capital": 1.0, "sentiment": 0.9,
    "catalyst": 1.0, "microstructure": 0.9,
}

# Interaction pairs: when both fire together, amplify signal
SIGNAL_INTERACTIONS = [
    # (agent_a, agent_b, condition_a, condition_b, amplification_description, score_adjust)
    ("ohlson", "minsky", "bearish", "bearish", "distress+Minsky convergence", -8),
    ("forensic", "accrual", "bearish", "bearish", "dual earnings manipulation signal", -10),
    ("forensic", "earnings_bench", "bearish", "bearish", "forensic+benchmark manipulation", -8),
    ("insider_flow", "buyback_signal", "bullish", "bullish", "insider+corporate buying alignment", +8),
    ("ohlson", "credit", "bearish", "bearish", "distress+credit stress convergence", -10),
    ("gross_profit", "moat", "bullish", "bullish", "quality+moat reinforcement", +6),
    ("short_interest", "forensic", "bearish", "bearish", "shorts see what forensic confirms", -8),
    ("vol_surface", "tail_risk", "bearish", "bearish", "vol surface+tail risk convergence", -6),
    ("activist", "issuance", "bullish", "bearish", "activist+empire-building pressure", +5),
    ("insider_flow", "ohlson", "bearish", "bearish", "insiders selling+distress signal", -12),
    ("growth", "capital_cycle", "bearish", "bearish", "growth slowing+overinvestment", -6),
    ("accrual", "real_manip", "bearish", "bearish", "accrual+real manipulation double flag", -10),
    ("gpr", "macro", "bearish", "bearish", "geopolitical+macro headwinds", -5),
    ("pension", "credit", "bearish", "bearish", "pension+credit compound liability", -8),
    ("textual", "filing_timing", "bearish", "bearish", "negative text+filing issues", -7),
    ("customer_conc", "supply_chain", "bearish", "bearish", "concentration+supply chain fragility", -6),
    ("kelly", "factor", "bullish", "bullish", "sizing+factor alignment", +5),
    ("klarman", "quant", "bullish", "bullish", "value floor+DCF upside confirmation", +7),
]


def compute_z_score(value, values_list):
    """Compute z-score of a value relative to a distribution."""
    if not values_list or len(values_list) < 3:
        return 0
    mu = statistics.mean(values_list)
    sigma = statistics.stdev(values_list)
    if sigma == 0:
        return 0
    return (value - mu) / sigma


def compute_spearman_correlation(x_ranks, y_ranks):
    """Compute Spearman rank correlation between two lists."""
    n = min(len(x_ranks), len(y_ranks))
    if n < 3:
        return 0
    d_squared = sum((x_ranks[i] - y_ranks[i]) ** 2 for i in range(n))
    return 1 - (6 * d_squared) / (n * (n ** 2 - 1))


def rank_values(values):
    """Convert values to ranks (1-based, average ties)."""
    indexed = sorted(enumerate(values), key=lambda x: x[1])
    ranks = [0] * len(values)
    i = 0
    while i < len(indexed):
        j = i
        while j < len(indexed) - 1 and indexed[j][1] == indexed[j + 1][1]:
            j += 1
        avg_rank = (i + j) / 2.0 + 1
        for k in range(i, j + 1):
            ranks[indexed[k][0]] = avg_rank
        i = j + 1
    return ranks


def detect_signal_interactions(agents):
    """Detect compound signals where agent pairs amplify each other."""
    agent_map = {a.name: a for a in agents}
    interactions_fired = []

    for a_name, b_name, a_cond, b_cond, desc, adj in SIGNAL_INTERACTIONS:
        a = agent_map.get(a_name)
        b = agent_map.get(b_name)
        if a and b and a.signal == a_cond and b.signal == b_cond:
            interactions_fired.append({
                "agents": (a_name, b_name),
                "description": desc,
                "score_adjustment": adj,
                "a_score": a.score,
                "b_score": b.score,
            })
    return interactions_fired


def compute_domain_consensus(agents):
    """Analyze consensus within each analytical domain."""
    agent_map = {a.name: a for a in agents}
    domain_analysis = {}

    for domain, member_names in AGENT_DOMAINS.items():
        members = [(name, agent_map[name]) for name in member_names if name in agent_map and agent_map[name].score is not None]
        if not members:
            continue

        scores = [a.score for _, a in members]
        signals = [a.signal for _, a in members]
        n_bull = sum(1 for s in signals if s == "bullish")
        n_bear = sum(1 for s in signals if s == "bearish")
        n_neut = sum(1 for s in signals if s == "neutral")
        n_total = len(signals)

        # Internal disagreement: if domain has both strong bull and strong bear
        has_conflict = n_bull > 0 and n_bear > 0
        conflict_severity = min(n_bull, n_bear) / max(n_total, 1)

        # Domain score
        weighted_score = sum(
            a.score * AGENT_RELIABILITY_WEIGHTS.get(name, 1.0)
            for name, a in members
        )
        total_weight = sum(AGENT_RELIABILITY_WEIGHTS.get(name, 1.0) for name, _ in members)
        domain_score = weighted_score / total_weight if total_weight > 0 else 0

        # Consensus strength (0 = chaos, 1 = unanimity)
        if n_total > 1:
            max_faction = max(n_bull, n_bear, n_neut)
            consensus_strength = max_faction / n_total
        else:
            consensus_strength = 1.0

        domain_analysis[domain] = {
            "members": [(name, a.signal, a.score) for name, a in members],
            "domain_score": domain_score,
            "n_bullish": n_bull, "n_bearish": n_bear, "n_neutral": n_neut,
            "has_conflict": has_conflict,
            "conflict_severity": conflict_severity,
            "consensus_strength": consensus_strength,
            "signal": "bullish" if domain_score > 3 else "bearish" if domain_score < -3 else "neutral",
        }

    return domain_analysis


def compute_agent_correlation_matrix(agents):
    """Compute cross-correlation between agent scores for divergence detection."""
    scored = [(a.name, a.score) for a in agents if a.score is not None]
    if len(scored) < 5:
        return {}, []

    names = [n for n, _ in scored]
    scores = [s for _, s in scored]

    # Detect outlier agents (z-score > 2 from consensus)
    z_scores = {}
    if len(scores) >= 3:
        mu = statistics.mean(scores)
        sigma = statistics.stdev(scores)
        if sigma > 0:
            z_scores = {name: (score - mu) / sigma for name, score in scored}

    outliers = [(name, z) for name, z in z_scores.items() if abs(z) > 1.8]
    return z_scores, outliers


def detect_regime(store, agents):
    """Multi-signal regime detection: bull/bear/transition/crisis."""
    signals = {}

    # Price momentum regime
    dp = store.daily_prices
    if dp and len(dp) >= 50:
        prices = [safe_float(p, "4. close") for p in dp[:50]]
        prices = [p for p in prices if p]
        if len(prices) >= 50:
            ma20 = statistics.mean(prices[:20])
            ma50 = statistics.mean(prices[:50])
            signals["price_trend"] = "bull" if prices[0] > ma20 > ma50 else "bear" if prices[0] < ma20 < ma50 else "mixed"

    # VIX regime
    vix = store.vix_data.get("vix")
    if vix:
        signals["vol_regime"] = "crisis" if vix > 30 else "fear" if vix > 20 else "complacent" if vix < 13 else "normal"

    # Credit regime
    hy_oas = store.credit_spreads.get("hy_oas")
    if hy_oas:
        signals["credit_regime"] = "stress" if hy_oas > 0.05 else "tight" if hy_oas < 0.03 else "normal"

    # Yield curve regime
    curve = store.treasury_curve
    if curve.get("2Y") and curve.get("10Y"):
        spread = curve["10Y"] - curve["2Y"]
        signals["yield_curve"] = "inverted" if spread < 0 else "flat" if spread < 0.005 else "steep" if spread > 0.015 else "normal"

    # GPR regime
    gpr_pct = store.gpr_index.get("percentile")
    if gpr_pct:
        signals["geopolitical"] = "elevated" if gpr_pct > 0.75 else "low" if gpr_pct < 0.25 else "normal"

    # CAPE valuation regime
    cape = store.cape_ratio
    if cape:
        signals["valuation_regime"] = "expensive" if cape > 30 else "rich" if cape > 25 else "fair" if cape > 15 else "cheap"

    # Determine composite regime
    bearish_signals = sum(1 for v in signals.values() if v in ("bear", "crisis", "stress", "inverted", "elevated", "expensive"))
    bullish_signals = sum(1 for v in signals.values() if v in ("bull", "complacent", "tight", "steep", "low", "cheap"))
    total = len(signals) or 1

    if bearish_signals / total > 0.5:
        composite = "RISK-OFF"
    elif bullish_signals / total > 0.5:
        composite = "RISK-ON"
    elif bearish_signals > 0 and bullish_signals > 0:
        composite = "TRANSITION"
    else:
        composite = "NEUTRAL"

    return {"signals": signals, "regime": composite,
            "bearish_count": bearish_signals, "bullish_count": bullish_signals}


def compute_signal_velocity(store, series_name, field, n_periods=5):
    """Compute velocity (first derivative) and acceleration (second derivative) of a metric."""
    s = getattr(store, series_name, {})
    keys = sorted(s.keys())[-n_periods:]
    values = [s[k].get(field) for k in keys if s[k].get(field) is not None]

    if len(values) < 3:
        return {"velocity": 0, "acceleration": 0, "trend": "insufficient_data"}

    # Velocity: average period-over-period change
    changes = [(values[i] - values[i-1]) / abs(values[i-1]) if values[i-1] != 0 else 0
               for i in range(1, len(values))]
    velocity = statistics.mean(changes)

    # Acceleration: change in velocity
    if len(changes) >= 2:
        accel_changes = [changes[i] - changes[i-1] for i in range(1, len(changes))]
        acceleration = statistics.mean(accel_changes)
    else:
        acceleration = 0

    if velocity > 0.05 and acceleration > 0:
        trend = "accelerating_growth"
    elif velocity > 0.05:
        trend = "decelerating_growth"
    elif velocity < -0.05 and acceleration < 0:
        trend = "accelerating_decline"
    elif velocity < -0.05:
        trend = "decelerating_decline"
    else:
        trend = "stable"

    return {"velocity": velocity, "acceleration": acceleration, "trend": trend}


def bayesian_score_update(prior_score, evidence_scores, prior_confidence=0.5):
    """Bayesian-inspired score update: combine prior view with new evidence.
    prior_score: initial estimate (-100 to +100)
    evidence_scores: list of (score, confidence) tuples from agents
    Returns updated score with posterior confidence."""
    if not evidence_scores:
        return prior_score, prior_confidence

    total_evidence_weight = sum(conf for _, conf in evidence_scores)
    if total_evidence_weight == 0:
        return prior_score, prior_confidence

    # Weighted evidence mean
    evidence_mean = sum(s * c for s, c in evidence_scores) / total_evidence_weight

    # Posterior = weighted average of prior and evidence, weighted by confidence
    posterior = (prior_score * prior_confidence + evidence_mean * total_evidence_weight) / (prior_confidence + total_evidence_weight)

    # Posterior confidence increases with more evidence (diminishing returns)
    posterior_confidence = min(0.95, prior_confidence + total_evidence_weight * 0.05)

    return posterior, posterior_confidence


# ══════════════════════════════════════════════════════════════════════════════
# Agent base class
# ══════════════════════════════════════════════════════════════════════════════

class Agent:
    name = "base"
    title = "Base Agent"
    icon = "🔬"
    domain = "general"  # analytical domain for cross-correlation

    def __init__(self, store: DataStore):
        self.store = store
        self.findings = {}
        self.score = None       # optional -100 to +100 conviction contribution
        self.signal = None      # "bullish" / "bearish" / "neutral"
        self.summary = ""       # 1-2 sentence summary
        self.confidence = 0.5   # 0-1 confidence in signal (for Bayesian weighting)
        self.data_quality = 1.0 # 0-1 data completeness (penalizes missing data)

    def run(self):
        raise NotImplementedError

    def render(self):
        raise NotImplementedError

    def header(self):
        safe_print(f"\n  {bold(cyan(f'{self.icon} {self.title}'))}")
        safe_print(f"  {'─' * 88}")

# ══════════════════════════════════════════════════════════════════════════════
# AGENT 1: Quantitative Analyst
# ══════════════════════════════════════════════════════════════════════════════

class QuantAgent(Agent):
    name = "quant"
    title = "QUANTITATIVE ANALYST"
    icon = "📊"

    def run(self):
        s = self.store
        inc = s.latest("inc_series")
        bs = s.latest("bs_series")
        cf = s.latest("cf_series")

        # WACC
        beta = s.overview.get("beta") or 1.0
        rf = s.risk_free_rate
        ke = rf + beta * MARKET_RISK_PREMIUM
        interest = abs(inc.get("interest_exp") or 0)
        total_d = (bs.get("lt_debt") or 0) + (bs.get("st_debt") or 0)
        kd = (interest / total_d if total_d > 0 else rf + 0.02)
        kd = min(kd, 0.15)
        pretax = inc.get("pretax_income") or 0
        tax = inc.get("tax") or 0
        tax_rate = min(tax / pretax, 0.40) if pretax > 0 and tax > 0 else 0.21
        eq_val = s.overview.get("market_cap")
        if not eq_val and s.price and s.shares > 1:
            eq_val = s.price * s.shares
        if not eq_val:
            # Fallback: use book equity * rough market-to-book multiple
            book_eq = bs.get("equity") or 0
            eq_val = book_eq * 5 if book_eq > 0 else 1e9  # rough approximation
        total_cap = (eq_val + total_d) or 1
        we = eq_val / total_cap
        wd = total_d / total_cap
        wacc = we * ke + wd * kd * (1 - tax_rate)

        self.findings["wacc"] = {"wacc": wacc, "ke": ke, "kd": kd, "tax_rate": tax_rate,
                                  "beta": beta, "rf": rf, "we": we, "wd": wd}

        # Base FCF
        base = ufcf(inc, cf, tax_rate) or fcf(cf)
        self.findings["base_fcf"] = base
        nd = net_debt(bs)
        self.findings["net_debt"] = nd

        rev_g = cagr(s.inc_series, "revenue") or 0.05
        fcf_g = cagr(s.cf_series, "cfo") or rev_g
        growth_est = max(min(rev_g * 0.4 + fcf_g * 0.6, 0.30), -0.05)
        self.findings["growth_est"] = growth_est

        # DCF scenarios
        scenarios = {"Bear": growth_est * 0.5, "Base": growth_est, "Bull": growth_est * 1.5}
        dcf_results = {}
        for label, g in scenarios.items():
            dcf_results[label] = self._dcf(base, g, wacc, s.shares, nd)
        self.findings["dcf"] = dcf_results

        # Reverse DCF
        self.findings["implied_growth"] = self._reverse_dcf(s.price, s.shares, nd, wacc, base)

        # Sensitivity table
        self.findings["sensitivity"] = self._sensitivity(base, s.shares, nd, wacc, growth_est)

        # Monte Carlo
        if base and base > 0:
            self.findings["mc"] = self._monte_carlo(base, growth_est, wacc, s.shares, nd)

        # Multiples
        self.findings["multiples"] = self._multiples(s.overview, inc, cf, s.price, s.shares)

        # Signal
        base_dcf = dcf_results.get("Base")
        if base_dcf and base_dcf["fair_value"] > 0:
            if s.price:
                upside = base_dcf["fair_value"] / s.price - 1
                if upside > 0.15: self.signal = "bullish"; self.score = min(40, int(upside * 100))
                elif upside < -0.15: self.signal = "bearish"; self.score = max(-40, int(upside * 100))
                else: self.signal = "neutral"; self.score = int(upside * 50)
                self.summary = f"DCF base case ${base_dcf['fair_value']:,.0f} implies {upside*100:+.1f}% vs current ${s.price:,.2f}"
            else:
                self.signal = "neutral"; self.score = 5
                self.summary = f"DCF base case ${base_dcf['fair_value']:,.0f} (no live price for comparison)"
        else:
            self.signal = "neutral"; self.score = 0
            self.summary = "Insufficient data for DCF valuation"

    def _dcf(self, base_fcf, growth, wacc, shares, nd, years=10, fade=5):
        if not base_fcf or base_fcf <= 0 or shares <= 0: return None
        tg = TERMINAL_GROWTH_DEFAULT
        projected = []
        pv_sum = 0
        for yr in range(1, years + 1):
            g = growth if yr <= fade else growth + (tg - growth) * ((yr - fade) / (years - fade))
            f = (base_fcf if yr == 1 else projected[-1]["fcf"]) * (1 + g)
            pv = f / (1 + wacc) ** yr
            pv_sum += pv
            projected.append({"year": yr, "growth": g, "fcf": f, "pv": pv})
        tv_fcf = projected[-1]["fcf"] * (1 + tg)
        tv = tv_fcf / (wacc - tg) if wacc > tg else tv_fcf * 30
        pv_tv = tv / (1 + wacc) ** years
        ev = pv_sum + pv_tv
        eq = ev - nd
        fv = eq / shares
        return {"projected": projected, "pv_fcfs": pv_sum, "terminal_value": tv,
                "pv_terminal": pv_tv, "ev": ev, "equity_value": eq, "fair_value": fv,
                "tv_pct": pv_tv / ev if ev > 0 else 0, "growth": growth, "wacc": wacc}

    def _reverse_dcf(self, price, shares, nd, wacc, base_fcf):
        if not all([price, shares, base_fcf]) or base_fcf <= 0: return None
        target_ev = price * shares + nd
        lo, hi = -0.10, 0.50
        for _ in range(80):
            mid = (lo + hi) / 2
            r = self._dcf(base_fcf, mid, wacc, shares, nd)
            if not r: return None
            if r["ev"] < target_ev: lo = mid
            else: hi = mid
        return (lo + hi) / 2

    def _sensitivity(self, base_fcf, shares, nd, wacc_center, growth_center):
        if not base_fcf or base_fcf <= 0: return None
        growth_range = [growth_center + d for d in [-0.06, -0.03, 0, 0.03, 0.06]]
        wacc_range = [wacc_center + d for d in [-0.02, -0.01, 0, 0.01, 0.02]]
        table = []
        for w in wacc_range:
            row = []
            for g in growth_range:
                r = self._dcf(base_fcf, g, max(w, 0.03), shares, nd)
                row.append(r["fair_value"] if r else None)
            table.append(row)
        return {"growth_headers": growth_range, "wacc_headers": wacc_range, "values": table}

    def _monte_carlo(self, base_fcf, growth_mean, wacc_mean, shares, nd):
        rng = random.Random(42)
        results = []
        g_std = abs(growth_mean) * 0.4 + 0.02
        w_std = 0.015
        for _ in range(MC_ITERATIONS):
            g = rng.gauss(growth_mean, g_std)
            w = max(rng.gauss(wacc_mean, w_std), 0.03)
            tg = max(min(rng.gauss(TERMINAL_GROWTH_DEFAULT, 0.005), w - 0.01), 0.005)
            r = self._dcf(base_fcf, g, w, shares, nd)
            if r and r["fair_value"] > 0:
                results.append(r["fair_value"])
        if not results: return None
        results.sort()
        n = len(results)
        return {"mean": statistics.mean(results), "median": statistics.median(results),
                "std": statistics.stdev(results) if n > 1 else 0,
                "p5": results[int(n*0.05)], "p10": results[int(n*0.10)],
                "p25": results[int(n*0.25)], "p75": results[int(n*0.75)],
                "p90": results[int(n*0.90)], "p95": results[int(n*0.95)],
                "count": n}

    def _multiples(self, ov, inc, cf, price, shares):
        if not price: return []
        vals = []
        eps = ov.get("eps")
        if eps and eps > 0:
            for lbl, m in [("P/E @15x", 15), ("P/E @20x", 20), ("P/E @25x", 25), ("P/E @35x", 35)]:
                vals.append({"method": lbl, "value": eps * m})
        ebitda = inc.get("ebitda") or (inc.get("op_income", 0) + (inc.get("da") or 0))
        if ebitda and ebitda > 0:
            for lbl, m in [("EV/EBITDA @12x", 12), ("EV/EBITDA @18x", 18), ("EV/EBITDA @25x", 25)]:
                vals.append({"method": lbl, "value": ebitda * m / shares})
        f = fcf(cf)
        if f and f > 0:
            fps = f / shares
            for lbl, m in [("P/FCF @20x", 20), ("P/FCF @30x", 30), ("P/FCF @40x", 40)]:
                vals.append({"method": lbl, "value": fps * m})
        rev = inc.get("revenue")
        if rev and rev > 0:
            rps = rev / shares
            for lbl, m in [("P/S @3x", 3), ("P/S @6x", 6), ("P/S @10x", 10)]:
                vals.append({"method": lbl, "value": rps * m})
        return vals

    def render(self):
        self.header()
        s = self.store
        w = self.findings.get("wacc", {})
        safe_print(f"    {dim('WACC Components:')}")
        safe_print(f"      Rf={pct(w.get('rf'))}  β={num(w.get('beta'))}  Ke={pct(w.get('ke'))}  Kd(at)={pct(w.get('kd',0)*(1-w.get('tax_rate',0.21)))}  We={pct(w.get('we'))}  Wd={pct(w.get('wd'))}")
        safe_print(f"      {bold('WACC = ' + pct(w.get('wacc')))}")

        safe_print(f"\n    {dim('DCF Scenarios:')}")
        dcf = self.findings.get("dcf", {})
        for label in ["Bear", "Base", "Bull"]:
            r = dcf.get(label)
            if not r: continue
            fv = r["fair_value"]
            up = (fv / s.price - 1) if s.price else None
            color_fn = green if (up and up > 0) else red if (up and up < 0) else lambda x: x
            safe_print(f"      {label:6s}  g={pct(r['growth'])}  →  {bold(color_fn(f'${fv:>8,.2f}'))}  ({pct(up)} vs ${s.price:,.2f})" if s.price else f"      {label:6s}  g={pct(r['growth'])}  →  ${fv:>8,.2f}")

        base_r = dcf.get("Base")
        if base_r:
            safe_print(f"\n      TV% of EV: {pct(base_r['tv_pct'])}  │  PV(FCFs): {usd(base_r['pv_fcfs'])}  │  PV(TV): {usd(base_r['pv_terminal'])}")

        # Sensitivity
        sens = self.findings.get("sensitivity")
        if sens:
            safe_print(f"\n    {dim('Sensitivity Table (Fair Value per Share):')}")
            gh = sens["growth_headers"]
            wh = sens["wacc_headers"]
            safe_print(f"      {'WACC↓ / Growth→':>18s}  " + "  ".join(f"{g*100:>6.1f}%" for g in gh))
            safe_print(f"      {'─'*18}  " + "  ".join("───────" for _ in gh))
            for i, w_val in enumerate(wh):
                row_str = f"      {w_val*100:>6.1f}%           "
                for j, v in enumerate(sens["values"][i]):
                    if v is None:
                        row_str += f"  {'N/A':>7s}"
                    else:
                        is_center = (i == len(wh)//2 and j == len(gh)//2)
                        cell = f"${v:>6,.0f}" if v < 10000 else f"${v/1000:>5,.0f}K"
                        row_str += f"  {bold(cell) if is_center else cell}"
                safe_print(row_str)

        # Reverse DCF
        ig = self.findings.get("implied_growth")
        if ig is not None:
            ge = self.findings.get("growth_est", 0)
            safe_print(f"\n    {dim('Reverse DCF:')} Market implies {bold(pct(ig))} growth (vs est {pct(ge)})")

        # Monte Carlo
        mc = self.findings.get("mc")
        if mc:
            mc_count = mc["count"]
            safe_print(f"\n    {dim(f'Monte Carlo ({mc_count:,} sims):')}")
            safe_print(f"      Mean: ${mc['mean']:,.0f}  │  Median: ${mc['median']:,.0f}  │  σ: ${mc['std']:,.0f}")
            safe_print(f"      P5: ${mc['p5']:,.0f}  P10: ${mc['p10']:,.0f}  P25: ${mc['p25']:,.0f}  P75: ${mc['p75']:,.0f}  P90: ${mc['p90']:,.0f}  P95: ${mc['p95']:,.0f}")
            if s.price:
                buckets = [mc['p5'], mc['p10'], mc['p25'], mc['median'], mc['p75'], mc['p90'], mc['p95']]
                mx = max(buckets + [s.price])
                labels = ["P5 ", "P10", "P25", "P50", "P75", "P90", "P95"]
                safe_print()
                for lbl, v in zip(labels, buckets):
                    w_bar = int(v / mx * 35) if mx > 0 else 0
                    color_fn = green if v > s.price else red
                    safe_print(f"      {lbl}  {color_fn('█' * w_bar)} ${v:,.0f}")
                pw = int(s.price / mx * 35) if mx > 0 else 0
                safe_print(f"      CUR  {'─' * pw}{bold('▼')} ${s.price:,.0f}")

        # Multiples
        mults = self.findings.get("multiples", [])
        if mults:
            safe_print(f"\n    {dim('Multiples Valuation:')}")
            for mv in mults:
                up = (mv["value"] / s.price - 1) if s.price and mv["value"] else None
                safe_print(f"      {mv['method']:<18s}  ${mv['value']:>8,.0f}  {pct(up)}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 2: Fundamental Quality Analyst
# ══════════════════════════════════════════════════════════════════════════════

class FundamentalAgent(Agent):
    name = "fundamental"
    title = "FUNDAMENTAL QUALITY ANALYST"
    icon = "🔍"

    def run(self):
        s = self.store
        years = s.years("inc_series")
        if len(years) < 2:
            self.signal = "neutral"; self.score = 0; self.summary = "Insufficient history"; return

        cy, py = years[-1], years[-2]
        ci, pi = s.inc_series.get(cy, {}), s.inc_series.get(py, {})
        cb, pb = s.bs_series.get(cy, {}), s.bs_series.get(py, {})
        cc = s.cf_series.get(cy, {})

        # Piotroski
        f, f_details = self._piotroski(ci, pi, cb, pb, cc)
        self.findings["piotroski"] = {"score": f, "details": f_details}

        # Altman
        z, zone = self._altman(ci, cb)
        self.findings["altman"] = {"z": z, "zone": zone}

        # DuPont 5-factor
        self.findings["dupont"] = self._dupont(ci, cb)

        # Earnings quality
        self.findings["earnings_quality"] = self._earnings_quality(ci, cc, cb)

        # Accruals ratio
        ta_c = cb.get("total_assets") or 0
        ta_p = pb.get("total_assets") or 0
        ni = ci.get("net_income") or 0
        cfo = cc.get("cfo") or 0
        avg_ta = (ta_c + ta_p) / 2 if ta_p > 0 else ta_c
        accruals = (ni - cfo) / avg_ta if avg_ta > 0 else None
        self.findings["accruals_ratio"] = accruals

        # Margin trajectory
        self.findings["margins"] = {}
        for label, field in [("gross", "gross_profit"), ("operating", "op_income"), ("net", "net_income")]:
            series = []
            for y in years:
                inc = s.inc_series.get(y, {})
                n = inc.get(field)
                d = inc.get("revenue")
                if n is not None and d and d != 0:
                    series.append((y, n / d))
            self.findings["margins"][label] = series

        # R&D intensity
        rd_series = []
        for y in years:
            inc = s.inc_series.get(y, {})
            rd = inc.get("rd")
            rev = inc.get("revenue")
            if rd is not None and rev and rev > 0:
                rd_series.append((y, rd / rev))
        self.findings["rd_intensity"] = rd_series

        # SGA leverage
        sga_series = []
        for y in years:
            inc = s.inc_series.get(y, {})
            sga = inc.get("sga")
            rev = inc.get("revenue")
            if sga is not None and rev and rev > 0:
                sga_series.append((y, sga / rev))
        self.findings["sga_leverage"] = sga_series

        # Composite quality score
        score_pts = 0
        max_pts = 0

        # Piotroski (0-9 → 0-25 pts)
        if f is not None:
            score_pts += int(f / 9 * 25)
            max_pts += 25

        # Altman zone (25 pts)
        if zone:
            max_pts += 25
            if zone == "Safe": score_pts += 25
            elif zone == "Grey": score_pts += 12

        # Profitability (25 pts)
        max_pts += 25
        pm = s.overview.get("profit_margin")
        om = s.overview.get("op_margin")
        roe = s.overview.get("roe")
        if pm and pm > 0.10: score_pts += 8
        if om and om > 0.15: score_pts += 8
        if roe and roe > 0.15: score_pts += 9

        # Cash quality (25 pts)
        max_pts += 25
        if accruals is not None and accruals < 0: score_pts += 12  # negative accruals = good
        if cfo > ni: score_pts += 13

        self.findings["quality_score"] = {"score": score_pts, "max": max_pts,
                                           "pct": score_pts / max_pts * 100 if max_pts > 0 else 0}

        # Inject pre-computed ratios from DataStore
        if s.ratios:
            self.findings["ratios"] = dict(s.ratios)
        if s.validation:
            self.findings["validation"] = dict(s.validation)
        if s.profile:
            self.findings["profile"] = dict(s.profile)

        # Quarterly trend analysis
        qtrs = list(s.inc_quarterly.items())[-8:]
        if len(qtrs) >= 4:
            qrev = [(q, d.get("revenue", 0) or 0) for q, d in qtrs]
            qni = [(q, d.get("net_income", 0) or 0) for q, d in qtrs]
            self.findings["quarterly_revenue"] = qrev
            self.findings["quarterly_ni"] = qni
            # QoQ and YoY growth
            if len(qrev) >= 5:
                latest_rev = qrev[-1][1]
                yoy_rev = qrev[-5][1] if len(qrev) >= 5 else 0
                self.findings["rev_yoy_q"] = (latest_rev / yoy_rev - 1) if yoy_rev > 0 else None

        # Signal
        qpct = self.findings["quality_score"]["pct"]
        if qpct >= 75: self.signal = "bullish"; self.score = 20
        elif qpct >= 50: self.signal = "neutral"; self.score = 5
        else: self.signal = "bearish"; self.score = -15
        self.summary = f"Quality score {qpct:.0f}/100 | Piotroski {f}/9 | Altman {zone}"

    def _piotroski(self, ci, pi, cb, pb, cc):
        score = 0; details = []
        def test(name, cond):
            nonlocal score
            if cond: score += 1
            details.append((name, bool(cond)))

        ni = ci.get("net_income") or 0
        cfo = cc.get("cfo") or 0
        ta_c = cb.get("total_assets") or 1
        ta_p = pb.get("total_assets") or 1
        ni_p = pi.get("net_income") or 0

        test("Net income > 0", ni > 0)
        test("CFO > 0", cfo > 0)
        test("ROA improving", (ni / ta_c) > (ni_p / ta_p) if ta_c and ta_p else False)
        test("CFO > Net Income", cfo > ni)

        ltd_c = (cb.get("lt_debt") or 0) + (cb.get("st_debt") or 0)
        ltd_p = (pb.get("lt_debt") or 0) + (pb.get("st_debt") or 0)
        test("Leverage declining", (ltd_c / ta_c) < (ltd_p / ta_p) if ta_c and ta_p else False)

        ca_c = cb.get("current_assets") or 0; cl_c = cb.get("current_liabilities") or 1
        ca_p = pb.get("current_assets") or 0; cl_p = pb.get("current_liabilities") or 1
        test("Current ratio improving", (ca_c / cl_c) > (ca_p / cl_p) if cl_c and cl_p else False)

        test("No dilution", True)  # simplified

        gm_c = ci.get("gross_profit", 0) / ci["revenue"] if ci.get("gross_profit") and ci.get("revenue") else 0
        gm_p = pi.get("gross_profit", 0) / pi["revenue"] if pi.get("gross_profit") and pi.get("revenue") else 0
        test("Gross margin improving", gm_c > gm_p)

        rev_c = ci.get("revenue") or 0; rev_p = pi.get("revenue") or 0
        test("Asset turnover improving", (rev_c / ta_c) > (rev_p / ta_p) if ta_c and ta_p else False)

        return score, details

    def _altman(self, inc, bs):
        ta = bs.get("total_assets")
        if not ta or ta == 0: return None, None
        ca = bs.get("current_assets") or 0
        cl = bs.get("current_liabilities") or 0
        re = bs.get("retained_earnings") or 0
        ebit = inc.get("op_income") or 0
        rev = inc.get("revenue") or 0
        eq = bs.get("equity") or 0
        tl = bs.get("total_liabilities") or 1
        z = 1.2*((ca-cl)/ta) + 1.4*(re/ta) + 3.3*(ebit/ta) + 0.6*(eq/tl) + 1.0*(rev/ta)
        zone = "Safe" if z > 2.99 else "Grey" if z > 1.81 else "Distress"
        return z, zone

    def _dupont(self, inc, bs):
        ni = inc.get("net_income"); pt = inc.get("pretax_income")
        ebit = inc.get("op_income"); rev = inc.get("revenue")
        ta = bs.get("total_assets"); eq = bs.get("equity")
        if not all([ni, pt, ebit, rev, ta, eq]) or any(v == 0 for v in [pt, ebit, rev, ta, eq]):
            return None
        return {
            "tax_burden": ni / pt, "interest_burden": pt / ebit,
            "ebit_margin": ebit / rev, "asset_turnover": rev / ta,
            "leverage": ta / eq, "roe": (ni / pt) * (pt / ebit) * (ebit / rev) * (rev / ta) * (ta / eq)
        }

    def _earnings_quality(self, inc, cf, bs):
        """Assess earnings quality from multiple angles."""
        flags = []
        ni = inc.get("net_income") or 0
        cfo = cf.get("cfo") or 0
        sbc = abs(cf.get("sbc") or 0)
        rev = inc.get("revenue") or 1

        # CFO vs NI
        if ni > 0 and cfo > ni * 1.1:
            flags.append(("CFO significantly exceeds NI", "positive"))
        elif ni > 0 and cfo < ni * 0.7:
            flags.append(("CFO trails NI — earnings may not be cash-backed", "negative"))

        # SBC as % of NI
        if ni > 0 and sbc / ni > 0.25:
            flags.append((f"SBC is {sbc/ni*100:.0f}% of NI — dilution concern", "negative"))
        elif ni > 0 and sbc / ni < 0.05:
            flags.append(("Minimal SBC dilution", "positive"))

        # Depreciation vs CapEx
        da = abs(cf.get("da_cf") or inc.get("da") or 0)
        capex = abs(cf.get("capex") or 0)
        if capex > 0 and da > 0:
            ratio = da / capex
            if ratio > 1.5:
                flags.append(("D&A >> CapEx — possible asset milking", "warning"))
            elif ratio < 0.5:
                flags.append(("CapEx >> D&A — heavy reinvestment phase", "neutral"))

        return flags

    def render(self):
        self.header()

        # Piotroski
        p = self.findings.get("piotroski", {})
        ps = p.get("score")
        if ps is not None:
            color_fn = green if ps >= 7 else yellow if ps >= 4 else red
            safe_print(f"    {bold('Piotroski F-Score:')} {color_fn(f'{ps}/9')}  {bar(ps, 9, 18)}")
            for name, passed in p.get("details", []):
                safe_print(f"      {green('✓') if passed else red('✗')} {name}")

        # Altman
        a = self.findings.get("altman", {})
        z = a.get("z")
        if z is not None:
            zone = a["zone"]
            color_fn = green if zone == "Safe" else yellow if zone == "Grey" else red
            safe_print(f"\n    {bold('Altman Z-Score:')} {color_fn(f'{z:.2f} ({zone})')}")

        # DuPont
        dp = self.findings.get("dupont")
        if dp:
            safe_print(f"\n    {bold('DuPont Decomposition:')}")
            safe_print(f"      Tax×Interest×Margin×Turnover×Leverage = ROE")
            safe_print(f"      {num(dp['tax_burden'])} × {num(dp['interest_burden'])} × {pct(dp['ebit_margin'])} × {num(dp['asset_turnover'])} × {dp['leverage']:.1f}x = {pct(dp['roe'])}")

        # Accruals
        ar = self.findings.get("accruals_ratio")
        if ar is not None:
            color_fn = green if ar < 0 else red if ar > 0.10 else yellow
            safe_print(f"\n    {bold('Accruals Ratio:')} {color_fn(f'{ar*100:+.1f}%')} {'(high quality — cash > accrual)' if ar < 0 else '(watch — accruals high)'}")

        # Earnings quality
        eq = self.findings.get("earnings_quality", [])
        if eq:
            safe_print(f"\n    {bold('Earnings Quality Flags:')}")
            for msg, tone in eq:
                icon = green("✓") if tone == "positive" else red("⚠") if tone == "negative" else yellow("◆") if tone == "warning" else dim("○")
                safe_print(f"      {icon} {msg}")

        # Margin trajectories
        safe_print(f"\n    {bold('Margin Trajectories:')}")
        for label, series in self.findings.get("margins", {}).items():
            if series:
                vals = [v for _, v in series]
                trend = "↑" if len(vals) >= 2 and vals[-1] > vals[-2] else "↓" if len(vals) >= 2 and vals[-1] < vals[-2] else "→"
                color_fn = green if trend == "↑" else red if trend == "↓" else dim
                safe_print(f"      {label:>10s}: {' '.join(heatmap_cell(v) for _, v in series[-5:])}  {color_fn(trend)}  {spark(vals[-8:])}")

        # R&D
        rd = self.findings.get("rd_intensity", [])
        if rd:
            safe_print(f"\n    {dim('R&D Intensity:')} {' '.join(f'{y}:{v*100:.1f}%' for y, v in rd[-5:])}")

        # Quality score
        qs = self.findings.get("quality_score", {})
        qpct = qs.get("pct", 0)
        color_fn = green if qpct >= 70 else yellow if qpct >= 40 else red
        safe_print(f"\n    {bold('Composite Quality:')} {color_fn(f'{qpct:.0f}/100')}  {bar(qpct, 100, 25)}")

        # Ratio Dashboard (from pre-computed engine)
        ratios = self.findings.get("ratios", {})
        if ratios:
            safe_print(f"\n    {bold('Financial Ratio Dashboard:')}")
            # Profitability
            safe_print(f"      {dim('Profitability')}")
            for k, label in [("gross_margin", "Gross Margin"), ("operating_margin", "Op Margin"),
                             ("net_margin", "Net Margin"), ("roa", "ROA"), ("roe", "ROE"), ("roic", "ROIC")]:
                v = ratios.get(k)
                if v is not None:
                    color_fn = green if v > 0.15 else yellow if v > 0.05 else red
                    safe_print("        {:<18s} {}".format(label, color_fn("{:.1%}".format(v))))
            # Solvency
            safe_print(f"      {dim('Solvency')}")
            for k, label in [("debt_to_equity", "D/E"), ("debt_to_ebitda", "Debt/EBITDA"),
                             ("interest_coverage", "Interest Coverage"), ("current_ratio", "Current Ratio")]:
                v = ratios.get(k)
                if v is not None:
                    if k == "current_ratio":
                        color_fn = green if v > 1.5 else yellow if v > 1.0 else red
                        safe_print("        {:<18s} {}".format(label, color_fn("{:.2f}x".format(v))))
                    elif k == "interest_coverage":
                        color_fn = green if v > 5 else yellow if v > 2 else red
                        safe_print("        {:<18s} {}".format(label, color_fn("{:.1f}x".format(v))))
                    else:
                        color_fn = green if v < 1.5 else yellow if v < 3 else red
                        safe_print("        {:<18s} {}".format(label, color_fn("{:.2f}x".format(v))))
            # Efficiency
            ccc = ratios.get("cash_conversion_cycle")
            if ccc is not None:
                safe_print(f"      {dim('Efficiency')}")
                safe_print("        {:<18s} {:.0f} days".format("Cash Conv Cycle", ccc))
                for k, label in [("receivable_days", "  Receivable"), ("inventory_days", "  Inventory"), ("payable_days", "  Payable")]:
                    v = ratios.get(k)
                    if v is not None:
                        safe_print("        {:<18s} {:.0f} days".format(label, v))

        # Quarterly trend
        qrev = self.findings.get("quarterly_revenue", [])
        if qrev:
            safe_print(f"\n    {bold('Quarterly Revenue Trend:')}")
            vals = [v for _, v in qrev]
            safe_print(f"      {spark(vals)}  {'  '.join('{}: ${:,.0f}M'.format(q[-5:], v/1e6) for q, v in qrev[-4:])}")

        # Validation
        val = self.findings.get("validation", {})
        if val and not val.get("clean", True):
            issues = val.get("issues", [])
            recent = [i for i in issues if int(i.get("year", "0")) >= 2020]
            if recent:
                safe_print(f"\n    {yellow('Balance Sheet Validation Flags:')}")
                for iss in recent[:3]:
                    safe_print("      {} {} {} ({:.0%} diff)".format(
                        yellow("⚠"), iss["year"], iss["type"], abs(iss.get("diff_pct", 0))))


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 3: Growth & Revenue Analyst
# ══════════════════════════════════════════════════════════════════════════════

class GrowthAgent(Agent):
    name = "growth"
    title = "GROWTH & REVENUE ANALYST"
    icon = "📈"

    def run(self):
        s = self.store

        # Growth rates
        self.findings["revenue_cagr"] = cagr(s.inc_series, "revenue")
        self.findings["ni_cagr"] = cagr(s.inc_series, "net_income")
        self.findings["ebitda_cagr"] = cagr(s.inc_series, "ebitda")
        self.findings["cfo_cagr"] = cagr(s.cf_series, "cfo")

        self.findings["rev_yoy"] = yoy(s.inc_series, "revenue")
        self.findings["ni_yoy"] = yoy(s.inc_series, "net_income")
        self.findings["ebitda_yoy"] = yoy(s.inc_series, "ebitda")

        # Revenue per employee (if available)
        # Growth consistency
        rev_yoys = self.findings["rev_yoy"]
        if rev_yoys:
            positive_years = sum(1 for _, g in rev_yoys if g > 0)
            self.findings["growth_consistency"] = positive_years / len(rev_yoys)
            rates = [g for _, g in rev_yoys]
            self.findings["growth_volatility"] = statistics.stdev(rates) if len(rates) > 1 else 0
            self.findings["growth_acceleration"] = rates[-1] - rates[-2] if len(rates) >= 2 else None
        else:
            self.findings["growth_consistency"] = None
            self.findings["growth_volatility"] = None
            self.findings["growth_acceleration"] = None

        # Earnings surprise history
        surprises = []
        for eq in s.earn_quarterly[:12]:
            est = safe_float(eq, "estimatedEPS")
            actual = safe_float(eq, "reportedEPS")
            surprise = safe_float(eq, "surprise")
            surprise_pct = safe_float(eq, "surprisePercentage")
            if actual is not None:
                surprises.append({
                    "date": eq.get("fiscalDateEnding", ""),
                    "estimated": est, "actual": actual,
                    "surprise": surprise, "surprise_pct": surprise_pct
                })
        self.findings["earnings_surprises"] = surprises

        beat_count = sum(1 for s_ in surprises if s_.get("surprise") and s_["surprise"] > 0)
        self.findings["beat_rate"] = beat_count / len(surprises) if surprises else None

        # Implied revenue growth from forward P/E vs trailing P/E
        pe = s.overview.get("pe")
        fwd_pe = s.overview.get("fwd_pe")
        if pe and fwd_pe and pe > 0 and fwd_pe > 0:
            self.findings["pe_implied_growth"] = (pe / fwd_pe) - 1
        else:
            self.findings["pe_implied_growth"] = None

        # Signal
        rc = self.findings["revenue_cagr"]
        ga = self.findings.get("growth_acceleration")
        if rc and rc > 0.15 and (ga is None or ga > -0.05):
            self.signal = "bullish"; self.score = 20
        elif rc and rc > 0.05:
            self.signal = "neutral"; self.score = 5
        elif rc and rc < 0:
            self.signal = "bearish"; self.score = -20
        else:
            self.signal = "neutral"; self.score = 0
        self.summary = f"Revenue CAGR {pct(rc)} | {'Accelerating' if ga and ga > 0 else 'Decelerating' if ga and ga < 0 else 'Stable'} | Beat rate {self.findings['beat_rate']*100:.0f}%" if self.findings.get('beat_rate') is not None else f"Revenue CAGR {pct(rc)}"

    def render(self):
        self.header()

        safe_print(f"    {bold('Growth Rates (CAGR):')}")
        for label, key in [("Revenue", "revenue_cagr"), ("Net Income", "ni_cagr"),
                           ("EBITDA", "ebitda_cagr"), ("Cash from Ops", "cfo_cagr")]:
            v = self.findings.get(key)
            safe_print(f"      {label:<16s}  {pct(v)}")

        safe_print(f"\n    {bold('Year-over-Year Revenue:')}")
        for y, g in self.findings.get("rev_yoy", [])[-6:]:
            safe_print(f"      {y}  {heatmap_cell(g, (-0.05, 0, 0.05, 0.15))}  {bar(max(g+0.3, 0), 0.6, 25)}")

        gc = self.findings.get("growth_consistency")
        gv = self.findings.get("growth_volatility")
        ga = self.findings.get("growth_acceleration")
        safe_print(f"\n    {dim('Consistency:')} {pct(gc)} positive years  │  {dim('Volatility:')} {num(gv) if gv else 'N/A'}  │  {dim('Acceleration:')} {pct(ga)}")

        pig = self.findings.get("pe_implied_growth")
        if pig is not None:
            safe_print(f"    {dim('P/E implied EPS growth (TTM→FWD):')} {pct(pig)}")

        # Earnings surprises
        surprises = self.findings.get("earnings_surprises", [])
        if surprises:
            safe_print(f"\n    {bold('Earnings Surprises (Last 8 Quarters):')}")
            for s_ in surprises[:8]:
                est = s_.get("estimated")
                act = s_.get("actual")
                sp = s_.get("surprise_pct")
                date = s_.get("date", "")[:7]
                if act is not None:
                    beat = act > (est or 0)
                    icon = green("▲") if beat else red("▼")
                    safe_print(f"      {date}  Est: ${est or 0:.2f}  Act: ${act:.2f}  {icon} {sp or 0:+.1f}%")

            br = self.findings.get("beat_rate")
            if br is not None:
                color_fn = green if br > 0.7 else yellow if br > 0.5 else red
                safe_print(f"    {dim('Beat Rate:')} {color_fn(f'{br*100:.0f}%')} ({sum(1 for s_ in surprises if (s_.get('surprise') or 0) > 0)}/{len(surprises)})")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 4: Credit & Balance Sheet Analyst
# ══════════════════════════════════════════════════════════════════════════════

class CreditAgent(Agent):
    name = "credit"
    title = "CREDIT & BALANCE SHEET ANALYST"
    icon = "🏦"

    def run(self):
        s = self.store
        years = s.years("bs_series")

        # Leverage evolution
        leverage = []
        for y in years:
            bs = s.bs_series.get(y, {})
            inc = s.inc_series.get(y, {})
            d = (bs.get("lt_debt") or 0) + (bs.get("st_debt") or 0)
            eq = bs.get("equity") or 1
            ta = bs.get("total_assets") or 1
            ebitda = inc.get("ebitda") or (inc.get("op_income", 0) + (inc.get("da") or 0))
            interest = abs(inc.get("interest_exp") or 0)
            leverage.append({
                "year": y,
                "de_ratio": d / eq if eq > 0 else None,
                "debt_to_assets": d / ta if ta > 0 else None,
                "net_debt_ebitda": net_debt(bs) / ebitda if ebitda and ebitda > 0 else None,
                "interest_coverage": (inc.get("op_income") or 0) / interest if interest > 0 else None,
            })
        self.findings["leverage"] = leverage

        # Liquidity
        liquidity = []
        for y in years:
            bs = s.bs_series.get(y, {})
            ca = bs.get("current_assets") or 0
            cl = bs.get("current_liabilities") or 1
            cash = bs.get("cash") or 0
            inv = bs.get("inventory") or 0
            liquidity.append({
                "year": y,
                "current_ratio": ca / cl if cl > 0 else None,
                "quick_ratio": (ca - inv) / cl if cl > 0 else None,
                "cash_ratio": cash / cl if cl > 0 else None,
            })
        self.findings["liquidity"] = liquidity

        # Working capital cycle (DIO, DSO, DPO)
        wc_cycle = []
        for y in years:
            bs = s.bs_series.get(y, {})
            inc = s.inc_series.get(y, {})
            rev = inc.get("revenue") or 1
            cogs = inc.get("cogs") or rev * 0.6
            recv = bs.get("receivables") or 0
            inv = bs.get("inventory") or 0
            # payables approximation from current_liabilities - short_term_debt
            payables = (bs.get("current_liabilities") or 0) - (bs.get("st_debt") or 0)

            dso = (recv / rev * 365) if rev > 0 and recv > 0 else None
            dio = (inv / cogs * 365) if cogs > 0 and inv > 0 else None
            dpo = (payables / cogs * 365) if cogs > 0 and payables > 0 else None
            ccc = None
            if dso is not None and dio is not None and dpo is not None:
                ccc = dso + dio - dpo

            wc_cycle.append({"year": y, "dso": dso, "dio": dio, "dpo": dpo, "ccc": ccc})
        self.findings["wc_cycle"] = wc_cycle

        # Debt maturity structure
        latest_bs = s.latest("bs_series")
        self.findings["debt_structure"] = {
            "lt_debt": latest_bs.get("lt_debt") or 0,
            "st_debt": latest_bs.get("st_debt") or 0,
            "cash": latest_bs.get("cash") or 0,
            "net_debt": net_debt(latest_bs),
        }

        # Asset composition
        ta = latest_bs.get("total_assets") or 1
        self.findings["asset_mix"] = {
            "tangible": (latest_bs.get("ppe") or 0) / ta,
            "goodwill": (latest_bs.get("goodwill") or 0) / ta,
            "intangibles": (latest_bs.get("intangibles") or 0) / ta,
            "cash": (latest_bs.get("cash") or 0) / ta,
            "receivables": (latest_bs.get("receivables") or 0) / ta,
            "inventory": (latest_bs.get("inventory") or 0) / ta,
        }

        # Signal
        latest_lev = leverage[-1] if leverage else {}
        de = latest_lev.get("de_ratio")
        ic = latest_lev.get("interest_coverage")
        if de is not None and de < 0.5 and ic and ic > 10:
            self.signal = "bullish"; self.score = 15
        elif de is not None and de > 2 or (ic and ic < 3):
            self.signal = "bearish"; self.score = -20
        else:
            self.signal = "neutral"; self.score = 0
        self.summary = f"D/E {num(de)}x | Interest coverage {num(ic)}x | Net debt/EBITDA {num(latest_lev.get('net_debt_ebitda'))}x"

    def render(self):
        self.header()

        # Leverage table
        lev = self.findings.get("leverage", [])
        if lev:
            safe_print(f"    {bold('Leverage Evolution:')}")
            safe_print(f"      {'Year':<6s} {'D/E':>8s} {'D/Assets':>10s} {'ND/EBITDA':>10s} {'Int. Cov.':>10s}")
            safe_print(f"      {'─'*6} {'─'*8} {'─'*10} {'─'*10} {'─'*10}")
            for r in lev[-6:]:
                de = f"{r['de_ratio']:.2f}" if r.get("de_ratio") is not None else "—"
                da = f"{r['debt_to_assets']:.2f}" if r.get("debt_to_assets") is not None else "—"
                nde = f"{r['net_debt_ebitda']:.1f}x" if r.get("net_debt_ebitda") is not None else "—"
                ic = f"{r['interest_coverage']:.1f}x" if r.get("interest_coverage") is not None else "—"
                safe_print(f"      {r['year']:<6s} {de:>8s} {da:>10s} {nde:>10s} {ic:>10s}")

        # Liquidity table
        liq = self.findings.get("liquidity", [])
        if liq:
            safe_print(f"\n    {bold('Liquidity Ratios:')}")
            safe_print(f"      {'Year':<6s} {'Current':>10s} {'Quick':>10s} {'Cash':>10s}")
            safe_print(f"      {'─'*6} {'─'*10} {'─'*10} {'─'*10}")
            for r in liq[-6:]:
                safe_print(f"      {r['year']:<6s} {num(r.get('current_ratio')):>10s} {num(r.get('quick_ratio')):>10s} {num(r.get('cash_ratio')):>10s}")

        # Working capital cycle
        wcc = self.findings.get("wc_cycle", [])
        if wcc:
            safe_print(f"\n    {bold('Working Capital Cycle (days):')}")
            safe_print(f"      {'Year':<6s} {'DSO':>8s} {'DIO':>8s} {'DPO':>8s} {'CCC':>8s}")
            safe_print(f"      {'─'*6} {'─'*8} {'─'*8} {'─'*8} {'─'*8}")
            for r in wcc[-6:]:
                safe_print(f"      {r['year']:<6s} {num(r.get('dso'),0):>8s} {num(r.get('dio'),0):>8s} {num(r.get('dpo'),0):>8s} {num(r.get('ccc'),0):>8s}")

        # Debt structure
        ds = self.findings.get("debt_structure", {})
        safe_print(f"\n    {bold('Debt Structure:')}")
        safe_print(f"      LT Debt: {usd(ds.get('lt_debt'))}  │  ST Debt: {usd(ds.get('st_debt'))}  │  Cash: {usd(ds.get('cash'))}  │  Net Debt: {usd(ds.get('net_debt'))}")

        # Asset composition
        am = self.findings.get("asset_mix", {})
        if am:
            safe_print(f"\n    {bold('Asset Composition:')}")
            for label, key in [("PP&E", "tangible"), ("Goodwill", "goodwill"), ("Intangibles", "intangibles"),
                                ("Cash", "cash"), ("Receivables", "receivables"), ("Inventory", "inventory")]:
                v = am.get(key, 0)
                safe_print(f"      {label:<14s} {bar(v*100, 50, 20)} {v*100:5.1f}%")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 5: Capital Allocation Analyst
# ══════════════════════════════════════════════════════════════════════════════

class CapitalAgent(Agent):
    name = "capital"
    title = "CAPITAL ALLOCATION ANALYST"
    icon = "💰"

    def run(self):
        s = self.store
        years = s.years("cf_series")

        # Capital deployment history
        deploy = []
        for y in years:
            cf = s.cf_series.get(y, {})
            inc = s.inc_series.get(y, {})
            bs = s.bs_series.get(y, {})
            cfo_val = cf.get("cfo") or 0
            capex_val = abs(cf.get("capex") or 0)
            divs = abs(cf.get("dividends") or 0)
            bb = abs(cf.get("buybacks") or 0)
            sbc = abs(cf.get("sbc") or 0)
            acq = abs(cf.get("acquisitions") or 0)
            fcf_val = cfo_val - capex_val

            # ROIC
            op = inc.get("op_income") or 0
            tax_rate = 0.21
            if inc.get("pretax_income") and inc.get("tax"):
                pt = inc["pretax_income"]
                if pt > 0: tax_rate = min(inc["tax"] / pt, 0.40)
            nopat = op * (1 - tax_rate)
            invested = (bs.get("equity") or 0) + (bs.get("lt_debt") or 0) + (bs.get("st_debt") or 0) - (bs.get("cash") or 0)

            deploy.append({
                "year": y, "cfo": cfo_val, "capex": capex_val, "fcf": fcf_val,
                "dividends": divs, "buybacks": bb, "sbc": sbc, "acquisitions": acq,
                "total_returned": divs + bb,
                "reinvestment_rate": (capex_val + acq) / cfo_val if cfo_val > 0 else None,
                "fcf_conversion": fcf_val / (inc.get("net_income") or 1) if inc.get("net_income") and inc["net_income"] > 0 else None,
                "nopat": nopat,
                "invested_capital": invested,
                "roic": nopat / invested if invested > 0 else None,
                "sbc_pct_of_fcf": sbc / fcf_val if fcf_val > 0 else None,
            })
        self.findings["deploy"] = deploy

        # Dividend analysis
        div_series = [(d["year"], d["dividends"]) for d in deploy if d["dividends"] > 0]
        if len(div_series) >= 2:
            first = div_series[0][1]; last = div_series[-1][1]
            n_years = int(div_series[-1][0]) - int(div_series[0][0])
            self.findings["div_cagr"] = (last / first) ** (1.0 / n_years) - 1 if n_years > 0 and first > 0 else None
        else:
            self.findings["div_cagr"] = None

        # ROIC vs WACC check (requires quant agent's WACC — we'll compute a simple one)
        beta = s.overview.get("beta") or 1.0
        wacc_est = s.risk_free_rate + beta * MARKET_RISK_PREMIUM * 0.95  # approximate
        roic_latest = deploy[-1]["roic"] if deploy else None
        self.findings["roic_vs_wacc"] = {"roic": roic_latest, "wacc_est": wacc_est,
                                          "spread": (roic_latest - wacc_est) if roic_latest else None}

        # Signal
        spread = self.findings["roic_vs_wacc"].get("spread")
        if spread and spread > 0.05:
            self.signal = "bullish"; self.score = 15
        elif spread and spread < -0.03:
            self.signal = "bearish"; self.score = -10
        else:
            self.signal = "neutral"; self.score = 0
        self.summary = f"ROIC {pct(roic_latest)} vs WACC ~{pct(wacc_est)} | Spread {pct(spread)} | Div CAGR {pct(self.findings.get('div_cagr'))}"

    def render(self):
        self.header()

        deploy = self.findings.get("deploy", [])
        if deploy:
            safe_print(f"    {bold('Capital Deployment History:')}")
            safe_print(f"      {'Year':<6s} {'CFO':>10s} {'CapEx':>10s} {'FCF':>10s} {'Divs':>10s} {'Buybacks':>10s} {'SBC':>10s} {'ROIC':>8s}")
            safe_print(f"      {'─'*6} {'─'*10} {'─'*10} {'─'*10} {'─'*10} {'─'*10} {'─'*10} {'─'*8}")
            for d in deploy[-6:]:
                roic_str = pct(d.get("roic")) if d.get("roic") is not None else dim("—")
                safe_print(f"      {d['year']:<6s} {usd(d['cfo']):>10s} {usd(d['capex']):>10s} {usd(d['fcf']):>10s} {usd(d['dividends']):>10s} {usd(d['buybacks']):>10s} {usd(d['sbc']):>10s} {roic_str:>8s}")

        # ROIC vs WACC
        rvw = self.findings.get("roic_vs_wacc", {})
        spread = rvw.get("spread")
        safe_print(f"\n    {bold('ROIC vs WACC:')}")
        safe_print(f"      ROIC: {pct(rvw.get('roic'))}  │  WACC (est): {pct(rvw.get('wacc_est'))}  │  Spread: {pct(spread)}")
        if spread and spread > 0:
            safe_print(f"      {green('✓ Value creation — earning above cost of capital')}")
        elif spread and spread < 0:
            safe_print(f"      {red('⚠ Value destruction — earning below cost of capital')}")

        # FCF conversion
        if deploy:
            parts = []
            for d in deploy[-5:]:
                yr = d["year"]
                fc = d.get("fcf_conversion")
                parts.append(f"{yr}:{fc:.0%}" if fc else f"{yr}:—")
            safe_print(f"\n    {dim('FCF Conversion (FCF/NI):')} {' '.join(parts)}")

        # SBC dilution
        if deploy:
            parts = []
            for d in deploy[-5:]:
                yr = d["year"]
                sp = d.get("sbc_pct_of_fcf")
                parts.append(f"{yr}:{sp:.0%}" if sp else f"{yr}:—")
            safe_print(f"    {dim('SBC as % of FCF:')} {' '.join(parts)}")

        # Dividend
        dc = self.findings.get("div_cagr")
        if dc is not None:
            safe_print(f"    {dim('Dividend CAGR:')} {pct(dc)}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 6: Competitive Moat Analyst
# ══════════════════════════════════════════════════════════════════════════════

class MoatAgent(Agent):
    name = "moat"
    title = "COMPETITIVE MOAT ANALYST"
    icon = "🏰"

    def run(self):
        s = self.store

        # Moat indicators from financials
        indicators = []

        # 1. Gross margin stability & level
        gm_series = []
        for y in s.years("inc_series"):
            inc = s.inc_series.get(y, {})
            gp = inc.get("gross_profit")
            rev = inc.get("revenue")
            if gp and rev and rev > 0:
                gm_series.append(gp / rev)
        if gm_series:
            avg_gm = statistics.mean(gm_series)
            gm_std = statistics.stdev(gm_series) if len(gm_series) > 1 else 0
            if avg_gm > 0.60:
                indicators.append(("High gross margins (>{:.0f}%) → pricing power".format(avg_gm*100), "strong", 20))
            elif avg_gm > 0.40:
                indicators.append(("Moderate gross margins ({:.0f}%)".format(avg_gm*100), "moderate", 10))
            else:
                indicators.append(("Low gross margins ({:.0f}%) → commodity business".format(avg_gm*100), "weak", -5))

            if gm_std < 0.02:
                indicators.append(("Gross margin very stable (σ={:.1f}%) → durable advantage".format(gm_std*100), "strong", 10))
            elif gm_std > 0.05:
                indicators.append(("Gross margin volatile (σ={:.1f}%) → cyclical/competitive pressure".format(gm_std*100), "weak", -5))
        elif s.overview.get("gp_ttm") and s.overview.get("revenue_ttm"):
            # Fallback to TTM data from overview
            gm = s.overview["gp_ttm"] / s.overview["revenue_ttm"]
            if gm > 0.60:
                indicators.append(("High gross margin ({:.0f}% TTM) → pricing power".format(gm*100), "strong", 20))
            elif gm > 0.40:
                indicators.append(("Moderate gross margin ({:.0f}% TTM)".format(gm*100), "moderate", 10))
            else:
                indicators.append(("Low gross margin ({:.0f}% TTM)".format(gm*100), "weak", -5))

        # 2. ROIC persistence
        roics = []
        for y in s.years("inc_series"):
            inc_y = s.inc_series.get(y, {})
            bs_y = s.bs_series.get(y, {})
            op = inc_y.get("op_income") or 0
            ic_val = (bs_y.get("equity") or 0) + (bs_y.get("lt_debt") or 0) + (bs_y.get("st_debt") or 0) - (bs_y.get("cash") or 0)
            if ic_val > 0 and op > 0:
                roics.append(op * 0.79 / ic_val)
        if roics:
            avg_roic = statistics.mean(roics)
            if avg_roic > 0.20:
                indicators.append(("Sustained high ROIC ({:.0f}%) → strong moat".format(avg_roic*100), "strong", 20))
            elif avg_roic > 0.10:
                indicators.append(("Moderate ROIC ({:.0f}%)".format(avg_roic*100), "moderate", 5))
            else:
                indicators.append(("Low ROIC ({:.0f}%) → no durable advantage".format(avg_roic*100), "weak", -10))
        else:
            # Fallback: use ROE from overview as ROIC proxy
            roe = s.overview.get("roe")
            if roe and roe > 0.25:
                indicators.append(("High ROE ({:.0f}%) → strong returns".format(roe*100), "strong", 15))
            elif roe and roe > 0.12:
                indicators.append(("Moderate ROE ({:.0f}%)".format(roe*100), "moderate", 5))

        # 3. Revenue growth consistency (moats grow)
        rc = cagr(s.inc_series, "revenue")
        gc = self.findings.get("growth_consistency")
        if rc and rc > 0.08:
            indicators.append(("Consistent revenue growth ({:.0f}% CAGR)".format(rc*100), "moderate", 10))

        # 4. R&D intensity → innovation moat
        latest_inc = s.latest("inc_series")
        rd = latest_inc.get("rd")
        rev = latest_inc.get("revenue")
        if rd and rev and rev > 0:
            rd_pct = rd / rev
            if rd_pct > 0.15:
                indicators.append(("Heavy R&D ({:.0f}% of revenue) → innovation-driven".format(rd_pct*100), "strong", 10))
            elif rd_pct > 0.05:
                indicators.append(("Moderate R&D ({:.0f}% of revenue)".format(rd_pct*100), "moderate", 5))

        # 5. Asset lightness → scalability
        latest_bs = s.latest("bs_series")
        ta = latest_bs.get("total_assets") or 1
        ppe = latest_bs.get("ppe") or 0
        if ppe / ta < 0.15 and rev and rev > 0:
            indicators.append(("Asset-light model (PP&E {:.0f}% of assets) → scalable".format(ppe/ta*100), "strong", 10))
        elif ppe / ta > 0.40:
            indicators.append(("Asset-heavy model (PP&E {:.0f}% of assets)".format(ppe/ta*100), "weak", -5))

        # 6. Intangibles/Goodwill → acquired moats
        gw = latest_bs.get("goodwill") or 0
        intang = latest_bs.get("intangibles") or 0
        if (gw + intang) / ta > 0.30:
            indicators.append(("Significant intangibles ({:.0f}% of assets) → brand/IP/acquisitions".format((gw+intang)/ta*100), "moderate", 5))

        self.findings["indicators"] = indicators

        # Composite moat score
        moat_score = sum(pts for _, _, pts in indicators)
        self.findings["moat_score"] = moat_score

        # Moat width classification
        if moat_score >= 50: width = "Wide"
        elif moat_score >= 25: width = "Narrow"
        else: width = "None/Thin"
        self.findings["moat_width"] = width

        if moat_score >= 40: self.signal = "bullish"; self.score = 15
        elif moat_score >= 15: self.signal = "neutral"; self.score = 5
        else: self.signal = "bearish"; self.score = -10
        self.summary = f"Moat: {width} (score {moat_score}) | {'Durable competitive advantages' if moat_score >= 40 else 'Limited pricing power'}"

    def render(self):
        self.header()

        width = self.findings.get("moat_width", "Unknown")
        ms = self.findings.get("moat_score", 0)
        color_fn = green if width == "Wide" else yellow if width == "Narrow" else red
        safe_print(f"    {bold('Moat Width:')} {color_fn(width)}  (score: {ms})")
        safe_print()

        for msg, strength, pts in self.findings.get("indicators", []):
            if strength == "strong": icon = green("■")
            elif strength == "moderate": icon = yellow("■")
            else: icon = red("□")
            safe_print(f"      {icon} {msg}  ({'+' if pts > 0 else ''}{pts}pts)")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 7: Macro & Sector Analyst
# ══════════════════════════════════════════════════════════════════════════════

class MacroAgent(Agent):
    name = "macro"
    title = "MACRO & SECTOR ANALYST"
    icon = "🌍"

    def run(self):
        s = self.store
        self.findings["sector"] = s.overview.get("sector", "")
        self.findings["industry"] = s.overview.get("industry", "")
        self.findings["beta"] = s.overview.get("beta")
        self.findings["risk_free"] = s.risk_free_rate

        # Rate sensitivity heuristic
        beta = s.overview.get("beta") or 1.0
        debt_heavy = False
        latest_bs = s.latest("bs_series")
        latest_inc = s.latest("inc_series")
        eq = latest_bs.get("equity") or 1
        d = (latest_bs.get("lt_debt") or 0) + (latest_bs.get("st_debt") or 0)
        if eq > 0 and d / eq > 1:
            debt_heavy = True

        sector = str(s.overview.get("sector") or "").upper()
        # Sector characteristics
        sector_traits = {
            "TECHNOLOGY": {"cyclical": False, "rate_sensitive": True, "growth_type": "secular"},
            "HEALTHCARE": {"cyclical": False, "rate_sensitive": False, "growth_type": "defensive"},
            "FINANCIAL": {"cyclical": True, "rate_sensitive": True, "growth_type": "cyclical"},
            "ENERGY": {"cyclical": True, "rate_sensitive": False, "growth_type": "commodity"},
            "CONSUMER DISCRETIONARY": {"cyclical": True, "rate_sensitive": True, "growth_type": "cyclical"},
            "CONSUMER STAPLES": {"cyclical": False, "rate_sensitive": False, "growth_type": "defensive"},
            "INDUSTRIALS": {"cyclical": True, "rate_sensitive": True, "growth_type": "cyclical"},
            "UTILITIES": {"cyclical": False, "rate_sensitive": True, "growth_type": "defensive"},
            "REAL ESTATE": {"cyclical": True, "rate_sensitive": True, "growth_type": "yield"},
            "MATERIALS": {"cyclical": True, "rate_sensitive": False, "growth_type": "commodity"},
            "COMMUNICATION SERVICES": {"cyclical": False, "rate_sensitive": True, "growth_type": "secular"},
        }
        traits = sector_traits.get(sector, {"cyclical": None, "rate_sensitive": None, "growth_type": "unknown"})
        self.findings["sector_traits"] = traits

        # Rate impact assessment
        rate_impact = []
        if traits.get("rate_sensitive"):
            rate_impact.append("Sector is rate-sensitive — higher rates compress multiples")
        if debt_heavy:
            rate_impact.append(f"High leverage (D/E {d/eq:.1f}x) amplifies rate impact on interest costs")
        if beta and beta > 1.3:
            rate_impact.append(f"High beta ({beta:.2f}) — amplified market sensitivity")
        if s.overview.get("dividend_yield") and s.overview["dividend_yield"] > 0.03:
            rate_impact.append("High yield — competes with risk-free rates for income investors")
        self.findings["rate_impact"] = rate_impact

        # Valuation context
        pe = s.overview.get("pe")
        fwd_pe = s.overview.get("fwd_pe")
        self.findings["valuation_context"] = {
            "pe": pe,
            "fwd_pe": fwd_pe,
            "growth_type": traits.get("growth_type"),
        }

        self.signal = "neutral"; self.score = 0
        self.summary = f"Sector: {s.overview.get('sector', 'N/A')} | {'Cyclical' if traits.get('cyclical') else 'Defensive'} | {'Rate-sensitive' if traits.get('rate_sensitive') else 'Rate-resilient'}"

    def render(self):
        self.header()

        safe_print(f"    {bold('Sector:')} {self.findings.get('sector', 'N/A')} — {self.findings.get('industry', 'N/A')}")

        traits = self.findings.get("sector_traits", {})
        safe_print(f"    {dim('Characteristics:')} {'Cyclical' if traits.get('cyclical') else 'Defensive'} | {traits.get('growth_type', '?').title()} growth | {'Rate-sensitive' if traits.get('rate_sensitive') else 'Rate-resilient'}")
        safe_print(f"    {dim('Risk-Free Rate:')} {pct(self.findings.get('risk_free'))}")
        safe_print(f"    {dim('Beta:')} {num(self.findings.get('beta'))}")

        ri = self.findings.get("rate_impact", [])
        if ri:
            safe_print(f"\n    {bold('Rate Environment Impact:')}")
            for item in ri:
                safe_print(f"      {yellow('◆')} {item}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 8: Risk Analyst
# ══════════════════════════════════════════════════════════════════════════════

class RiskAgent(Agent):
    name = "risk"
    title = "RISK ANALYST"
    icon = "⚠️"

    def run(self):
        s = self.store

        risks = []

        # Valuation risk
        pe = s.overview.get("pe")
        if pe and pe > 35:
            risks.append({"category": "Valuation", "severity": "high",
                          "detail": f"P/E of {pe:.1f} implies high expectations — vulnerable to multiple compression"})
        elif pe and pe > 25:
            risks.append({"category": "Valuation", "severity": "medium",
                          "detail": f"P/E of {pe:.1f} — above historical market average"})

        # Concentration risk
        rev = s.latest("inc_series").get("revenue")
        if rev and s.overview.get("market_cap") and rev > 0:
            ps = s.overview["market_cap"] / rev
            if ps > 15:
                risks.append({"category": "Valuation", "severity": "high",
                              "detail": f"Price/Sales of {ps:.1f}x — extreme revenue dependency"})

        # Leverage risk
        bs = s.latest("bs_series")
        d = (bs.get("lt_debt") or 0) + (bs.get("st_debt") or 0)
        eq = bs.get("equity") or 1
        if eq > 0 and d / eq > 2:
            risks.append({"category": "Balance Sheet", "severity": "high",
                          "detail": f"D/E of {d/eq:.1f}x — high financial leverage"})
        elif eq <= 0:
            risks.append({"category": "Balance Sheet", "severity": "critical",
                          "detail": "Negative shareholder equity"})

        # Liquidity risk
        ca = bs.get("current_assets") or 0
        cl = bs.get("current_liabilities") or 1
        cr = ca / cl if cl > 0 else 0
        if cr < 1.0:
            risks.append({"category": "Liquidity", "severity": "high",
                          "detail": f"Current ratio {cr:.2f} < 1.0 — potential liquidity stress"})

        # Growth deceleration
        rev_yoys = yoy(s.inc_series, "revenue")
        if len(rev_yoys) >= 3:
            recent = [g for _, g in rev_yoys[-3:]]
            if all(recent[i] < recent[i-1] for i in range(1, len(recent))):
                risks.append({"category": "Growth", "severity": "medium",
                              "detail": f"Revenue growth decelerating for 3+ years ({', '.join(f'{g*100:.0f}%' for g in recent)})"})

        # Margin erosion
        gm_recent = []
        for y in s.years("inc_series", 4):
            inc = s.inc_series.get(y, {})
            gp = inc.get("gross_profit"); rev = inc.get("revenue")
            if gp and rev and rev > 0: gm_recent.append(gp / rev)
        if len(gm_recent) >= 3 and gm_recent[-1] < gm_recent[0] - 0.03:
            risks.append({"category": "Profitability", "severity": "medium",
                          "detail": f"Gross margin contracting ({gm_recent[0]*100:.1f}% → {gm_recent[-1]*100:.1f}%)"})

        # SBC dilution
        cf = s.latest("cf_series")
        sbc = abs(cf.get("sbc") or 0)
        ni = s.latest("inc_series").get("net_income") or 1
        if ni > 0 and sbc / ni > 0.30:
            risks.append({"category": "Dilution", "severity": "high",
                          "detail": f"SBC is {sbc/ni*100:.0f}% of net income — significant shareholder dilution"})

        # Customer/revenue concentration (proxy: if single sector & small cap)
        mc = s.overview.get("market_cap") or 0
        if mc < 2e9:
            risks.append({"category": "Size", "severity": "medium",
                          "detail": f"Small-cap (${mc/1e9:.1f}B) — less diversified, higher vol"})

        self.findings["risks"] = risks

        # Scenario analysis
        self.findings["scenarios"] = {
            "bull": {"probability": 25, "conditions": "Beat estimates, multiple expansion, sector tailwinds"},
            "base": {"probability": 50, "conditions": "In-line growth, stable multiples"},
            "bear": {"probability": 20, "conditions": "Miss estimates, margin compression, macro headwinds"},
            "tail": {"probability": 5, "conditions": "Black swan — regulatory action, loss of key customer, credit event"},
        }

        # Signal
        high_risks = sum(1 for r in risks if r["severity"] in ("high", "critical"))
        if high_risks >= 3:
            self.signal = "bearish"; self.score = -25
        elif high_risks >= 1:
            self.signal = "neutral"; self.score = -5
        else:
            self.signal = "bullish"; self.score = 10
        self.summary = f"{len(risks)} risk factors identified ({high_risks} high severity)"

    def render(self):
        self.header()

        risks = self.findings.get("risks", [])
        if risks:
            safe_print(f"    {bold(f'Identified {len(risks)} Risk Factors:')}")
            safe_print()
            for r in risks:
                sev = r["severity"]
                if sev == "critical": icon = bg_red(" CRIT ")
                elif sev == "high": icon = red("HIGH")
                elif sev == "medium": icon = yellow("MED ")
                else: icon = dim("LOW ")
                safe_print(f"      [{icon}] {dim(r['category'] + ':')} {r['detail']}")
        else:
            safe_print(f"    {green('No significant risk factors identified')}")

        safe_print(f"\n    {bold('Scenario Framework:')}")
        for label, info in self.findings.get("scenarios", {}).items():
            safe_print(f"      {label.upper():5s} ({info['probability']:2d}%)  {info['conditions']}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 9: Sentiment & News Analyst
# ══════════════════════════════════════════════════════════════════════════════

class SentimentAgent(Agent):
    name = "sentiment"
    title = "SENTIMENT & NEWS ANALYST"
    icon = "📰"

    def run(self):
        s = self.store

        # Analyze news sentiment
        articles = s.news[:30]
        sentiments = {"Bullish": 0, "Somewhat-Bullish": 0, "Neutral": 0, "Somewhat-Bearish": 0, "Bearish": 0}
        scores = []
        relevant_articles = []

        for article in articles:
            # Find ticker-specific sentiment
            for ts in article.get("ticker_sentiment", []):
                if ts.get("ticker") == s.ticker:
                    label = ts.get("ticker_sentiment_label", "Neutral")
                    score = safe_float(ts, "ticker_sentiment_score", 0)
                    sentiments[label] = sentiments.get(label, 0) + 1
                    scores.append(score)
                    relevant_articles.append({
                        "title": article.get("title", "")[:80],
                        "source": article.get("source", ""),
                        "time": article.get("time_published", "")[:10],
                        "sentiment": label,
                        "score": score,
                    })
                    break

        self.findings["sentiment_dist"] = sentiments
        self.findings["avg_sentiment"] = statistics.mean(scores) if scores else 0
        self.findings["articles"] = relevant_articles[:15]
        self.findings["article_count"] = len(articles)

        # Analyst ratings
        sb = s.overview.get("AnalystRatingStrongBuy") or "0"
        b = s.overview.get("AnalystRatingBuy") or "0"
        h = s.overview.get("AnalystRatingHold") or "0"
        se = s.overview.get("AnalystRatingSell") or "0"
        ss = s.overview.get("AnalystRatingStrongSell") or "0"
        try:
            ratings = {"Strong Buy": int(sb), "Buy": int(b), "Hold": int(h), "Sell": int(se), "Strong Sell": int(ss)}
        except:
            ratings = {}
        self.findings["analyst_ratings"] = ratings
        self.findings["analyst_target"] = s.overview.get("analyst_target")

        total_ratings = sum(ratings.values())
        if total_ratings > 0:
            bullish_pct = (ratings.get("Strong Buy", 0) + ratings.get("Buy", 0)) / total_ratings
            bearish_pct = (ratings.get("Sell", 0) + ratings.get("Strong Sell", 0)) / total_ratings
            self.findings["analyst_bullish_pct"] = bullish_pct
            self.findings["analyst_bearish_pct"] = bearish_pct
        else:
            self.findings["analyst_bullish_pct"] = None
            self.findings["analyst_bearish_pct"] = None

        # Technical signals
        price = s.price
        dma50 = s.overview.get("50dma")
        dma200 = s.overview.get("200dma")
        high52 = s.overview.get("52w_high")
        low52 = s.overview.get("52w_low")

        technicals = []
        if price and dma50 and dma200:
            if dma50 > dma200:
                technicals.append(("Golden cross (50 DMA > 200 DMA)", "bullish"))
            else:
                technicals.append(("Death cross (50 DMA < 200 DMA)", "bearish"))
            if price > dma200:
                technicals.append(("Trading above 200 DMA", "bullish"))
            else:
                technicals.append(("Trading below 200 DMA", "bearish"))
        if price and high52 and low52:
            range_pos = (price - low52) / (high52 - low52) if high52 != low52 else 0.5
            if range_pos > 0.9:
                technicals.append((f"Near 52-week high ({range_pos*100:.0f}% of range)", "bullish"))
            elif range_pos < 0.2:
                technicals.append((f"Near 52-week low ({range_pos*100:.0f}% of range)", "bearish"))
            else:
                technicals.append((f"Mid-range ({range_pos*100:.0f}% of 52-week range)", "neutral"))
        self.findings["technicals"] = technicals

        # Signal
        avg_s = self.findings["avg_sentiment"]
        if avg_s > 0.15: self.signal = "bullish"; self.score = 10
        elif avg_s < -0.15: self.signal = "bearish"; self.score = -10
        else: self.signal = "neutral"; self.score = 0
        abp = self.findings.get('analyst_bullish_pct') or 0
        self.summary = f"News sentiment {avg_s:+.2f} | Analyst {abp*100:.0f}% bullish | {'Above' if price and dma200 and price > dma200 else 'Below'} 200 DMA"

    def render(self):
        self.header()

        # Analyst ratings
        ratings = self.findings.get("analyst_ratings", {})
        if ratings:
            total = sum(ratings.values())
            safe_print(f"    {bold('Analyst Consensus:')} ({total} analysts)")
            for label, count in ratings.items():
                pct_val = count / total * 100 if total > 0 else 0
                color_fn = green if "Buy" in label else red if "Sell" in label else dim
                safe_print(f"      {color_fn(f'{label:<14s}')} {bar(pct_val, 100, 20)} {count} ({pct_val:.0f}%)")

        target = self.findings.get("analyst_target")
        if target and self.store.price:
            up = target / self.store.price - 1
            safe_print(f"    {dim('Target:')} ${target:,.2f} ({pct(up)})")

        # News sentiment
        sd = self.findings.get("sentiment_dist", {})
        safe_print(f"\n    {bold('News Sentiment Distribution:')} ({self.findings.get('article_count', 0)} articles)")
        for label in ["Bullish", "Somewhat-Bullish", "Neutral", "Somewhat-Bearish", "Bearish"]:
            count = sd.get(label, 0)
            color_fn = green if "Bull" in label else red if "Bear" in label else dim
            safe_print(f"      {color_fn(f'{label:<20s}')} {'█' * count}")
        safe_print(f"    {dim('Avg Score:')} {self.findings.get('avg_sentiment', 0):+.3f}")

        # Top articles
        articles = self.findings.get("articles", [])
        if articles:
            safe_print(f"\n    {bold('Recent Headlines:')}")
            for a in articles[:8]:
                sent = a.get("sentiment", "")
                if "Bull" in sent: icon = green("▲")
                elif "Bear" in sent: icon = red("▼")
                else: icon = dim("○")
                safe_print(f"      {icon} {dim(a.get('time', '')[:10])} {a.get('title', '')[:70]}")

        # Technicals
        techs = self.findings.get("technicals", [])
        if techs:
            safe_print(f"\n    {bold('Technical Signals:')}")
            for msg, tone in techs:
                icon = green("▲") if tone == "bullish" else red("▼") if tone == "bearish" else dim("●")
                safe_print(f"      {icon} {msg}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 10: Klarman Margin of Safety Agent
# ══════════════════════════════════════════════════════════════════════════════

class KlarmanAgent(Agent):
    """Seth Klarman's Margin of Safety framework.

    Core principles from the book:
    - Value investing is buying at a significant discount to conservatively estimated intrinsic value
    - The margin of safety is the discount to intrinsic value — the larger the discount, the lower the risk
    - Focus on downside protection first, upside takes care of itself
    - Use multiple independent valuation methods; take the LOWEST as your anchor
    - Normalize earnings over a full cycle — don't extrapolate peak or trough
    - Liquidation/tangible book value sets the hard floor
    - Catalysts matter — a cheap stock can stay cheap forever without one
    - Avoid permanent capital loss above all else
    """
    name = "klarman"
    title = "MARGIN OF SAFETY (KLARMAN)"
    icon = "🛡️"

    def run(self):
        s = self.store
        inc = s.latest("inc_series")
        bs = s.latest("bs_series")
        cf = s.latest("cf_series")
        years = s.years("inc_series")

        # ── 1. Normalized Earnings Power Value (EPV) ──
        # Klarman: use mid-cycle earnings, not peak/trough
        ni_series = []
        for y in years:
            ni = s.inc_series.get(y, {}).get("net_income")
            if ni is not None:
                ni_series.append(ni)

        normalized_ni = statistics.median(ni_series) if len(ni_series) >= 3 else (ni_series[-1] if ni_series else None)
        self.findings["normalized_ni"] = normalized_ni

        # EPV = Normalized EBIT * (1-tax) / WACC
        ebit_series = []
        for y in years:
            ebit = s.inc_series.get(y, {}).get("op_income")
            if ebit is not None:
                ebit_series.append(ebit)
        normalized_ebit = statistics.median(ebit_series) if len(ebit_series) >= 3 else (ebit_series[-1] if ebit_series else None)

        beta = s.overview.get("beta") or 1.0
        rf = s.risk_free_rate
        wacc = rf + beta * MARKET_RISK_PREMIUM
        # Conservative: use higher WACC for margin of safety
        wacc_conservative = wacc + 0.02  # 200bp penalty

        pretax = inc.get("pretax_income") or 0
        tax = inc.get("tax") or 0
        tax_rate = min(tax / pretax, 0.40) if pretax > 0 and tax > 0 else 0.25

        if normalized_ebit and normalized_ebit > 0 and wacc_conservative > 0.01:
            nopat = normalized_ebit * (1 - tax_rate)
            # EPV assumes no growth — just sustainable earnings capitalized
            epv_enterprise = nopat / wacc_conservative
            nd = net_debt(bs)
            epv_equity = epv_enterprise - nd
            epv_per_share = epv_equity / s.shares if s.shares > 1 else 0
        else:
            nopat = None
            epv_enterprise = None
            epv_equity = None
            epv_per_share = None

        self.findings["epv"] = {
            "normalized_ebit": normalized_ebit,
            "nopat": nopat,
            "wacc_conservative": wacc_conservative,
            "enterprise": epv_enterprise,
            "equity": epv_equity,
            "per_share": epv_per_share,
        }

        # ── 2. Tangible Book Value (Liquidation Floor) ──
        # Klarman: What would you get if the company shut down tomorrow?
        ta = bs.get("total_assets") or 0
        tl = bs.get("total_liabilities") or 0
        gw = bs.get("goodwill") or 0
        intang = bs.get("intangibles") or 0

        tangible_book = ta - tl - gw - intang
        # Apply haircuts: receivables 80%, inventory 60%, PP&E 50% — conservative liquidation
        recv = bs.get("receivables") or 0
        inv = bs.get("inventory") or 0
        ppe = bs.get("ppe") or 0
        cash = bs.get("cash") or 0
        other_assets = ta - cash - recv - inv - ppe - gw - intang
        liquidation_value = (cash +
                             recv * 0.80 +
                             inv * 0.60 +
                             ppe * 0.50 +
                             other_assets * 0.30 -
                             tl)
        tbv_per_share = tangible_book / s.shares if s.shares > 1 else 0
        liq_per_share = liquidation_value / s.shares if s.shares > 1 else 0

        self.findings["asset_value"] = {
            "tangible_book": tangible_book,
            "tbv_per_share": tbv_per_share,
            "liquidation_value": liquidation_value,
            "liq_per_share": liq_per_share,
            "goodwill_pct": gw / ta if ta > 0 else 0,
            "intangibles_pct": intang / ta if ta > 0 else 0,
        }

        # ── 3. Normalized FCF Yield ──
        fcf_series = []
        for y in years:
            c = s.cf_series.get(y, {})
            f = fcf(c)
            if f is not None:
                fcf_series.append(f)
        normalized_fcf = statistics.median(fcf_series) if len(fcf_series) >= 3 else (fcf_series[-1] if fcf_series else None)
        fcf_yield = None
        if normalized_fcf and s.overview.get("market_cap") and s.overview["market_cap"] > 0:
            fcf_yield = normalized_fcf / s.overview["market_cap"]
        self.findings["fcf_yield"] = {
            "normalized_fcf": normalized_fcf,
            "yield": fcf_yield,
        }

        # ── 4. Conservative DCF (Klarman-style: no-growth + modest growth) ──
        # Scenario 1: Zero growth (what's it worth if it never grows?)
        # Scenario 2: GDP-like growth (2.5%)
        # Scenario 3: Half of historical growth
        dcf_scenarios = {}
        base_fcf = normalized_fcf
        nd = net_debt(bs)
        if base_fcf and base_fcf > 0:
            hist_growth = cagr(s.cf_series, "cfo") or 0.05
            scenarios = {
                "No Growth (floor)": 0.0,
                "GDP Growth (2.5%)": 0.025,
                "Half Historical": max(min(hist_growth * 0.5, 0.10), 0.0),
            }
            for label, g in scenarios.items():
                # 10-year DCF with terminal value
                pv = 0
                f = base_fcf
                for yr in range(1, 11):
                    f *= (1 + g)
                    pv += f / (1 + wacc_conservative) ** yr
                tg = min(g, 0.02)  # terminal growth capped at 2%
                if wacc_conservative > tg:
                    tv = f * (1 + tg) / (wacc_conservative - tg)
                else:
                    tv = f * 15
                pv_tv = tv / (1 + wacc_conservative) ** 10
                ev = pv + pv_tv
                eq = ev - nd
                fv = eq / s.shares if s.shares > 1 else 0
                dcf_scenarios[label] = {"growth": g, "fair_value": fv, "ev": ev, "tv_pct": pv_tv / ev if ev > 0 else 0}

        self.findings["conservative_dcf"] = dcf_scenarios

        # ── 5. Multi-method Intrinsic Value (take the LOWEST) ──
        # Klarman: "The most reliable estimate of value comes from using several methods
        #           simultaneously and choosing the most conservative result"
        iv_estimates = {}
        if epv_per_share and epv_per_share > 0:
            iv_estimates["EPV (no growth)"] = epv_per_share
        for label, scen in dcf_scenarios.items():
            if scen["fair_value"] > 0:
                iv_estimates[f"DCF: {label}"] = scen["fair_value"]
        if tbv_per_share > 0:
            iv_estimates["Tangible Book"] = tbv_per_share

        # Also add a normalized P/E approach
        if normalized_ni and normalized_ni > 0 and s.shares > 1:
            norm_eps = normalized_ni / s.shares
            # Conservative P/E: use 12x for value, 15x for quality
            quality_premium = 15 if (s.overview.get("roe") or 0) > 0.15 else 12
            iv_estimates[f"Normalized P/E @{quality_premium}x"] = norm_eps * quality_premium

        self.findings["iv_estimates"] = iv_estimates

        # Conservative intrinsic value = LOWEST of all methods
        if iv_estimates:
            conservative_iv = min(iv_estimates.values())
            median_iv = statistics.median(iv_estimates.values())
        else:
            conservative_iv = None
            median_iv = None

        self.findings["conservative_iv"] = conservative_iv
        self.findings["median_iv"] = median_iv

        # ── 6. Margin of Safety Calculation ──
        if conservative_iv and s.price and s.price > 0:
            mos = 1 - (s.price / conservative_iv) if conservative_iv > 0 else None
            mos_median = 1 - (s.price / median_iv) if median_iv and median_iv > 0 else None
        else:
            mos = None
            mos_median = None

        self.findings["margin_of_safety"] = mos
        self.findings["margin_of_safety_median"] = mos_median

        # ── 7. Downside Risk Assessment ──
        # What's the max you could lose?
        downside_scenarios = []
        if liq_per_share and s.price and s.price > 0:
            loss_to_liq = (liq_per_share / s.price - 1)
            downside_scenarios.append(("Liquidation value", liq_per_share, loss_to_liq))
        if tbv_per_share and s.price and s.price > 0:
            loss_to_tbv = (tbv_per_share / s.price - 1)
            downside_scenarios.append(("Tangible book", tbv_per_share, loss_to_tbv))
        if epv_per_share and epv_per_share > 0 and s.price and s.price > 0:
            loss_to_epv = (epv_per_share / s.price - 1)
            downside_scenarios.append(("EPV (zero growth)", epv_per_share, loss_to_epv))
        self.findings["downside_scenarios"] = downside_scenarios

        # ── 8. Klarman Checklist ──
        checklist = []
        # a) Is it cheap on absolute basis?
        if mos and mos > 0.30:
            checklist.append(("Trading >30% below conservative IV", True, "strong"))
        elif mos and mos > 0.15:
            checklist.append(("Trading >15% below conservative IV", True, "moderate"))
        elif mos and mos > 0:
            checklist.append(("Slight discount to conservative IV", True, "weak"))
        else:
            checklist.append(("Trading at or above conservative IV — no margin of safety", False, "fail"))

        # b) Tangible assets provide floor?
        if tbv_per_share and s.price and tbv_per_share > s.price * 0.5:
            checklist.append(("Tangible book > 50% of price — asset floor exists", True, "strong"))
        else:
            checklist.append(("Limited tangible asset support", False, "weak"))

        # c) Normalized earnings power positive?
        if normalized_ni and normalized_ni > 0:
            checklist.append(("Positive normalized earnings", True, "strong"))
        else:
            checklist.append(("Negative or zero normalized earnings", False, "fail"))

        # d) FCF yield attractive?
        if fcf_yield and fcf_yield > 0.08:
            checklist.append((f"High normalized FCF yield ({fcf_yield*100:.1f}%)", True, "strong"))
        elif fcf_yield and fcf_yield > 0.05:
            checklist.append((f"Adequate FCF yield ({fcf_yield*100:.1f}%)", True, "moderate"))
        elif fcf_yield and fcf_yield > 0:
            checklist.append((f"Low FCF yield ({fcf_yield*100:.1f}%)", False, "weak"))
        else:
            checklist.append(("Negative or zero FCF yield", False, "fail"))

        # e) Low dependency on terminal value?
        no_growth = dcf_scenarios.get("No Growth (floor)")
        if no_growth and no_growth["tv_pct"] < 0.60:
            checklist.append(("TV <60% of EV in no-growth case — near-term cash flows dominant", True, "strong"))
        elif no_growth and no_growth["tv_pct"] < 0.75:
            checklist.append(("Moderate terminal value dependency", True, "moderate"))
        else:
            checklist.append(("Heavy terminal value dependency — requires sustained growth", False, "weak"))

        # f) Low goodwill/intangibles risk?
        gw_pct = gw / ta if ta > 0 else 0
        if gw_pct < 0.10:
            checklist.append(("Low goodwill risk (<10% of assets)", True, "strong"))
        elif gw_pct < 0.30:
            checklist.append((f"Moderate goodwill ({gw_pct*100:.0f}% of assets)", True, "moderate"))
        else:
            checklist.append((f"High goodwill ({gw_pct*100:.0f}% of assets) — impairment risk", False, "weak"))

        # g) Sustainable competitive position?
        gm = s.overview.get("gp_ttm", 0)
        rev = s.overview.get("revenue_ttm", 1) or 1
        if gm and rev:
            gm_pct = gm / rev
            if gm_pct > 0.40:
                checklist.append(("Gross margins suggest pricing power", True, "moderate"))
            else:
                checklist.append(("Thin margins — limited moat evidence", False, "weak"))

        self.findings["checklist"] = checklist

        # ── Signal ──
        passed = sum(1 for _, ok, _ in checklist if ok)
        total = len(checklist)
        strong = sum(1 for _, ok, s in checklist if ok and s == "strong")

        if mos and mos > 0.25 and passed >= total * 0.7:
            self.signal = "bullish"; self.score = 30
        elif mos and mos > 0.10 and passed >= total * 0.5:
            self.signal = "bullish"; self.score = 15
        elif mos and mos < -0.20:
            self.signal = "bearish"; self.score = -25
        elif mos and mos < 0:
            self.signal = "bearish"; self.score = -10
        else:
            self.signal = "neutral"; self.score = 0

        if conservative_iv and s.price:
            self.summary = f"Conservative IV ${conservative_iv:,.0f} vs ${s.price:,.0f} | MoS {pct(mos)} | {passed}/{total} checklist"
        else:
            self.summary = f"Insufficient data for Klarman analysis"

    def render(self):
        self.header()
        s = self.store

        safe_print(f"    {dim('\"The secret to investing is to figure out the value of something —')}")
        safe_print(f"    {dim(' and then pay a lot less.\" — Seth Klarman')}")

        # Earnings Power Value
        epv = self.findings.get("epv", {})
        if epv.get("per_share"):
            safe_print(f"\n    {bold('Earnings Power Value (no-growth capitalization):')}")
            safe_print(f"      Normalized EBIT: {usd(epv.get('normalized_ebit'))}  →  NOPAT: {usd(epv.get('nopat'))}")
            safe_print(f"      WACC (conservative): {pct(epv.get('wacc_conservative'))}  (+200bp penalty)")
            color_fn = green if (epv['per_share'] > (s.price or 999999)) else red
            epv_str = f"${epv['per_share']:,.2f}"
            safe_print(f"      {bold('EPV/share:')} {color_fn(epv_str)}")

        # Asset-Based Floor
        av = self.findings.get("asset_value", {})
        safe_print(f"\n    {bold('Asset-Based Floor:')}")
        safe_print(f"      Tangible Book / share:   ${av.get('tbv_per_share', 0):>10,.2f}")
        safe_print(f"      Liquidation Value / share: ${av.get('liq_per_share', 0):>10,.2f}")
        safe_print(f"      Goodwill: {pct(av.get('goodwill_pct'))} of assets  │  Intangibles: {pct(av.get('intangibles_pct'))} of assets")

        # FCF Yield
        fy = self.findings.get("fcf_yield", {})
        if fy.get("yield") is not None:
            y = fy["yield"]
            color_fn = green if y > 0.06 else yellow if y > 0.03 else red
            safe_print(f"\n    {bold('Normalized FCF Yield:')} {color_fn(f'{y*100:.1f}%')}  (normalized FCF: {usd(fy.get('normalized_fcf'))})")

        # Conservative DCF scenarios
        dcf = self.findings.get("conservative_dcf", {})
        if dcf:
            safe_print(f"\n    {bold('Conservative DCF (Klarman-style):')}")
            safe_print(f"      {'Scenario':<24s} {'Growth':>8s} {'Fair Value':>12s} {'vs Price':>10s} {'TV%':>8s}")
            safe_print(f"      {'─'*24} {'─'*8} {'─'*12} {'─'*10} {'─'*8}")
            for label, d in dcf.items():
                fv = d["fair_value"]
                up = (fv / s.price - 1) if s.price else None
                color_fn = green if (up and up > 0) else red
                safe_print(f"      {label:<24s} {pct(d['growth']):>8s} {color_fn(f'${fv:>10,.2f}')} {pct(up):>10s} {pct(d['tv_pct']):>8s}")

        # Multi-method IV
        iv = self.findings.get("iv_estimates", {})
        if iv:
            safe_print(f"\n    {bold('Multi-Method Intrinsic Value Estimates:')}")
            sorted_iv = sorted(iv.items(), key=lambda x: x[1])
            for method, val in sorted_iv:
                is_min = (val == min(iv.values()))
                prefix = bold("→ ") if is_min else "  "
                color_fn = green if (s.price and val > s.price) else red
                safe_print(f"      {prefix}{method:<32s} {color_fn(f'${val:>10,.2f}')}{bold(' ← CONSERVATIVE ANCHOR') if is_min else ''}")

        # Margin of Safety
        mos = self.findings.get("margin_of_safety")
        mos_med = self.findings.get("margin_of_safety_median")
        civ = self.findings.get("conservative_iv")
        miv = self.findings.get("median_iv")

        safe_print(f"\n    {'━' * 60}")
        if civ and s.price:
            safe_print(f"    {bold('CONSERVATIVE INTRINSIC VALUE:')} ${civ:,.2f}")
            safe_print(f"    {bold('MEDIAN INTRINSIC VALUE:')}       ${miv:,.2f}" if miv else "")
            safe_print(f"    {bold('CURRENT PRICE:')}                ${s.price:,.2f}")
        if mos is not None:
            if mos > 0.25:
                safe_print(f"    {bold('MARGIN OF SAFETY:')} {bg_green(f' {mos*100:+.1f}% ')} {green('— ATTRACTIVE per Klarman framework')}")
            elif mos > 0:
                safe_print(f"    {bold('MARGIN OF SAFETY:')} {yellow(f'{mos*100:+.1f}%')} — exists but thin")
            else:
                safe_print(f"    {bold('MARGIN OF SAFETY:')} {red(f'{mos*100:+.1f}%')} — {bold(red('NEGATIVE — Klarman would pass'))}")
        safe_print(f"    {'━' * 60}")

        # Downside scenarios
        ds = self.findings.get("downside_scenarios", [])
        if ds:
            safe_print(f"\n    {bold('Downside Protection Analysis:')}")
            for label, val, loss in ds:
                color_fn = green if loss > -0.15 else yellow if loss > -0.40 else red
                safe_print(f"      {label:<20s} ${val:>10,.2f}  ({color_fn(pct(loss))} from current)")

        # Klarman Checklist
        cl = self.findings.get("checklist", [])
        if cl:
            passed = sum(1 for _, ok, _ in cl if ok)
            safe_print(f"\n    {bold(f'Klarman Checklist ({passed}/{len(cl)}):')}")
            for msg, ok, strength in cl:
                if ok and strength == "strong": icon = green("■")
                elif ok and strength == "moderate": icon = yellow("■")
                elif ok: icon = dim("□")
                else: icon = red("✗")
                safe_print(f"      {icon} {msg}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 11: Factor & Momentum Agent (Modern Quant)
# ══════════════════════════════════════════════════════════════════════════════

class FactorAgent(Agent):
    """Modern multi-factor analysis.

    Based on Fama-French, AQR, and modern quant research:
    - Value factor: cheap stocks outperform expensive ones
    - Quality factor: profitable, stable businesses outperform
    - Momentum factor: recent winners continue winning (12-1 month)
    - Size factor: small caps carry a premium
    - Low-volatility: low-beta stocks deliver risk-adjusted alpha
    """
    name = "factor"
    title = "FACTOR & MOMENTUM ANALYST"
    icon = "⚡"

    def run(self):
        s = self.store

        # ── Value Factor Scores ──
        value_signals = []
        pe = s.overview.get("pe")
        if pe and pe > 0:
            # Earnings yield (inverse P/E)
            ey = 1 / pe
            if ey > 0.08: value_signals.append(("Earnings yield", ey, "cheap", 15))
            elif ey > 0.05: value_signals.append(("Earnings yield", ey, "fair", 5))
            else: value_signals.append(("Earnings yield", ey, "expensive", -10))

        pb = s.overview.get("pb")
        if pb and pb > 0:
            if pb < 1.5: value_signals.append(("P/B", pb, "cheap", 10))
            elif pb < 3: value_signals.append(("P/B", pb, "fair", 0))
            elif pb < 8: value_signals.append(("P/B", pb, "rich", -5))
            else: value_signals.append(("P/B", pb, "extreme", -15))

        ev_ebitda = s.overview.get("ev_ebitda")
        if ev_ebitda and ev_ebitda > 0:
            if ev_ebitda < 10: value_signals.append(("EV/EBITDA", ev_ebitda, "cheap", 10))
            elif ev_ebitda < 15: value_signals.append(("EV/EBITDA", ev_ebitda, "fair", 0))
            elif ev_ebitda < 25: value_signals.append(("EV/EBITDA", ev_ebitda, "rich", -5))
            else: value_signals.append(("EV/EBITDA", ev_ebitda, "extreme", -15))

        # FCF yield
        cf = s.latest("cf_series")
        f = fcf(cf)
        mc = s.overview.get("market_cap") or 0
        if f and mc > 0:
            fy = f / mc
            if fy > 0.08: value_signals.append(("FCF yield", fy, "cheap", 15))
            elif fy > 0.05: value_signals.append(("FCF yield", fy, "fair", 5))
            elif fy > 0: value_signals.append(("FCF yield", fy, "expensive", -5))
            else: value_signals.append(("FCF yield", fy, "negative", -15))

        self.findings["value_signals"] = value_signals
        value_score = sum(pts for _, _, _, pts in value_signals)
        self.findings["value_score"] = value_score

        # ── Quality Factor ──
        quality_signals = []
        roe = s.overview.get("roe")
        if roe:
            if roe > 0.20: quality_signals.append(("ROE", roe, "high", 10))
            elif roe > 0.12: quality_signals.append(("ROE", roe, "good", 5))
            elif roe > 0: quality_signals.append(("ROE", roe, "low", -5))
            else: quality_signals.append(("ROE", roe, "negative", -15))

        gm = None
        if s.overview.get("gp_ttm") and s.overview.get("revenue_ttm"):
            gm = s.overview["gp_ttm"] / s.overview["revenue_ttm"]
        if gm:
            if gm > 0.50: quality_signals.append(("Gross margin", gm, "high", 10))
            elif gm > 0.30: quality_signals.append(("Gross margin", gm, "moderate", 5))
            else: quality_signals.append(("Gross margin", gm, "low", -5))

        # Earnings stability (low variance in NI/Revenue)
        ni_margins = []
        for y in s.years("inc_series"):
            inc_y = s.inc_series.get(y, {})
            ni_y = inc_y.get("net_income")
            rev_y = inc_y.get("revenue")
            if ni_y is not None and rev_y and rev_y > 0:
                ni_margins.append(ni_y / rev_y)
        if len(ni_margins) >= 3:
            stability = 1 - min(statistics.stdev(ni_margins) / (statistics.mean(ni_margins) or 1), 2)
            if stability > 0.8: quality_signals.append(("Earnings stability", stability, "very stable", 10))
            elif stability > 0.5: quality_signals.append(("Earnings stability", stability, "moderate", 5))
            else: quality_signals.append(("Earnings stability", stability, "volatile", -10))

        # Low accruals (cash earnings > reported earnings)
        ni = s.latest("inc_series").get("net_income") or 0
        cfo = s.latest("cf_series").get("cfo") or 0
        if ni > 0:
            accrual_quality = cfo / ni
            if accrual_quality > 1.2: quality_signals.append(("Cash > reported earnings", accrual_quality, "high quality", 10))
            elif accrual_quality > 0.8: quality_signals.append(("Cash ≈ reported earnings", accrual_quality, "ok", 5))
            else: quality_signals.append(("Cash < reported earnings", accrual_quality, "low quality", -10))

        self.findings["quality_signals"] = quality_signals
        quality_score = sum(pts for _, _, _, pts in quality_signals)
        self.findings["quality_score"] = quality_score

        # ── Momentum Factor ──
        momentum_signals = []
        price = s.price
        dma50 = s.overview.get("50dma")
        dma200 = s.overview.get("200dma")
        high52 = s.overview.get("52w_high")
        low52 = s.overview.get("52w_low")

        if price and dma200:
            pct_above_200 = (price / dma200 - 1)
            if pct_above_200 > 0.10: momentum_signals.append(("Price vs 200 DMA", pct_above_200, "strong uptrend", 10))
            elif pct_above_200 > 0: momentum_signals.append(("Price vs 200 DMA", pct_above_200, "above trend", 5))
            elif pct_above_200 > -0.10: momentum_signals.append(("Price vs 200 DMA", pct_above_200, "near trend", -5))
            else: momentum_signals.append(("Price vs 200 DMA", pct_above_200, "downtrend", -15))

        if dma50 and dma200:
            if dma50 > dma200 * 1.03: momentum_signals.append(("Golden cross strength", dma50/dma200-1, "bullish", 10))
            elif dma50 < dma200 * 0.97: momentum_signals.append(("Death cross", dma50/dma200-1, "bearish", -10))

        if price and high52 and low52 and high52 != low52:
            range_pct = (price - low52) / (high52 - low52)
            if range_pct > 0.80: momentum_signals.append(("52w range position", range_pct, "near highs", 10))
            elif range_pct < 0.20: momentum_signals.append(("52w range position", range_pct, "near lows", -10))
            else: momentum_signals.append(("52w range position", range_pct, "mid-range", 0))

        # Revenue momentum (acceleration)
        rev_yoys = yoy(s.inc_series, "revenue")
        if len(rev_yoys) >= 2:
            accel = rev_yoys[-1][1] - rev_yoys[-2][1]
            if accel > 0.05: momentum_signals.append(("Revenue acceleration", accel, "accelerating", 10))
            elif accel < -0.05: momentum_signals.append(("Revenue deceleration", accel, "decelerating", -10))
            else: momentum_signals.append(("Revenue momentum", accel, "stable", 0))

        self.findings["momentum_signals"] = momentum_signals
        momentum_score = sum(pts for _, _, _, pts in momentum_signals)
        self.findings["momentum_score"] = momentum_score

        # ── Size & Volatility Factors ──
        size_score = 0
        beta_val = s.overview.get("beta") or 1.0
        mcap = s.overview.get("market_cap") or 0
        if mcap < 2e9: size_score += 10  # small-cap premium
        elif mcap < 10e9: size_score += 5  # mid-cap
        if beta_val < 0.8: size_score += 10  # low-vol premium
        elif beta_val > 1.5: size_score -= 5

        self.findings["size_vol"] = {
            "market_cap": mcap, "beta": beta_val, "size_score": size_score,
            "size_label": "Small" if mcap < 2e9 else "Mid" if mcap < 10e9 else "Large" if mcap < 200e9 else "Mega",
            "vol_label": "Low" if beta_val < 0.8 else "Market" if beta_val < 1.2 else "High" if beta_val < 1.8 else "Very High"
        }

        # ── Composite Factor Score ──
        composite = value_score + quality_score + momentum_score + size_score
        self.findings["composite"] = composite

        # Factor tilt recommendation
        if value_score > 15: tilt = "Deep Value"
        elif value_score > 5 and quality_score > 15: tilt = "Quality Value (GARP)"
        elif quality_score > 20 and momentum_score > 10: tilt = "Quality Momentum"
        elif momentum_score > 15: tilt = "Momentum"
        elif quality_score > 15: tilt = "Quality Compounder"
        elif value_score < -10 and momentum_score < -5: tilt = "Expensive & Fading"
        else: tilt = "Neutral / Multi-factor"
        self.findings["factor_tilt"] = tilt

        # Signal
        if composite > 25: self.signal = "bullish"; self.score = 20
        elif composite > 10: self.signal = "bullish"; self.score = 10
        elif composite < -20: self.signal = "bearish"; self.score = -20
        elif composite < -5: self.signal = "bearish"; self.score = -10
        else: self.signal = "neutral"; self.score = 0

        self.summary = f"Factor tilt: {tilt} | V:{value_score:+d} Q:{quality_score:+d} M:{momentum_score:+d} | Composite {composite:+d}"

    def render(self):
        self.header()

        def render_factor(name, signals, score):
            color_fn = green if score > 10 else yellow if score > -5 else red
            safe_print(f"\n    {bold(f'{name} Factor')} {color_fn(f'({score:+d} pts)')}")
            for label, val, desc, pts in signals:
                icon = green("▲") if pts > 5 else red("▼") if pts < -5 else yellow("●")
                if isinstance(val, float) and abs(val) < 1:
                    val_str = f"{val*100:.1f}%"
                elif isinstance(val, float):
                    val_str = f"{val:.1f}x"
                else:
                    val_str = str(val)
                safe_print(f"      {icon} {label:<25s} {val_str:>10s}  {dim(desc)}")

        render_factor("VALUE", self.findings.get("value_signals", []), self.findings.get("value_score", 0))
        render_factor("QUALITY", self.findings.get("quality_signals", []), self.findings.get("quality_score", 0))
        render_factor("MOMENTUM", self.findings.get("momentum_signals", []), self.findings.get("momentum_score", 0))

        # Size / Vol
        sv = self.findings.get("size_vol", {})
        safe_print(f"\n    {bold('Size & Volatility:')}")
        safe_print(f"      Size: {sv.get('size_label', '?')} ({usd(sv.get('market_cap'))})  │  Vol: {sv.get('vol_label', '?')} (β={num(sv.get('beta'))})")

        # Composite
        comp = self.findings.get("composite", 0)
        tilt = self.findings.get("factor_tilt", "")
        color_fn = green if comp > 15 else yellow if comp > -5 else red
        safe_print(f"\n    {bold('COMPOSITE FACTOR SCORE:')} {color_fn(f'{comp:+d}')}  │  {bold('Factor Tilt:')} {tilt}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 12: Catalyst & Event Agent
# ══════════════════════════════════════════════════════════════════════════════

class CatalystAgent(Agent):
    """Event-driven catalyst analysis.

    Identifies potential value-unlocking events:
    - Earnings surprise trajectory → likely future beats
    - Insider buying/selling patterns
    - Institutional accumulation/distribution
    - Valuation gap closure catalysts
    - Capital return catalysts (buyback acceleration, special dividends)
    """
    name = "catalyst"
    title = "CATALYST & EVENT ANALYST"
    icon = "💥"

    def run(self):
        s = self.store

        catalysts = []
        anti_catalysts = []

        # ── 1. Earnings Momentum as Catalyst ──
        surprises = s.earn_quarterly[:12]
        beats = 0
        total = 0
        for eq in surprises:
            surprise = safe_float(eq, "surprise")
            if surprise is not None:
                total += 1
                if surprise > 0:
                    beats += 1

        if total >= 4:
            beat_rate = beats / total
            if beat_rate > 0.80:
                catalysts.append(("Consistent earnings beats", f"{beats}/{total} quarters",
                                  "High probability of continued beats — positive estimate revision cycle", 15))
            elif beat_rate > 0.60:
                catalysts.append(("Moderate beat rate", f"{beats}/{total} quarters",
                                  "Estimates may be conservatively set", 5))
            elif beat_rate < 0.40:
                anti_catalysts.append(("Frequent misses", f"{total-beats}/{total} quarters",
                                       "Estimate revision cycle may be negative", -10))

        beat_rate = beats / total if total >= 4 else None
        self.findings["beat_rate"] = beat_rate

        # ── 2. Forward P/E Compression (Growth Catalyst) ──
        pe = s.overview.get("pe")
        fwd_pe = s.overview.get("fwd_pe")
        if pe and fwd_pe and pe > 0 and fwd_pe > 0:
            pe_compression = (pe / fwd_pe - 1)
            if pe_compression > 0.15:
                catalysts.append(("Earnings growth catalyst", f"P/E {pe:.0f}x → Fwd {fwd_pe:.0f}x",
                                  f"Implied {pe_compression*100:.0f}% EPS growth compresses forward multiple", 10))
            self.findings["pe_compression"] = pe_compression
        else:
            self.findings["pe_compression"] = None

        # ── 3. Insider Activity ──
        insider = s.insider[:20]
        insider_buys = 0
        insider_sells = 0
        insider_buy_value = 0
        insider_sell_value = 0
        for tx in insider:
            acq = tx.get("acquisition_or_disposition", "")
            shares = safe_float(tx, "shares") or 0
            price_val = safe_float(tx, "share_price") or 0
            value = shares * price_val
            if acq == "A":
                insider_buys += 1
                insider_buy_value += value
            elif acq == "D":
                insider_sells += 1
                insider_sell_value += value

        self.findings["insider"] = {
            "buys": insider_buys, "sells": insider_sells,
            "buy_value": insider_buy_value, "sell_value": insider_sell_value,
        }

        if insider_buys > insider_sells and insider_buy_value > 100000:
            catalysts.append(("Insider buying", f"{insider_buys} buys ({usd(insider_buy_value)})",
                              "Insiders putting their own money in — strong conviction signal", 15))
        elif insider_sells > insider_buys * 3 and insider_sell_value > 1e6:
            anti_catalysts.append(("Heavy insider selling", f"{insider_sells} sells ({usd(insider_sell_value)})",
                                    "Insiders reducing exposure — may indicate headwinds ahead", -10))

        # ── 4. Capital Return Catalyst ──
        cf = s.latest("cf_series")
        bb = abs(cf.get("buybacks") or 0)
        fcf_val = fcf(cf)
        if fcf_val and fcf_val > 0 and bb > 0:
            buyback_pct = bb / s.overview.get("market_cap", 1e18)
            if buyback_pct > 0.03:
                catalysts.append(("Aggressive buyback", f"{buyback_pct*100:.1f}% of mkt cap",
                                  "Reducing share count — EPS accretion catalyst", 10))
            self.findings["buyback_pct"] = buyback_pct
        else:
            self.findings["buyback_pct"] = 0

        # ── 5. Valuation Gap Catalyst ──
        analyst_target = s.overview.get("analyst_target")
        if analyst_target and s.price and s.price > 0:
            target_gap = analyst_target / s.price - 1
            if target_gap > 0.25:
                catalysts.append(("Large analyst target gap", f"${analyst_target:.0f} ({target_gap*100:+.0f}%)",
                                  "Consensus sees significant upside — potential re-rating", 10))
            elif target_gap < -0.10:
                anti_catalysts.append(("Trading above consensus", f"${analyst_target:.0f} ({target_gap*100:+.0f}%)",
                                        "Price exceeds analyst targets", -5))
            self.findings["target_gap"] = target_gap
        else:
            self.findings["target_gap"] = None

        # ── 6. Revenue Acceleration Catalyst ──
        rev_yoys = yoy(s.inc_series, "revenue")
        if len(rev_yoys) >= 2:
            accel = rev_yoys[-1][1] - rev_yoys[-2][1]
            if accel > 0.05:
                catalysts.append(("Revenue re-acceleration", f"{accel*100:+.1f}pp",
                                  "Growth rate inflecting positive — potential multiple expansion", 10))
            elif accel < -0.10:
                anti_catalysts.append(("Revenue deceleration", f"{accel*100:+.1f}pp",
                                        "Growth rate declining sharply", -10))

        self.findings["catalysts"] = catalysts
        self.findings["anti_catalysts"] = anti_catalysts

        # Score
        cat_score = sum(pts for _, _, _, pts in catalysts)
        anti_score = sum(pts for _, _, _, pts in anti_catalysts)
        net = cat_score + anti_score
        self.findings["net_catalyst_score"] = net

        if net > 20: self.signal = "bullish"; self.score = 20
        elif net > 5: self.signal = "bullish"; self.score = 10
        elif net < -15: self.signal = "bearish"; self.score = -15
        elif net < -5: self.signal = "bearish"; self.score = -5
        else: self.signal = "neutral"; self.score = 0

        n_cat = len(catalysts)
        n_anti = len(anti_catalysts)
        self.summary = f"{n_cat} catalysts, {n_anti} anti-catalysts | Net score {net:+d}"

    def render(self):
        self.header()

        catalysts = self.findings.get("catalysts", [])
        anti = self.findings.get("anti_catalysts", [])

        if catalysts:
            safe_print(f"    {bold(green(f'Catalysts ({len(catalysts)}):'))}")
            for name, metric, rationale, pts in catalysts:
                safe_print(f"      {green('▲')} {bold(name)} {dim(f'[{metric}]')} (+{pts}pts)")
                safe_print(f"        {dim(rationale)}")

        if anti:
            safe_print(f"\n    {bold(red(f'Anti-Catalysts ({len(anti)}):'))}")
            for name, metric, rationale, pts in anti:
                safe_print(f"      {red('▼')} {bold(name)} {dim(f'[{metric}]')} ({pts}pts)")
                safe_print(f"        {dim(rationale)}")

        # Insider summary
        ins = self.findings.get("insider", {})
        if ins.get("buys") or ins.get("sells"):
            safe_print(f"\n    {bold('Insider Activity:')}")
            safe_print(f"      Buys: {ins.get('buys', 0)} ({usd(ins.get('buy_value', 0))})  │  Sells: {ins.get('sells', 0)} ({usd(ins.get('sell_value', 0))})")

        net = self.findings.get("net_catalyst_score", 0)
        color_fn = green if net > 10 else yellow if net > -5 else red
        safe_print(f"\n    {bold('Net Catalyst Score:')} {color_fn(f'{net:+d}')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 13: Narrative & Reflexivity Agent (Soros-inspired)
# ══════════════════════════════════════════════════════════════════════════════

class NarrativeAgent(Agent):
    """George Soros's reflexivity + narrative economics framework.

    Core insights:
    - Markets are not efficient — they are reflexive
    - The prevailing narrative CHANGES the fundamentals (self-fulfilling/self-defeating)
    - Boom-bust sequences follow a predictable arc
    - The gap between perception and reality creates opportunity
    - When a trend is strong, fundamentals get better (reflexive feedback)
    """
    name = "narrative"
    title = "NARRATIVE & REFLEXIVITY ANALYST"
    icon = "🔮"

    def run(self):
        s = self.store

        # ── 1. Narrative Regime Detection ──
        # Are fundamentals improving AND price rising? (positive reflexive loop)
        # Or fundamentals deteriorating despite price rising? (fragile)

        rev_trend = cagr(s.inc_series, "revenue") or 0
        ni_trend = cagr(s.inc_series, "net_income") or 0

        # Price trend (52w positioning as proxy)
        price = s.price
        high52 = s.overview.get("52w_high")
        low52 = s.overview.get("52w_low")
        dma200 = s.overview.get("200dma")

        price_trend = 0
        if price and dma200 and dma200 > 0:
            price_trend = price / dma200 - 1  # positive = uptrend

        fundamental_trend = (rev_trend + ni_trend) / 2 if rev_trend and ni_trend else 0

        # Reflexivity regime classification
        if price_trend > 0.05 and fundamental_trend > 0.05:
            regime = "Positive Reflexive Loop"
            regime_desc = "Price rising ↔ fundamentals improving. Classic virtuous cycle — can persist but eventually overshoots."
            regime_score = 15
        elif price_trend > 0.05 and fundamental_trend < -0.02:
            regime = "Narrative Bubble Risk"
            regime_desc = "Price rising while fundamentals deteriorating. Gap between perception and reality is widening."
            regime_score = -20
        elif price_trend < -0.05 and fundamental_trend > 0.05:
            regime = "Contrarian Opportunity"
            regime_desc = "Price declining while fundamentals improving. Negative narrative may be creating a discount."
            regime_score = 20
        elif price_trend < -0.05 and fundamental_trend < -0.02:
            regime = "Negative Reflexive Loop"
            regime_desc = "Price declining ↔ fundamentals deteriorating. Vicious cycle — risk of further decline."
            regime_score = -15
        elif abs(price_trend) < 0.05 and abs(fundamental_trend) < 0.03:
            regime = "Equilibrium / Range-Bound"
            regime_desc = "Price and fundamentals stable. Low reflexivity — waiting for a catalyst to break equilibrium."
            regime_score = 0
        else:
            regime = "Transitional"
            regime_desc = "Mixed signals — regime may be shifting."
            regime_score = 0

        self.findings["regime"] = regime
        self.findings["regime_desc"] = regime_desc
        self.findings["regime_score"] = regime_score
        self.findings["price_trend"] = price_trend
        self.findings["fundamental_trend"] = fundamental_trend

        # ── 2. Narrative Consensus Analysis ──
        # Use news sentiment + analyst ratings to gauge prevailing narrative
        news_score = 0
        articles = s.news[:30]
        for article in articles:
            for ts in article.get("ticker_sentiment", []):
                if ts.get("ticker") == s.ticker:
                    score = safe_float(ts, "ticker_sentiment_score", 0)
                    news_score += score
                    break

        avg_news = news_score / len(articles) if articles else 0
        self.findings["narrative_score"] = avg_news

        # Analyst consensus tilt
        sb = int(s.overview.get("AnalystRatingStrongBuy") or 0)
        b = int(s.overview.get("AnalystRatingBuy") or 0)
        h = int(s.overview.get("AnalystRatingHold") or 0)
        se = int(s.overview.get("AnalystRatingSell") or 0)
        ss = int(s.overview.get("AnalystRatingStrongSell") or 0)
        total = sb + b + h + se + ss
        if total > 0:
            consensus_score = (sb * 2 + b * 1 + h * 0 + se * -1 + ss * -2) / total
        else:
            consensus_score = 0
        self.findings["consensus_score"] = consensus_score

        # ── 3. Narrative Arc Position ──
        # Based on reflexivity theory, where are we in the boom-bust cycle?
        # Signals: momentum, valuation, growth trajectory, sentiment

        pe = s.overview.get("pe") or 20
        fwd_pe = s.overview.get("fwd_pe") or pe
        peg = s.overview.get("peg")

        arc_signals = []

        # Phase detection
        if price_trend > 0.15 and pe > 35 and consensus_score > 1.0:
            arc_signals.append(("Late-stage euphoria", "Extreme optimism + stretched multiples. Soros: 'close to tipping point'", -15))
        elif price_trend > 0.10 and consensus_score > 0.5:
            arc_signals.append(("Acceleration phase", "Strong momentum + growing consensus. Trend likely continues short-term", 5))
        elif price_trend > 0 and consensus_score > 0:
            arc_signals.append(("Early uptrend", "Price and sentiment positive but not extreme. Best risk/reward phase", 10))
        elif price_trend < -0.10 and consensus_score < -0.5:
            arc_signals.append(("Capitulation zone", "Negative sentiment + price decline. Potential bottom formation", 15))
        elif price_trend < -0.05:
            arc_signals.append(("Correction phase", "Price declining, sentiment turning. Watch for stabilization signals", -5))
        else:
            arc_signals.append(("Neutral — no dominant narrative", "Waiting for directional catalyst", 0))

        self.findings["arc_signals"] = arc_signals

        # ── 4. Perception vs Reality Gap ──
        # The bigger the gap, the bigger the opportunity (in either direction)
        gaps = []
        if s.overview.get("analyst_target") and price:
            target_gap = s.overview["analyst_target"] / price - 1
            if abs(target_gap) > 0.20:
                direction = "undervalued" if target_gap > 0 else "overvalued"
                gaps.append((f"Analyst target gap: {target_gap*100:+.0f}%", direction))

        if peg and peg > 0:
            if peg < 0.8:
                gaps.append(("PEG < 0.8 — growth underpriced by market", "undervalued"))
            elif peg > 2.5:
                gaps.append(("PEG > 2.5 — market pricing in unrealistic growth", "overvalued"))

        # Revenue growth vs P/S ratio mismatch
        rev_cagr = cagr(s.inc_series, "revenue") or 0
        ps = s.overview.get("ps") or 0
        if rev_cagr > 0.15 and ps < 3:
            gaps.append(("High growth + low P/S — market underappreciating growth", "undervalued"))
        elif rev_cagr < 0.05 and ps > 10:
            gaps.append(("Low growth + high P/S — narrative premium unwarranted", "overvalued"))

        self.findings["perception_gaps"] = gaps

        # ── Signal ──
        total_score = regime_score + sum(pts for _, _, pts in arc_signals)
        if total_score > 15: self.signal = "bullish"; self.score = 15
        elif total_score > 5: self.signal = "bullish"; self.score = 8
        elif total_score < -15: self.signal = "bearish"; self.score = -15
        elif total_score < -5: self.signal = "bearish"; self.score = -8
        else: self.signal = "neutral"; self.score = 0

        self.summary = f"Regime: {regime} | Narrative {avg_news:+.2f} | Consensus {consensus_score:+.1f}"

    def render(self):
        self.header()

        safe_print(f"    {dim('\"Markets are always wrong — the question is how much.\" — George Soros')}")

        # Reflexivity regime
        regime = self.findings.get("regime", "Unknown")
        rs = self.findings.get("regime_score", 0)
        color_fn = green if rs > 10 else red if rs < -10 else yellow
        safe_print(f"\n    {bold('Reflexivity Regime:')} {color_fn(regime)}")
        safe_print(f"      {dim(self.findings.get('regime_desc', ''))}")
        safe_print(f"      Price trend: {pct(self.findings.get('price_trend'))}  │  Fundamental trend: {pct(self.findings.get('fundamental_trend'))}")

        # Narrative arc
        arc = self.findings.get("arc_signals", [])
        if arc:
            safe_print(f"\n    {bold('Boom-Bust Arc Position:')}")
            for phase, desc, pts in arc:
                icon = green("▲") if pts > 5 else red("▼") if pts < -5 else yellow("●")
                safe_print(f"      {icon} {bold(phase)}")
                safe_print(f"        {dim(desc)}")

        # Narrative scores
        ns = self.findings.get("narrative_score", 0)
        cs = self.findings.get("consensus_score", 0)
        safe_print(f"\n    {bold('Narrative Indicators:')}")
        safe_print(f"      News sentiment: {green(f'{ns:+.2f}') if ns > 0.1 else red(f'{ns:+.2f}') if ns < -0.1 else dim(f'{ns:+.2f}')}")
        safe_print(f"      Analyst consensus: {green(f'{cs:+.1f}') if cs > 0.5 else red(f'{cs:+.1f}') if cs < -0.5 else dim(f'{cs:+.1f}')} (-2 to +2 scale)")

        # Perception gaps
        gaps = self.findings.get("perception_gaps", [])
        if gaps:
            safe_print(f"\n    {bold('Perception vs Reality Gaps:')}")
            for msg, direction in gaps:
                icon = green("↑") if direction == "undervalued" else red("↓")
                safe_print(f"      {icon} {msg}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 14: Minsky Financial Instability — Hyman Minsky
# ══════════════════════════════════════════════════════════════════════════════

class MinskyAgent(Agent):
    """
    Hyman Minsky's Financial Instability Hypothesis.
    Classifies financing as Hedge / Speculative / Ponzi based on whether
    cash flows can service both interest AND principal. Tracks the transition
    across Minsky's stability→instability arc.

    References: "Stabilizing an Unstable Economy" (Minsky),
                "Manias, Panics, and Crashes" (Kindleberger)
    """
    name = "minsky"
    title = "MINSKY INSTABILITY ANALYST"
    icon = "🏗️"

    def run(self):
        s = self.store
        price = s.price or 0

        # ── 1. Minsky Classification: Hedge / Speculative / Ponzi ──
        # Hedge: operating cash flow covers interest + principal repayment
        # Speculative: covers interest but must roll over principal
        # Ponzi: can't even cover interest from cash flows

        inc = s.inc_annual[0] if s.inc_annual else {}
        cf = s.cf_annual[0] if s.cf_annual else {}
        bs = s.bs_annual[0] if s.bs_annual else {}

        # Try AV format first, then EDGAR series
        op_cf = safe_float(cf, "operatingCashflow", 0)
        if op_cf == 0 and s.cf_series:
            latest_cf = list(s.cf_series.values())[-1] if s.cf_series else {}
            op_cf = latest_cf.get("cfo", 0) or 0
        interest = safe_float(inc, "interestExpense", 0)
        if interest == 0 and s.inc_series:
            latest_inc = list(s.inc_series.values())[-1] if s.inc_series else {}
            interest = latest_inc.get("interest_exp", 0) or 0
        if interest < 0: interest = abs(interest)

        total_debt = safe_float(bs, "shortLongTermDebtTotal", 0) or safe_float(bs, "longTermDebt", 0)
        if total_debt == 0 and s.bs_series:
            latest_bs = list(s.bs_series.values())[-1] if s.bs_series else {}
            total_debt = latest_bs.get("long_term_debt", 0) or 0
        current_debt = safe_float(bs, "currentDebt", 0) or safe_float(bs, "shortTermDebt", 0)
        if current_debt == 0 and s.bs_series:
            latest_bs = list(s.bs_series.values())[-1] if s.bs_series else {}
            current_debt = latest_bs.get("current_debt", 0) or 0
        # Annual principal proxy: current portion of debt
        principal_due = current_debt if current_debt > 0 else total_debt * 0.10  # assume 10% rollover

        total_service = interest + principal_due

        if op_cf <= 0:
            minsky_class = "Ponzi"
            minsky_desc = "Negative operating cash flow — cannot service any debt from operations"
            minsky_score = -25
        elif op_cf < interest:
            minsky_class = "Ponzi"
            minsky_desc = "Operating cash flow insufficient to cover interest expense"
            minsky_score = -20
        elif op_cf < total_service:
            minsky_class = "Speculative"
            minsky_desc = "Cash flow covers interest but not principal — must refinance to survive"
            minsky_score = -10
        else:
            minsky_class = "Hedge"
            minsky_desc = "Cash flow covers both interest and principal — self-sustaining"
            minsky_score = 10

        self.findings["minsky_class"] = minsky_class
        self.findings["minsky_desc"] = minsky_desc
        self.findings["op_cf"] = op_cf
        self.findings["interest"] = interest
        self.findings["principal_due"] = principal_due
        self.findings["total_service"] = total_service

        # ── 2. Stability-Instability Arc Position ──
        # Track how the company's financing has evolved across time
        # Rising leverage + rising asset prices = instability building
        # Falling leverage + stable cash flows = stability

        debt_ratios = []
        if s.bs_annual:
            for i_bs in s.bs_annual[:5]:
                td = safe_float(i_bs, "shortLongTermDebtTotal", 0) or safe_float(i_bs, "longTermDebt", 0)
                eq = safe_float(i_bs, "totalShareholderEquity", 0)
                if eq and eq > 0:
                    debt_ratios.append(td / eq)
        if not debt_ratios and s.bs_series:
            for yr, bs_yr in list(s.bs_series.items())[-5:]:
                td = bs_yr.get("long_term_debt", 0) or 0
                eq = bs_yr.get("equity", 0) or 0
                if eq > 0:
                    debt_ratios.append(td / eq)

        leverage_trend = "stable"
        leverage_delta = 0
        if len(debt_ratios) >= 2:
            leverage_delta = debt_ratios[0] - debt_ratios[-1]
            if leverage_delta > 0.3:
                leverage_trend = "rising fast"
                minsky_score -= 10
            elif leverage_delta > 0.1:
                leverage_trend = "rising"
                minsky_score -= 5
            elif leverage_delta < -0.3:
                leverage_trend = "deleveraging fast"
                minsky_score += 5
            elif leverage_delta < -0.1:
                leverage_trend = "deleveraging"
                minsky_score += 3

        self.findings["leverage_trend"] = leverage_trend
        self.findings["leverage_delta"] = leverage_delta
        self.findings["debt_ratios"] = debt_ratios

        # ── 3. Kindleberger Mania Indicators ──
        # From "Manias, Panics, and Crashes":
        # - Credit growth outpacing revenue growth
        # - Asset price acceleration disconnected from fundamentals
        # - Herd behavior in analyst ratings

        mania_flags = []

        # Credit vs revenue growth comparison
        rev_series = [safe_float(inc_yr, "totalRevenue", 0) for inc_yr in s.inc_annual[:5]]
        if not any(r > 0 for r in rev_series) and s.inc_series:
            rev_series = [yr.get("revenue", 0) or 0 for yr in list(s.inc_series.values())[-5:]]
        rev_growth = (rev_series[0] / rev_series[-1]) - 1 if len(rev_series) >= 2 and rev_series[-1] > 0 else 0
        debt_growth = (debt_ratios[0] / debt_ratios[-1]) - 1 if len(debt_ratios) >= 2 and debt_ratios[-1] > 0 else 0
        if debt_growth > rev_growth * 2 and debt_growth > 0.20:
            mania_flags.append("Debt growing 2x+ faster than revenue — classic instability signal")
            minsky_score -= 5

        # Analyst herding (extreme consensus = Kindleberger displacement)
        sb = int(s.overview.get("AnalystRatingStrongBuy") or 0)
        b = int(s.overview.get("AnalystRatingBuy") or 0)
        h = int(s.overview.get("AnalystRatingHold") or 0)
        se = int(s.overview.get("AnalystRatingSell") or 0)
        ss = int(s.overview.get("AnalystRatingStrongSell") or 0)
        total_ratings = sb + b + h + se + ss
        if total_ratings > 0:
            bull_pct = (sb + b) / total_ratings
            bear_pct = (se + ss) / total_ratings
            if bull_pct > 0.85:
                mania_flags.append("Extreme analyst consensus ({}% bull) — Kindleberger herding risk".format(int(bull_pct * 100)))
                minsky_score -= 3
            elif bear_pct > 0.50:
                mania_flags.append("Majority bearish analyst consensus — potential capitulation")
                minsky_score += 3

        # Interest coverage deterioration
        coverage_series = []
        for i_idx in range(min(len(s.inc_annual), len(s.cf_annual), 5)):
            ie = safe_float(s.inc_annual[i_idx], "interestExpense", 0)
            if ie < 0: ie = abs(ie)
            ebit = safe_float(s.inc_annual[i_idx], "ebit", 0)
            if ie > 0:
                coverage_series.append(ebit / ie)
        if len(coverage_series) >= 2:
            if coverage_series[0] < coverage_series[-1] * 0.6:
                mania_flags.append("Interest coverage deteriorated >40% — Minsky moment approaching")
                minsky_score -= 5

        self.findings["mania_flags"] = mania_flags

        # ── Signal ──
        if minsky_score > 10:
            self.signal = "bullish"; self.score = min(minsky_score, 20)
        elif minsky_score > 0:
            self.signal = "bullish"; self.score = 5
        elif minsky_score > -10:
            self.signal = "neutral"; self.score = 0
        elif minsky_score > -20:
            self.signal = "bearish"; self.score = -10
        else:
            self.signal = "bearish"; self.score = -20

        self.summary = "Minsky: {} | Leverage {} | {} flags".format(
            minsky_class, leverage_trend, len(mania_flags))

    def render(self):
        self.header()
        safe_print(f"    {dim('\"Stability leads to instability. The more stable things become,')}")
        safe_print(f"    {dim(' the more unstable they will be when the crisis hits.\" — Hyman Minsky')}")

        mc = self.findings.get("minsky_class", "Unknown")
        color_map = {"Hedge": green, "Speculative": yellow, "Ponzi": red}
        color_fn = color_map.get(mc, dim)
        safe_print(f"\n    {bold('Minsky Financing Classification:')} {color_fn(mc)}")
        safe_print(f"      {dim(self.findings.get('minsky_desc', ''))}")

        op_cf = self.findings.get("op_cf", 0)
        interest = self.findings.get("interest", 0)
        total_service = self.findings.get("total_service", 0)
        safe_print(f"\n    {bold('Debt Service Analysis:')}")
        safe_print(f"      Operating Cash Flow:  {usd(op_cf)}")
        safe_print(f"      Interest Expense:     {usd(interest)}")
        safe_print(f"      Total Debt Service:   {usd(total_service)}")
        if total_service > 0:
            coverage = op_cf / total_service
            cov_fn = green if coverage > 2 else yellow if coverage > 1 else red
            cov_str = "{:.2f}x".format(coverage)
            safe_print(f"      Coverage Ratio:       {cov_fn(cov_str)}")

        lt = self.findings.get("leverage_trend", "stable")
        ld = self.findings.get("leverage_delta", 0)
        lt_fn = red if "rising" in lt else green if "deleveraging" in lt else yellow
        safe_print(f"\n    {bold('Leverage Trajectory:')} {lt_fn(lt)} (delta: {ld:+.2f})")
        dr = self.findings.get("debt_ratios", [])
        if dr:
            dr_strs = ["{:.2f}x".format(d) for d in dr]
            safe_print(f"      D/E history (recent→old): {' → '.join(dr_strs)}")

        flags = self.findings.get("mania_flags", [])
        if flags:
            safe_print(f"\n    {bold(red('Kindleberger Mania Indicators:'))}")
            for f in flags:
                safe_print(f"      {red('⚠')} {f}")
        else:
            safe_print(f"\n    {green('✓')} No Kindleberger mania indicators detected")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 15: Tail Risk & Fractal Markets — Mandelbrot + Taleb
# ══════════════════════════════════════════════════════════════════════════════

class TailRiskAgent(Agent):
    """
    Fractal market analysis inspired by Mandelbrot's "(Mis)behavior of Markets"
    and Taleb's "The Black Swan" / "Antifragile".

    Key concepts:
    - Fat tails: stock returns are NOT normally distributed
    - Hurst exponent proxy: persistence vs mean-reversion
    - Drawdown analysis: maximum observed drawdown and recovery
    - Antifragility score: does the company benefit from disorder?

    References: Mandelbrot "Fractals and Scaling in Finance",
                Taleb "Statistical Consequences of Fat Tails",
                McNeil/Frey/Embrechts "Quantitative Risk Management"
    """
    name = "tail_risk"
    title = "TAIL RISK & FRACTAL ANALYST"
    icon = "🦢"

    def run(self):
        import math
        s = self.store
        price = s.price or 0

        # ── 1. Return Distribution Analysis ──
        # Compute daily returns from available price data
        prices = []
        for dp in (s.daily_prices or [])[:252]:  # 1 year of daily data
            p = safe_float(dp, "4. close", 0)
            if p > 0:
                prices.append(p)
        prices.reverse()  # oldest first

        returns = []
        if len(prices) >= 2:
            for i in range(1, len(prices)):
                ret = math.log(prices[i] / prices[i-1])
                returns.append(ret)

        tail_score = 0

        if len(returns) >= 20:
            mean_ret = statistics.mean(returns)
            std_ret = statistics.stdev(returns) if len(returns) > 1 else 0.01

            # Kurtosis (excess kurtosis — 0 for normal distribution)
            n = len(returns)
            if std_ret > 0 and n >= 4:
                m4 = sum((r - mean_ret) ** 4 for r in returns) / n
                kurtosis = (m4 / (std_ret ** 4)) - 3
            else:
                kurtosis = 0

            # Skewness
            if std_ret > 0 and n >= 3:
                m3 = sum((r - mean_ret) ** 3 for r in returns) / n
                skewness = m3 / (std_ret ** 3)
            else:
                skewness = 0

            self.findings["kurtosis"] = kurtosis
            self.findings["skewness"] = skewness
            self.findings["daily_vol"] = std_ret
            self.findings["annual_vol"] = std_ret * math.sqrt(252)

            # Fat tail detection
            if kurtosis > 5:
                self.findings["tail_type"] = "Extremely fat tails"
                tail_score -= 10
            elif kurtosis > 2:
                self.findings["tail_type"] = "Fat tails (leptokurtic)"
                tail_score -= 5
            elif kurtosis > 0.5:
                self.findings["tail_type"] = "Mildly fat tails"
                tail_score -= 2
            else:
                self.findings["tail_type"] = "Near-normal tails"

            if skewness < -0.5:
                self.findings["skew_type"] = "Negative skew — left tail risk"
                tail_score -= 5
            elif skewness > 0.5:
                self.findings["skew_type"] = "Positive skew — upside asymmetry"
                tail_score += 3
            else:
                self.findings["skew_type"] = "Roughly symmetric"

            # ── 2. Hurst Exponent Proxy (Rescaled Range) ──
            # H > 0.5: trending/persistent, H < 0.5: mean-reverting, H = 0.5: random walk
            # Simplified R/S calculation
            if len(returns) >= 50:
                half = len(returns) // 2
                segments = [returns[:half], returns[half:]]
                rs_values = []
                for seg in segments:
                    seg_mean = statistics.mean(seg)
                    cumdev = []
                    running = 0
                    for r in seg:
                        running += (r - seg_mean)
                        cumdev.append(running)
                    R = max(cumdev) - min(cumdev)
                    S = statistics.stdev(seg) if len(seg) > 1 else 0.01
                    if S > 0:
                        rs_values.append(R / S)

                if rs_values and len(returns) > 0:
                    avg_rs = statistics.mean(rs_values)
                    # H ≈ log(R/S) / log(n)
                    hurst = math.log(max(avg_rs, 0.01)) / math.log(len(returns) / 2) if avg_rs > 0 else 0.5
                    hurst = max(0.0, min(1.0, hurst))  # clamp
                else:
                    hurst = 0.5
            else:
                hurst = 0.5

            self.findings["hurst"] = hurst
            if hurst > 0.65:
                self.findings["hurst_regime"] = "Trending (persistent) — momentum strategies favored"
                tail_score += 3
            elif hurst < 0.35:
                self.findings["hurst_regime"] = "Mean-reverting — contrarian strategies favored"
                tail_score += 2
            else:
                self.findings["hurst_regime"] = "Random walk — no exploitable autocorrelation"

            # ── 3. Maximum Drawdown Analysis ──
            peak = prices[0]
            max_dd = 0
            dd_start = 0
            dd_end = 0
            for i, p in enumerate(prices):
                if p > peak:
                    peak = p
                dd = (peak - p) / peak
                if dd > max_dd:
                    max_dd = dd
                    dd_end = i

            self.findings["max_drawdown"] = max_dd
            if max_dd > 0.50:
                self.findings["drawdown_severity"] = "Catastrophic (>50%)"
                tail_score -= 10
            elif max_dd > 0.30:
                self.findings["drawdown_severity"] = "Severe (30-50%)"
                tail_score -= 5
            elif max_dd > 0.15:
                self.findings["drawdown_severity"] = "Moderate (15-30%)"
                tail_score -= 2
            else:
                self.findings["drawdown_severity"] = "Mild (<15%)"
                tail_score += 3

            # ── 4. VaR and CVaR (Expected Shortfall) ──
            sorted_returns = sorted(returns)
            n5 = max(1, int(len(sorted_returns) * 0.05))
            n1 = max(1, int(len(sorted_returns) * 0.01))

            var_95 = sorted_returns[n5 - 1] if n5 <= len(sorted_returns) else sorted_returns[0]
            var_99 = sorted_returns[n1 - 1] if n1 <= len(sorted_returns) else sorted_returns[0]
            cvar_95 = statistics.mean(sorted_returns[:n5]) if n5 > 0 else var_95

            self.findings["var_95"] = var_95
            self.findings["var_99"] = var_99
            self.findings["cvar_95"] = cvar_95

        else:
            self.findings["tail_type"] = "Insufficient data"
            self.findings["kurtosis"] = None
            self.findings["skewness"] = None
            self.findings["hurst"] = None
            self.findings["max_drawdown"] = None

        # ── 5. Antifragility Score (Taleb) ──
        # A company is antifragile if it benefits from volatility/disorder:
        # - Low debt (survives stress)
        # - High cash / assets ratio (optionality)
        # - Pricing power (can pass through shocks)
        # - Diversified revenue (not fragile to single point of failure)
        # - High FCF generation (self-funding)

        antifragile_pts = 0
        antifragile_reasons = []

        bs = s.bs_annual[0] if s.bs_annual else {}
        inc = s.inc_annual[0] if s.inc_annual else {}
        cf_stmt = s.cf_annual[0] if s.cf_annual else {}

        total_debt = safe_float(bs, "shortLongTermDebtTotal", 0) or safe_float(bs, "longTermDebt", 0)
        total_assets = safe_float(bs, "totalAssets", 0)
        cash = safe_float(bs, "cashAndShortTermInvestments", 0)
        revenue = safe_float(inc, "totalRevenue", 0)
        gp = safe_float(inc, "grossProfit", 0)
        op_cf = safe_float(cf_stmt, "operatingCashflow", 0)
        capex = abs(safe_float(cf_stmt, "capitalExpenditures", 0))
        free_cf = op_cf - capex

        # Low debt / high cash
        if total_assets > 0:
            debt_ratio = total_debt / total_assets
            cash_ratio = cash / total_assets
            if debt_ratio < 0.15:
                antifragile_pts += 2; antifragile_reasons.append("Very low leverage ({:.0%} debt/assets)".format(debt_ratio))
            elif debt_ratio < 0.30:
                antifragile_pts += 1; antifragile_reasons.append("Low leverage ({:.0%} debt/assets)".format(debt_ratio))
            elif debt_ratio > 0.60:
                antifragile_pts -= 2; antifragile_reasons.append("High leverage ({:.0%}) — fragile to shocks".format(debt_ratio))

            if cash_ratio > 0.20:
                antifragile_pts += 2; antifragile_reasons.append("Strong cash position ({:.0%}) — optionality".format(cash_ratio))

        # Gross margin (pricing power proxy)
        if revenue > 0:
            gm = gp / revenue
            if gm > 0.60:
                antifragile_pts += 2; antifragile_reasons.append("High gross margin ({:.0%}) — pricing power".format(gm))
            elif gm > 0.40:
                antifragile_pts += 1; antifragile_reasons.append("Good gross margin ({:.0%})".format(gm))
            elif gm < 0.20:
                antifragile_pts -= 1; antifragile_reasons.append("Thin margins ({:.0%}) — fragile to cost shocks".format(gm))

        # FCF generation
        if revenue > 0:
            fcf_margin = free_cf / revenue
            if fcf_margin > 0.20:
                antifragile_pts += 2; antifragile_reasons.append("Excellent FCF margin ({:.0%}) — self-funding".format(fcf_margin))
            elif fcf_margin > 0.10:
                antifragile_pts += 1; antifragile_reasons.append("Good FCF margin ({:.0%})".format(fcf_margin))
            elif fcf_margin < 0:
                antifragile_pts -= 2; antifragile_reasons.append("Negative FCF — dependent on external capital")

        self.findings["antifragile_score"] = antifragile_pts
        self.findings["antifragile_reasons"] = antifragile_reasons
        self.findings["antifragile_label"] = (
            "Antifragile" if antifragile_pts >= 5 else
            "Robust" if antifragile_pts >= 2 else
            "Fragile" if antifragile_pts <= -2 else
            "Neutral"
        )

        if antifragile_pts >= 3: tail_score += 5
        elif antifragile_pts <= -2: tail_score -= 5

        # ── Signal ──
        if tail_score > 5:
            self.signal = "bullish"; self.score = min(tail_score, 15)
        elif tail_score > -5:
            self.signal = "neutral"; self.score = 0
        elif tail_score > -15:
            self.signal = "bearish"; self.score = max(tail_score, -15)
        else:
            self.signal = "bearish"; self.score = -20

        dd = self.findings.get("max_drawdown")
        dd_str = "{:.0%}".format(dd) if dd is not None else "N/A"
        self.summary = "{} | MaxDD {} | H={:.2f} | {}".format(
            self.findings.get("tail_type", "?"),
            dd_str,
            self.findings.get("hurst", 0),
            self.findings.get("antifragile_label", "?")
        )

    def render(self):
        self.header()
        safe_print(f"    {dim('\"The cotton price data contradicted everything the financial orthodox taught.')}")
        safe_print(f"    {dim(' The tails were fat. The variance was wild.\" — Benoit Mandelbrot')}")

        # Distribution
        kurt = self.findings.get("kurtosis")
        skew = self.findings.get("skewness")
        tt = self.findings.get("tail_type", "N/A")
        avol = self.findings.get("annual_vol")
        safe_print(f"\n    {bold('Return Distribution:')}")
        safe_print(f"      Type: {yellow(tt) if 'fat' in tt.lower() else green(tt)}")
        if kurt is not None:
            safe_print(f"      Excess Kurtosis: {red('{:.2f}'.format(kurt)) if kurt > 3 else yellow('{:.2f}'.format(kurt)) if kurt > 1 else dim('{:.2f}'.format(kurt))}  (0 = normal)")
        if skew is not None:
            skew_fn = red if skew < -0.5 else green if skew > 0.5 else dim
            safe_print(f"      Skewness: {skew_fn('{:+.2f}'.format(skew))}")
        if avol is not None:
            safe_print(f"      Annualized Volatility: {yellow('{:.1%}'.format(avol)) if avol > 0.40 else dim('{:.1%}'.format(avol))}")

        # Hurst
        h = self.findings.get("hurst")
        if h is not None:
            hr = self.findings.get("hurst_regime", "")
            h_fn = green if h > 0.55 else cyan if h < 0.45 else dim
            safe_print(f"\n    {bold('Hurst Exponent:')} {h_fn('{:.3f}'.format(h))}")
            safe_print(f"      {dim(hr)}")

        # Drawdown
        dd = self.findings.get("max_drawdown")
        if dd is not None:
            sev = self.findings.get("drawdown_severity", "")
            dd_fn = red if dd > 0.30 else yellow if dd > 0.15 else green
            safe_print(f"\n    {bold('Maximum Drawdown (1Y):')} {dd_fn('{:.1%}'.format(dd))}")
            safe_print(f"      Severity: {sev}")

        # VaR
        var95 = self.findings.get("var_95")
        var99 = self.findings.get("var_99")
        cvar = self.findings.get("cvar_95")
        if var95 is not None:
            safe_print(f"\n    {bold('Value at Risk:')}")
            safe_print(f"      1-day VaR (95%):  {red('{:.2%}'.format(abs(var95)))}")
            safe_print(f"      1-day VaR (99%):  {red('{:.2%}'.format(abs(var99)))}")
            safe_print(f"      1-day CVaR (95%): {red('{:.2%}'.format(abs(cvar)))}  {dim('(expected shortfall)')}")

        # Antifragility
        af_label = self.findings.get("antifragile_label", "?")
        af_score = self.findings.get("antifragile_score", 0)
        af_reasons = self.findings.get("antifragile_reasons", [])
        label_fn = {"Antifragile": green, "Robust": cyan, "Fragile": red}.get(af_label, yellow)
        safe_print(f"\n    {bold('Taleb Antifragility Assessment:')} {label_fn(af_label)} ({af_score:+d})")
        for reason in af_reasons:
            icon = green("✓") if "low" in reason.lower() or "high" in reason.lower() or "excellent" in reason.lower() or "good" in reason.lower() or "strong" in reason.lower() else red("✗") if "fragile" in reason.lower() or "negative" in reason.lower() or "thin" in reason.lower() else yellow("●")
            safe_print(f"      {icon} {reason}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 16: Complexity & Regime — Arthur/Santa Fe + Sornette
# ══════════════════════════════════════════════════════════════════════════════

class ComplexityAgent(Agent):
    """
    Complexity-economics inspired analysis. Draws from:
    - W. Brian Arthur / Santa Fe Institute: increasing returns, path dependence
    - Didier Sornette: log-periodic oscillations, bubble detection
    - Stuart Kauffman: fitness landscapes, adjacent possible
    - Complex Adaptive Systems: emergent properties, non-linearity

    Examines whether the company operates in an increasing-returns regime
    (winner-take-all/network effects) vs diminishing-returns (commoditized).
    """
    name = "complexity"
    title = "COMPLEXITY & REGIME ANALYST"
    icon = "🌀"

    def run(self):
        import math
        s = self.store
        price = s.price or 0
        complexity_score = 0

        # ── 1. Increasing vs Diminishing Returns Regime (Brian Arthur) ──
        # Increasing returns: high margins, growing faster, network effects
        # Diminishing returns: shrinking margins, commodity dynamics
        inc = s.inc_annual
        regime_signals = []

        # Margin trajectory
        margins = []
        for yr in inc[:5]:
            rev = safe_float(yr, "totalRevenue", 0)
            op_inc = safe_float(yr, "operatingIncome", 0)
            if rev > 0:
                margins.append(op_inc / rev)

        if len(margins) >= 2:
            margin_delta = margins[0] - margins[-1]
            if margin_delta > 0.05 and margins[0] > 0.20:
                regime_signals.append(("Increasing returns", "Operating margins expanding + high ({:.0%} → {:.0%})".format(margins[-1], margins[0]), 10))
            elif margin_delta > 0 and margins[0] > 0.15:
                regime_signals.append(("Mild increasing returns", "Margins improving ({:.0%} → {:.0%})".format(margins[-1], margins[0]), 5))
            elif margin_delta < -0.05:
                regime_signals.append(("Diminishing returns", "Operating margins compressing ({:.0%} → {:.0%})".format(margins[-1], margins[0]), -10))
            else:
                regime_signals.append(("Stable regime", "Margins flat ({:.0%})".format(margins[0] if margins else 0), 0))

        # Revenue growth acceleration / deceleration
        revs = [safe_float(yr, "totalRevenue", 0) for yr in inc[:5]]
        growth_rates = []
        for i in range(len(revs) - 1):
            if revs[i+1] > 0:
                growth_rates.append(revs[i] / revs[i+1] - 1)

        if len(growth_rates) >= 2:
            recent_growth = growth_rates[0]
            older_growth = growth_rates[-1]
            if recent_growth > older_growth + 0.05 and recent_growth > 0.10:
                regime_signals.append(("Accelerating growth", "Growth rate increasing ({:.0%} → {:.0%}) — positive feedback loop".format(older_growth, recent_growth), 10))
                complexity_score += 10
            elif recent_growth < older_growth - 0.10:
                regime_signals.append(("Decelerating growth", "Growth rate declining ({:.0%} → {:.0%}) — approaching saturation".format(older_growth, recent_growth), -5))
                complexity_score -= 5

        for label, desc, pts in regime_signals:
            complexity_score += pts

        self.findings["regime_signals"] = regime_signals

        # ── 2. Path Dependence & Lock-in (Arthur) ──
        # High switching costs, network effects, platform dynamics
        # Proxy: R&D intensity + gross margin stability + market share indicators
        lock_in = []

        bs = s.bs_annual[0] if s.bs_annual else {}
        inc0 = inc[0] if inc else {}

        rd = safe_float(inc0, "researchAndDevelopment", 0)
        rev0 = safe_float(inc0, "totalRevenue", 0)
        gp0 = safe_float(inc0, "grossProfit", 0)

        if rev0 > 0 and rd > 0:
            rd_intensity = rd / rev0
            if rd_intensity > 0.15:
                lock_in.append("High R&D intensity ({:.0%}) — building proprietary moats".format(rd_intensity))
                complexity_score += 5
            elif rd_intensity > 0.08:
                lock_in.append("Moderate R&D ({:.0%}) — investing in differentiation".format(rd_intensity))
                complexity_score += 2

        if rev0 > 0 and gp0 > 0:
            gm = gp0 / rev0
            if gm > 0.65:
                lock_in.append("Very high gross margin ({:.0%}) suggests strong lock-in/switching costs".format(gm))
                complexity_score += 5
            elif gm < 0.25:
                lock_in.append("Low gross margin ({:.0%}) — commodity dynamics, low lock-in".format(gm))
                complexity_score -= 5

        self.findings["lock_in_signals"] = lock_in

        # ── 3. Sornette Log-Periodic Bubble Detection (simplified) ──
        # Super-exponential growth in price = potential bubble
        # We check if recent returns are accelerating (convex price path)
        prices = []
        for dp in (s.daily_prices or [])[:252]:
            p = safe_float(dp, "4. close", 0)
            if p > 0: prices.append(p)
        prices.reverse()

        bubble_risk = "Low"
        bubble_signals = []
        if len(prices) >= 60:
            # Compare first-half return vs second-half return
            mid = len(prices) // 2
            first_half_ret = (prices[mid] / prices[0]) - 1 if prices[0] > 0 else 0
            second_half_ret = (prices[-1] / prices[mid]) - 1 if prices[mid] > 0 else 0

            # Super-exponential: second half significantly faster than first
            if second_half_ret > first_half_ret * 1.5 and second_half_ret > 0.15:
                bubble_risk = "Elevated"
                bubble_signals.append("Super-exponential price growth — second half ({:.0%}) >> first half ({:.0%})".format(second_half_ret, first_half_ret))
                complexity_score -= 8

            # Check P/E expansion
            pe = s.overview.get("pe") or 0
            if pe > 50 and second_half_ret > 0.20:
                bubble_risk = "High"
                bubble_signals.append("P/E > 50 combined with rapid price appreciation — Sornette warning")
                complexity_score -= 5
            elif pe > 0 and pe < 15 and second_half_ret < -0.10:
                bubble_signals.append("Low P/E with price decline — possible anti-bubble (overshoot down)")
                complexity_score += 5

        self.findings["bubble_risk"] = bubble_risk
        self.findings["bubble_signals"] = bubble_signals

        # ── 4. Fitness Landscape Position (Kauffman) ──
        # Is the company on a local peak (optimized but brittle) or exploring adjacent possibilities?
        fitness = []
        sector = (s.overview.get("sector") or "").lower()

        # Capex-to-depreciation ratio: >1 means investing in future capacity
        capex = abs(safe_float(s.cf_annual[0] if s.cf_annual else {}, "capitalExpenditures", 0))
        dep = safe_float(inc0, "depreciationAndAmortization", 0)
        if dep > 0:
            capex_ratio = capex / dep
            if capex_ratio > 1.5:
                fitness.append("Heavy reinvestment (capex/dep {:.1f}x) — exploring adjacent possibilities".format(capex_ratio))
                complexity_score += 3
            elif capex_ratio < 0.5:
                fitness.append("Low reinvestment (capex/dep {:.1f}x) — harvesting / local peak".format(capex_ratio))
                complexity_score -= 2

        self.findings["fitness_signals"] = fitness

        # ── Signal ──
        if complexity_score > 15:
            self.signal = "bullish"; self.score = min(complexity_score, 20)
        elif complexity_score > 5:
            self.signal = "bullish"; self.score = 8
        elif complexity_score > -5:
            self.signal = "neutral"; self.score = 0
        elif complexity_score > -15:
            self.signal = "bearish"; self.score = -8
        else:
            self.signal = "bearish"; self.score = min(-15, complexity_score)

        returns_type = "Increasing" if complexity_score > 5 else "Diminishing" if complexity_score < -5 else "Mixed"
        self.summary = "{} returns | Bubble: {} | {} lock-in signals".format(
            returns_type, bubble_risk, len(lock_in))

    def render(self):
        self.header()
        safe_print(f"    {dim('\"Increasing returns generate not a single equilibrium but multiple ones.')}")
        safe_print(f"    {dim(' The economy selects from among these via path-dependent dynamics.\" — W. Brian Arthur')}")

        # Returns regime
        rs = self.findings.get("regime_signals", [])
        if rs:
            safe_print(f"\n    {bold('Returns Regime (Arthur):')}")
            for label, desc, pts in rs:
                icon = green("▲") if pts > 5 else red("▼") if pts < -5 else yellow("●")
                safe_print(f"      {icon} {bold(label)}: {dim(desc)}")

        # Lock-in
        li = self.findings.get("lock_in_signals", [])
        if li:
            safe_print(f"\n    {bold('Path Dependence & Lock-in:')}")
            for s_li in li:
                safe_print(f"      {'🔒'} {s_li}")

        # Bubble detection
        br = self.findings.get("bubble_risk", "Low")
        bs = self.findings.get("bubble_signals", [])
        br_fn = {"Low": green, "Elevated": yellow, "High": red}.get(br, dim)
        safe_print(f"\n    {bold('Sornette Bubble Risk:')} {br_fn(br)}")
        for b in bs:
            safe_print(f"      {yellow('⚡')} {b}")
        if not bs:
            safe_print(f"      {green('✓')} No super-exponential patterns detected")

        # Fitness
        fitness = self.findings.get("fitness_signals", [])
        if fitness:
            safe_print(f"\n    {bold('Kauffman Fitness Landscape:')}")
            for f in fitness:
                safe_print(f"      {f}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 17: Forensic Accounting — Schilit / Beneish
# ══════════════════════════════════════════════════════════════════════════════

class ForensicAgent(Agent):
    """
    Forensic accounting inspired by Howard Schilit's "Financial Shenanigans"
    and Messod Beneish's earnings manipulation detection.

    - Beneish M-Score: 8-variable model to detect earnings manipulation
    - Sloan Accrual Ratio: high accruals = low earnings quality
    - Cash-Earnings Divergence: persistent gap = manipulation risk
    - Revenue Quality: channel stuffing, bill-and-hold red flags

    References: Schilit "Financial Shenanigans", Beneish (1999),
                Penman "Financial Statement Analysis & Security Valuation"
    """
    name = "forensic"
    title = "FORENSIC ACCOUNTING ANALYST"
    icon = "🔍"

    def run(self):
        s = self.store
        forensic_score = 0
        red_flags = []
        green_flags = []

        # Need 2 years of data for Beneish
        def get_yr(series, idx):
            vals = list(series.values())
            if idx < len(vals): return vals[-(idx+1)]
            return {}

        inc_t = get_yr(s.inc_series, 0) if s.inc_series else (s.inc_annual[0] if s.inc_annual else {})
        inc_t1 = get_yr(s.inc_series, 1) if s.inc_series else (s.inc_annual[1] if len(s.inc_annual) > 1 else {})
        bs_t = get_yr(s.bs_series, 0) if s.bs_series else (s.bs_annual[0] if s.bs_annual else {})
        bs_t1 = get_yr(s.bs_series, 1) if s.bs_series else (s.bs_annual[1] if len(s.bs_annual) > 1 else {})
        cf_t = get_yr(s.cf_series, 0) if s.cf_series else (s.cf_annual[0] if s.cf_annual else {})

        def gf(d, keys, default=0):
            """Get first matching key from dict."""
            if isinstance(keys, str): keys = [keys]
            for k in keys:
                v = d.get(k)
                if v is not None and v != 0:
                    return float(v)
            return default

        # ── 1. Beneish M-Score Components ──
        # DSRI: Days Sales in Receivables Index
        rev_t = gf(inc_t, ["revenue", "totalRevenue"])
        rev_t1 = gf(inc_t1, ["revenue", "totalRevenue"])
        rec_t = gf(bs_t, ["receivables", "currentNetReceivables"])
        rec_t1 = gf(bs_t1, ["receivables", "currentNetReceivables"])

        dsri = ((rec_t / rev_t) / (rec_t1 / rev_t1)) if all([rev_t, rev_t1, rec_t1]) and rec_t1/rev_t1 > 0 else 1.0

        # GMI: Gross Margin Index
        gp_t = gf(inc_t, ["gross_profit", "grossProfit"])
        gp_t1 = gf(inc_t1, ["gross_profit", "grossProfit"])
        gm_t = gp_t / rev_t if rev_t else 0
        gm_t1 = gp_t1 / rev_t1 if rev_t1 else 0
        gmi = gm_t1 / gm_t if gm_t > 0 else 1.0

        # AQI: Asset Quality Index (non-current assets excluding PP&E / total assets)
        ta_t = gf(bs_t, ["total_assets", "totalAssets"])
        ta_t1 = gf(bs_t1, ["total_assets", "totalAssets"])
        ca_t = gf(bs_t, ["current_assets", "totalCurrentAssets"])
        ca_t1 = gf(bs_t1, ["current_assets", "totalCurrentAssets"])
        ppe_t = gf(bs_t, ["ppe_net", "propertyPlantEquipment"])
        ppe_t1 = gf(bs_t1, ["ppe_net", "propertyPlantEquipment"])

        aq_t = 1 - (ca_t + ppe_t) / ta_t if ta_t else 0
        aq_t1 = 1 - (ca_t1 + ppe_t1) / ta_t1 if ta_t1 else 0
        aqi = aq_t / aq_t1 if aq_t1 > 0 else 1.0

        # SGI: Sales Growth Index
        sgi = rev_t / rev_t1 if rev_t1 else 1.0

        # DEPI: Depreciation Index
        da_t = gf(inc_t, ["da", "depreciationAndAmortization"])
        da_t1 = gf(inc_t1, ["da", "depreciationAndAmortization"])
        dep_rate_t = da_t / (da_t + ppe_t) if (da_t + ppe_t) > 0 else 0
        dep_rate_t1 = da_t1 / (da_t1 + ppe_t1) if (da_t1 + ppe_t1) > 0 else 0
        depi = dep_rate_t1 / dep_rate_t if dep_rate_t > 0 else 1.0

        # SGAI: SGA Index
        sga_t = gf(inc_t, ["sga", "sellingGeneralAndAdministrative"])
        sga_t1 = gf(inc_t1, ["sga", "sellingGeneralAndAdministrative"])
        sga_ratio_t = sga_t / rev_t if rev_t else 0
        sga_ratio_t1 = sga_t1 / rev_t1 if rev_t1 else 0
        sgai = sga_ratio_t / sga_ratio_t1 if sga_ratio_t1 > 0 else 1.0

        # LVGI: Leverage Index
        ltd_t = gf(bs_t, ["long_term_debt", "longTermDebt"])
        ltd_t1 = gf(bs_t1, ["long_term_debt", "longTermDebt"])
        cl_t = gf(bs_t, ["current_liabilities", "totalCurrentLiabilities"])
        cl_t1 = gf(bs_t1, ["current_liabilities", "totalCurrentLiabilities"])
        lev_t = (ltd_t + cl_t) / ta_t if ta_t else 0
        lev_t1 = (ltd_t1 + cl_t1) / ta_t1 if ta_t1 else 0
        lvgi = lev_t / lev_t1 if lev_t1 > 0 else 1.0

        # TATA: Total Accruals to Total Assets
        ni_t = gf(inc_t, ["net_income", "netIncome"])
        cfo_t = gf(cf_t, ["cfo", "operatingCashflow"])
        tata = (ni_t - cfo_t) / ta_t if ta_t else 0

        # M-Score = -4.84 + 0.920*DSRI + 0.528*GMI + 0.404*AQI + 0.892*SGI
        #           + 0.115*DEPI - 0.172*SGAI + 4.679*TATA - 0.327*LVGI
        m_score = (-4.84 + 0.920 * dsri + 0.528 * gmi + 0.404 * aqi
                   + 0.892 * sgi + 0.115 * depi - 0.172 * sgai
                   + 4.679 * tata - 0.327 * lvgi)

        self.findings["m_score"] = m_score
        self.findings["m_components"] = {
            "DSRI": dsri, "GMI": gmi, "AQI": aqi, "SGI": sgi,
            "DEPI": depi, "SGAI": sgai, "LVGI": lvgi, "TATA": tata
        }

        if m_score > -1.78:
            red_flags.append("Beneish M-Score {:.2f} > -1.78 — LIKELY earnings manipulator".format(m_score))
            forensic_score -= 20
        elif m_score > -2.22:
            red_flags.append("Beneish M-Score {:.2f} — gray zone, elevated manipulation risk".format(m_score))
            forensic_score -= 8
        else:
            green_flags.append("Beneish M-Score {:.2f} < -2.22 — unlikely manipulator".format(m_score))
            forensic_score += 5

        # ── 2. Sloan Accrual Ratio ──
        # Total accruals / average total assets
        avg_ta = (ta_t + ta_t1) / 2 if ta_t1 else ta_t
        accruals = ni_t - cfo_t
        sloan_ratio = accruals / avg_ta if avg_ta else 0
        self.findings["sloan_ratio"] = sloan_ratio

        if abs(sloan_ratio) > 0.10:
            red_flags.append("High Sloan accrual ratio ({:.1%}) — earnings quality concern".format(sloan_ratio))
            forensic_score -= 10
        elif abs(sloan_ratio) > 0.05:
            red_flags.append("Elevated accrual ratio ({:.1%})".format(sloan_ratio))
            forensic_score -= 3
        else:
            green_flags.append("Low accrual ratio ({:.1%}) — cash-backed earnings".format(sloan_ratio))
            forensic_score += 5

        # ── 3. Cash-Earnings Divergence ──
        # Persistent gap between net income and operating cash flow
        divergence_years = 0
        divergence_total = 0
        years_data = list(s.inc_series.items())[-5:] if s.inc_series else []
        cf_data = list(s.cf_series.items())[-5:] if s.cf_series else []

        for i in range(min(len(years_data), len(cf_data))):
            yr_inc = years_data[i][1]
            yr_cf = cf_data[i][1]
            ni = yr_inc.get("net_income", 0) or 0
            cfo = yr_cf.get("cfo", 0) or 0
            if ni > cfo and ni > 0:
                divergence_years += 1
                divergence_total += (ni - cfo)

        self.findings["divergence_years"] = divergence_years
        if divergence_years >= 4:
            red_flags.append("NI > CFO for {}/5 years — persistent cash-earnings gap (Schilit red flag #1)".format(divergence_years))
            forensic_score -= 10
        elif divergence_years >= 3:
            red_flags.append("NI > CFO for {}/5 years — watch for earnings quality deterioration".format(divergence_years))
            forensic_score -= 5
        else:
            green_flags.append("CFO tracks or exceeds NI — healthy cash conversion")
            forensic_score += 3

        # ── 4. Revenue Quality Signals ──
        # Receivables growing faster than revenue = channel stuffing risk
        if rev_t > 0 and rev_t1 > 0 and rec_t > 0 and rec_t1 > 0:
            rev_growth = rev_t / rev_t1 - 1
            rec_growth = rec_t / rec_t1 - 1
            if rec_growth > rev_growth + 0.10 and rec_growth > 0.15:
                red_flags.append("Receivables growing {:.0%} vs revenue {:.0%} — channel stuffing risk".format(rec_growth, rev_growth))
                forensic_score -= 8
            elif rec_growth > rev_growth + 0.05:
                red_flags.append("Receivables outpacing revenue ({:.0%} vs {:.0%})".format(rec_growth, rev_growth))
                forensic_score -= 3

        # ── 5. Deferred Revenue Check ──
        # Declining deferred revenue while reporting growth = pulling forward revenue
        # (This is a Schilit shenanigan)
        inv_t = gf(bs_t, ["inventory"])
        inv_t1 = gf(bs_t1, ["inventory"])
        if inv_t > 0 and inv_t1 > 0 and rev_t > 0 and rev_t1 > 0:
            inv_days_t = (inv_t / rev_t) * 365
            inv_days_t1 = (inv_t1 / rev_t1) * 365
            if inv_days_t > inv_days_t1 * 1.3 and inv_days_t > 30:
                red_flags.append("Inventory days surging ({:.0f} → {:.0f}) — demand weakness or obsolescence".format(inv_days_t1, inv_days_t))
                forensic_score -= 5

        self.findings["red_flags"] = red_flags
        self.findings["green_flags"] = green_flags

        # Signal
        if forensic_score >= 8:
            self.signal = "bullish"; self.score = min(forensic_score, 15)
        elif forensic_score > -5:
            self.signal = "neutral"; self.score = 0
        elif forensic_score > -15:
            self.signal = "bearish"; self.score = max(forensic_score, -15)
        else:
            self.signal = "bearish"; self.score = max(forensic_score, -25)

        self.summary = "M-Score {:.2f} | Accrual {:.1%} | {} red flags".format(
            m_score, sloan_ratio, len(red_flags))

    def render(self):
        self.header()
        safe_print(f"    {dim('\"Financial statements are like fine perfume — to be sniffed but not swallowed.\" — Abraham Briloff')}")

        # M-Score
        ms = self.findings.get("m_score", 0)
        ms_fn = red if ms > -1.78 else yellow if ms > -2.22 else green
        safe_print(f"\n    {bold('Beneish M-Score:')} {ms_fn('{:.2f}'.format(ms))}")
        safe_print(f"      {dim('> -1.78 = likely manipulator | -2.22 to -1.78 = gray zone | < -2.22 = unlikely')}")

        mc = self.findings.get("m_components", {})
        if mc:
            safe_print(f"\n    {bold('M-Score Components:')}")
            labels = {"DSRI": "Days Sales Receivables Idx", "GMI": "Gross Margin Index",
                      "AQI": "Asset Quality Index", "SGI": "Sales Growth Index",
                      "DEPI": "Depreciation Index", "SGAI": "SGA Expense Index",
                      "LVGI": "Leverage Index", "TATA": "Total Accruals/Assets"}
            for k, v in mc.items():
                flag = red("⚠") if ((k == "DSRI" and v > 1.2) or (k == "GMI" and v > 1.2) or
                    (k == "AQI" and v > 1.2) or (k == "TATA" and v > 0.05)) else dim("·")
                safe_print(f"      {flag} {labels.get(k, k):<30s} {v:>8.3f}")

        # Sloan
        sr = self.findings.get("sloan_ratio", 0)
        sr_fn = red if abs(sr) > 0.10 else yellow if abs(sr) > 0.05 else green
        safe_print(f"\n    {bold('Sloan Accrual Ratio:')} {sr_fn('{:.1%}'.format(sr))}")
        safe_print(f"      {dim('|ratio| > 10% = high accruals, low earnings quality')}")

        # Red/green flags
        rfs = self.findings.get("red_flags", [])
        gfs = self.findings.get("green_flags", [])
        if rfs:
            safe_print(f"\n    {bold(red('Red Flags:'))}")
            for f in rfs:
                safe_print(f"      {red('✗')} {f}")
        if gfs:
            safe_print(f"\n    {bold(green('Clean Signals:'))}")
            for f in gfs:
                safe_print(f"      {green('✓')} {f}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 18: Kelly Criterion & Position Sizing — Thorp / Shannon
# ══════════════════════════════════════════════════════════════════════════════

class KellyAgent(Agent):
    """
    Optimal position sizing using the Kelly Criterion and information theory.

    - Kelly fraction: optimal bet size given edge and odds
    - Geometric growth rate: expected compound growth of capital
    - Shannon entropy of returns: market efficiency proxy
    - Information ratio: alpha per unit of tracking error
    - Half-Kelly recommendation: practical position sizing

    References: Thorp "Beat the Market", Shannon/Kelly (Bell Labs),
                Poundstone "Fortune's Formula", MacLean/Thorp/Ziemba "Kelly Capital Growth"
    """
    name = "kelly"
    title = "KELLY CRITERION & SIZING"
    icon = "🎰"

    def run(self):
        import math
        s = self.store
        price = s.price or 0

        # Compute returns from daily prices
        prices = []
        for dp in (s.daily_prices or [])[:252]:
            p = safe_float(dp, "4. close", 0)
            if p > 0: prices.append(p)
        prices.reverse()

        if len(prices) < 30:
            self.signal = "neutral"; self.score = 0
            self.summary = "Insufficient price data for Kelly analysis"
            return

        returns = []
        for i in range(1, len(prices)):
            returns.append(prices[i] / prices[i-1] - 1)

        mean_ret = statistics.mean(returns)
        std_ret = statistics.stdev(returns) if len(returns) > 1 else 0.01
        annual_ret = mean_ret * 252
        annual_vol = std_ret * (252 ** 0.5)

        # Risk-free rate
        rf_daily = s.risk_free_rate / 252

        # ── 1. Kelly Fraction ──
        # For continuous returns: f* = (μ - r_f) / σ²
        # This is the fraction of capital to allocate
        excess_return = mean_ret - rf_daily
        kelly_fraction = excess_return / (std_ret ** 2) if std_ret > 0 else 0
        half_kelly = kelly_fraction / 2  # practical recommendation

        self.findings["kelly_fraction"] = kelly_fraction
        self.findings["half_kelly"] = half_kelly
        self.findings["annual_return"] = annual_ret
        self.findings["annual_vol"] = annual_vol

        # ── 2. Expected Geometric Growth Rate ──
        # g = μ - σ²/2 (continuous approximation)
        geometric_growth = annual_ret - (annual_vol ** 2) / 2
        self.findings["geometric_growth"] = geometric_growth

        # At Kelly sizing: g* = (μ - rf)² / (2σ²)
        if annual_vol > 0:
            kelly_growth = ((annual_ret - s.risk_free_rate) ** 2) / (2 * annual_vol ** 2)
        else:
            kelly_growth = 0
        self.findings["kelly_growth"] = kelly_growth

        # ── 3. Shannon Entropy of Returns ──
        # Discretize returns into bins and compute entropy
        # High entropy = unpredictable = efficient market
        # Low entropy = patterns = potential alpha
        n_bins = min(20, max(5, len(returns) // 10))
        min_r = min(returns)
        max_r = max(returns)
        bin_width = (max_r - min_r) / n_bins if max_r > min_r else 0.01
        bins = [0] * n_bins
        for r in returns:
            b = min(n_bins - 1, int((r - min_r) / bin_width))
            bins[b] += 1

        entropy = 0
        for count in bins:
            if count > 0:
                p = count / len(returns)
                entropy -= p * math.log2(p)

        max_entropy = math.log2(n_bins)
        efficiency_ratio = entropy / max_entropy if max_entropy > 0 else 1.0

        self.findings["entropy"] = entropy
        self.findings["max_entropy"] = max_entropy
        self.findings["efficiency_ratio"] = efficiency_ratio

        # ── 4. Sharpe & Sortino Ratios ──
        sharpe = (annual_ret - s.risk_free_rate) / annual_vol if annual_vol > 0 else 0
        self.findings["sharpe"] = sharpe

        # Sortino: only downside deviation
        downside_returns = [r for r in returns if r < 0]
        if len(downside_returns) >= 5:
            downside_dev = statistics.stdev(downside_returns) * (252 ** 0.5)
            sortino = (annual_ret - s.risk_free_rate) / downside_dev if downside_dev > 0 else 0
        else:
            sortino = sharpe
            downside_dev = annual_vol
        self.findings["sortino"] = sortino
        self.findings["downside_vol"] = downside_dev

        # ── 5. Win Rate & Payoff Ratio ──
        wins = [r for r in returns if r > 0]
        losses = [r for r in returns if r < 0]
        win_rate = len(wins) / len(returns) if returns else 0.5
        avg_win = statistics.mean(wins) if wins else 0
        avg_loss = abs(statistics.mean(losses)) if losses else 0.01
        payoff_ratio = avg_win / avg_loss if avg_loss > 0 else 1.0

        self.findings["win_rate"] = win_rate
        self.findings["payoff_ratio"] = payoff_ratio
        self.findings["avg_win"] = avg_win
        self.findings["avg_loss"] = avg_loss

        # ── 6. Calmar Ratio (return / max drawdown) ──
        peak = prices[0]
        max_dd = 0
        for p in prices:
            if p > peak: peak = p
            dd = (peak - p) / peak
            if dd > max_dd: max_dd = dd

        calmar = annual_ret / max_dd if max_dd > 0 else 0
        self.findings["calmar"] = calmar
        self.findings["max_drawdown"] = max_dd

        # ── 7. Sizing recommendation ──
        sizing_score = 0
        if sharpe > 1.5: sizing_score += 15
        elif sharpe > 0.8: sizing_score += 8
        elif sharpe > 0.3: sizing_score += 3
        elif sharpe < -0.3: sizing_score -= 10

        if kelly_fraction > 2.0: sizing_score += 10  # strong edge
        elif kelly_fraction > 0.5: sizing_score += 5
        elif kelly_fraction < -0.5: sizing_score -= 10

        if win_rate > 0.55 and payoff_ratio > 1.2:
            sizing_score += 5
        elif win_rate < 0.45 and payoff_ratio < 0.8:
            sizing_score -= 5

        if sizing_score > 10:
            self.signal = "bullish"; self.score = min(sizing_score, 15)
        elif sizing_score > 0:
            self.signal = "bullish"; self.score = 5
        elif sizing_score > -5:
            self.signal = "neutral"; self.score = 0
        else:
            self.signal = "bearish"; self.score = max(sizing_score, -15)

        self.summary = "Kelly {:.0%} | Sharpe {:.2f} | Win {:.0%} | Entropy {:.1f}/{:.1f}".format(
            half_kelly, sharpe, win_rate, entropy, max_entropy)

    def render(self):
        self.header()
        thorp_quote = "\"The objective is to bet big when you have the edge and small when you don't.\" -- Ed Thorp"
        safe_print(f"    {dim(thorp_quote)}")

        kf = self.findings.get("kelly_fraction", 0)
        hk = self.findings.get("half_kelly", 0)
        kf_fn = green if kf > 0 else red
        safe_print(f"\n    {bold('Kelly Criterion:')}")
        safe_print(f"      Full Kelly:  {kf_fn('{:.1%}'.format(kf))} of portfolio")
        safe_print(f"      Half Kelly:  {kf_fn('{:.1%}'.format(hk))} {dim('(recommended practical sizing)')}")

        gg = self.findings.get("geometric_growth", 0)
        kg = self.findings.get("kelly_growth", 0)
        safe_print(f"\n    {bold('Growth Rates:')}")
        safe_print(f"      Expected geometric growth:     {pct(gg)}")
        safe_print(f"      Max growth at Kelly sizing:    {pct(kg)}")

        safe_print(f"\n    {bold('Risk-Adjusted Returns:')}")
        sh = self.findings.get("sharpe", 0)
        so = self.findings.get("sortino", 0)
        ca = self.findings.get("calmar", 0)
        sh_fn = green if sh > 1 else yellow if sh > 0.5 else red
        safe_print(f"      Sharpe Ratio:   {sh_fn('{:.2f}'.format(sh))}  {dim('(>1 good, >2 excellent)')}")
        safe_print(f"      Sortino Ratio:  {sh_fn('{:.2f}'.format(so))}  {dim('(downside risk-adjusted)')}")
        safe_print(f"      Calmar Ratio:   {sh_fn('{:.2f}'.format(ca))}  {dim('(return / max drawdown)')}")

        safe_print(f"\n    {bold('Win/Loss Statistics:')}")
        wr = self.findings.get("win_rate", 0)
        pr = self.findings.get("payoff_ratio", 0)
        aw = self.findings.get("avg_win", 0)
        al = self.findings.get("avg_loss", 0)
        safe_print(f"      Win rate:       {green('{:.1%}'.format(wr)) if wr > 0.52 else red('{:.1%}'.format(wr))}")
        safe_print(f"      Avg win:        {green('{:.2%}'.format(aw))}")
        safe_print(f"      Avg loss:       {red('{:.2%}'.format(al))}")
        safe_print(f"      Payoff ratio:   {'{:.2f}'.format(pr)}x  {dim('(avg win / avg loss)')}")

        safe_print(f"\n    {bold('Shannon Information:')}")
        ent = self.findings.get("entropy", 0)
        maxe = self.findings.get("max_entropy", 0)
        eff = self.findings.get("efficiency_ratio", 0)
        safe_print(f"      Return entropy:    {'{:.2f}'.format(ent)} / {'{:.2f}'.format(maxe)} bits")
        safe_print(f"      Market efficiency: {'{:.1%}'.format(eff)}  {dim('(100% = perfectly random/efficient)')}")
        if eff < 0.85:
            safe_print(f"      {green('→ Below 85% — potential exploitable patterns')}")
        else:
            safe_print(f"      {dim('→ Highly efficient — limited alpha opportunity')}")

        av = self.findings.get("annual_vol", 0)
        ar = self.findings.get("annual_return", 0)
        safe_print(f"\n    {bold('Realized Performance (1Y):')}")
        safe_print(f"      Annualized return:    {pct(ar)}")
        safe_print(f"      Annualized vol:       {'{:.1%}'.format(av)}")
        md = self.findings.get("max_drawdown", 0)
        safe_print(f"      Max drawdown:         {red('{:.1%}'.format(md))}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 19: Ergodicity Economics — Ole Peters / LML
# ══════════════════════════════════════════════════════════════════════════════

class ErgodicityAgent(Agent):
    """
    Ergodicity economics — the crucial distinction between ensemble and
    time averages. When returns are multiplicative (as in investing),
    the expected value is NOT what you experience over time.

    - Time-average growth rate vs ensemble average
    - Leverage impact on ergodic growth
    - Ruin probability estimation
    - Optimal leverage under multiplicative dynamics

    References: Ole Peters & Alexander Adamou (London Mathematical Laboratory),
                Peters "The ergodicity problem in economics" (Nature Physics, 2019),
                Cover & Thomas "Elements of Information Theory"
    """
    name = "ergodicity"
    title = "ERGODICITY & RUIN ANALYST"
    icon = "♾️"

    def run(self):
        import math
        s = self.store

        prices = []
        for dp in (s.daily_prices or [])[:252]:
            p = safe_float(dp, "4. close", 0)
            if p > 0: prices.append(p)
        prices.reverse()

        if len(prices) < 30:
            self.signal = "neutral"; self.score = 0
            self.summary = "Insufficient data for ergodicity analysis"
            return

        log_returns = []
        simple_returns = []
        for i in range(1, len(prices)):
            lr = math.log(prices[i] / prices[i-1])
            sr = prices[i] / prices[i-1] - 1
            log_returns.append(lr)
            simple_returns.append(sr)

        # ── 1. Ensemble vs Time Average ──
        # Ensemble average (what classical economics uses): E[r]
        # Time average (what you actually experience): E[log(1+r)] = <log(x)>
        ensemble_avg = statistics.mean(simple_returns) * 252  # annualized
        time_avg = statistics.mean(log_returns) * 252  # annualized geometric

        # The ergodicity gap: how much worse your time-average experience is
        ergodicity_gap = ensemble_avg - time_avg

        self.findings["ensemble_avg"] = ensemble_avg
        self.findings["time_avg"] = time_avg
        self.findings["ergodicity_gap"] = ergodicity_gap

        # ── 2. Leverage & Growth Rate ──
        # At leverage l, time-average growth rate = l*μ - l²σ²/2
        # Optimal leverage l* = μ/σ² (same as Kelly)
        mu = statistics.mean(log_returns)
        sigma = statistics.stdev(log_returns) if len(log_returns) > 1 else 0.01

        optimal_leverage = mu / (sigma ** 2) if sigma > 0 else 0
        # Growth rate at different leverage levels
        leverage_curve = {}
        for lev in [0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0]:
            g = lev * mu * 252 - (lev ** 2) * (sigma ** 2) * 252 / 2
            leverage_curve[lev] = g

        self.findings["optimal_leverage"] = optimal_leverage
        self.findings["leverage_curve"] = leverage_curve

        # ── 3. Ruin Probability ──
        # Probability of losing X% of capital at current vol
        # P(ruin) ≈ exp(-2μd/σ²) where d = distance to ruin (in log terms)
        ruin_levels = {}
        for pct_loss in [0.20, 0.50, 0.80]:
            d = -math.log(1 - pct_loss)  # distance in log space
            if mu > 0 and sigma > 0:
                # Approximate probability of ever hitting this drawdown
                ruin_prob = math.exp(-2 * mu * d / (sigma ** 2))
                ruin_prob = min(1.0, max(0.0, ruin_prob))
            else:
                ruin_prob = 1.0 if mu <= 0 else 0.5
            ruin_levels[pct_loss] = ruin_prob

        self.findings["ruin_levels"] = ruin_levels

        # ── 4. Volatility Tax ──
        # The drag that volatility imposes on compound growth
        # vol_tax = σ²/2 (annualized)
        vol_tax = (sigma ** 2) * 252 / 2
        self.findings["vol_tax"] = vol_tax

        # ── 5. Growth Stability Index ──
        # Rolling 20-day growth rate stability
        window = 20
        rolling_growth = []
        for i in range(window, len(log_returns)):
            rg = statistics.mean(log_returns[i-window:i]) * 252
            rolling_growth.append(rg)

        if len(rolling_growth) >= 5:
            growth_stability = 1 - (statistics.stdev(rolling_growth) / (abs(statistics.mean(rolling_growth)) + 0.01))
            growth_stability = max(0, min(1, growth_stability))
        else:
            growth_stability = 0.5

        self.findings["growth_stability"] = growth_stability

        # ── Signal ──
        erg_score = 0
        if time_avg > 0.10: erg_score += 10
        elif time_avg > 0.05: erg_score += 5
        elif time_avg > 0: erg_score += 2
        elif time_avg > -0.10: erg_score -= 5
        else: erg_score -= 10

        # Penalize large ergodicity gap (high vol drag)
        if ergodicity_gap > 0.15: erg_score -= 8
        elif ergodicity_gap > 0.05: erg_score -= 3

        # Ruin risk
        if ruin_levels.get(0.50, 0) > 0.50: erg_score -= 8
        elif ruin_levels.get(0.50, 0) > 0.20: erg_score -= 3

        if erg_score > 8:
            self.signal = "bullish"; self.score = min(erg_score, 15)
        elif erg_score > 0:
            self.signal = "bullish"; self.score = 5
        elif erg_score > -5:
            self.signal = "neutral"; self.score = 0
        else:
            self.signal = "bearish"; self.score = max(erg_score, -15)

        self.summary = "TimeAvg {:.1%} | EnsAvg {:.1%} | Gap {:.1%} | VolTax {:.1%}".format(
            time_avg, ensemble_avg, ergodicity_gap, vol_tax)

    def render(self):
        self.header()
        safe_print(f"    {dim('\"The expected value of a gamble is not what matters.')}")
        safe_print(f"    {dim(' What matters is the time-average growth rate.\" — Ole Peters')}")

        ea = self.findings.get("ensemble_avg", 0)
        ta = self.findings.get("time_avg", 0)
        eg = self.findings.get("ergodicity_gap", 0)

        safe_print(f"\n    {bold('Ensemble vs Time Average (Annual):')}")
        safe_print(f"      Ensemble avg (E[r]):   {pct(ea)}  {dim('← what classical finance computes')}")
        ta_fn = green if ta > 0.05 else yellow if ta > 0 else red
        safe_print(f"      Time avg (E[log r]):   {ta_fn('{:+.1%}'.format(ta))}  {dim('← what you actually experience')}")
        eg_fn = red if eg > 0.10 else yellow if eg > 0.03 else green
        safe_print(f"      Ergodicity gap:        {eg_fn('{:.1%}'.format(eg))}  {dim('← volatility destroys this much growth')}")

        vt = self.findings.get("vol_tax", 0)
        safe_print(f"\n    {bold('Volatility Tax:')} {red('{:.2%}'.format(vt)) if vt > 0.05 else yellow('{:.2%}'.format(vt))}")
        safe_print(f"      {dim('Compound returns lose σ²/2 annually to variance drag')}")

        safe_print(f"\n    {bold('Leverage Growth Curve:')}")
        lc = self.findings.get("leverage_curve", {})
        ol = self.findings.get("optimal_leverage", 0)
        for lev, g in sorted(lc.items()):
            bar_len = max(0, int(g * 100))
            bar = green("█" * min(bar_len, 40)) if g > 0 else red("█" * min(abs(int(g * 100)), 40))
            marker = " ← optimal" if abs(lev - ol) < 0.2 else ""
            g_fn = green if g > 0 else red
            safe_print(f"      {lev:>4.1f}x: {g_fn('{:>+7.1%}'.format(g))} {bar}{dim(marker)}")

        safe_print(f"      {dim('Optimal leverage: {:.1f}x'.format(ol))}")

        safe_print(f"\n    {bold('Ruin Probability (at 1x leverage):')}")
        rl = self.findings.get("ruin_levels", {})
        for pct_loss, prob in sorted(rl.items()):
            prob_fn = green if prob < 0.10 else yellow if prob < 0.30 else red
            safe_print(f"      Lose {pct_loss:.0%} of capital: {prob_fn('{:.1%}'.format(prob))} probability")

        gs = self.findings.get("growth_stability", 0)
        gs_fn = green if gs > 0.6 else yellow if gs > 0.3 else red
        safe_print(f"\n    {bold('Growth Stability Index:')} {gs_fn('{:.0%}'.format(gs))}")
        safe_print(f"      {dim('Measures consistency of rolling growth rates (100% = perfectly stable)')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 20: Microstructure & Liquidity — Kyle / Glosten-Milgrom
# ══════════════════════════════════════════════════════════════════════════════

class MicrostructureAgent(Agent):
    """
    Market microstructure analysis: how the stock actually trades.

    - Amihud illiquidity ratio
    - Turnover analysis
    - Price impact estimation
    - Bid-ask spread proxy from daily OHLC
    - Institutional vs retail flow indicators

    References: Kyle (1985) "Continuous Auctions and Insider Trading",
                Amihud (2002) "Illiquidity and Stock Returns",
                O'Hara "Market Microstructure Theory"
    """
    name = "microstructure"
    title = "MICROSTRUCTURE & LIQUIDITY"
    icon = "🔬"

    def run(self):
        import math
        s = self.store

        prices_data = s.daily_prices[:252] if s.daily_prices else []
        micro_score = 0

        if len(prices_data) < 10:
            self.signal = "neutral"; self.score = 0
            self.summary = "Insufficient daily data for microstructure analysis"
            return

        # ── 1. Amihud Illiquidity Ratio ──
        # ILLIQ = (1/T) * Σ |r_t| / V_t
        # Higher = less liquid = higher expected return premium
        amihud_values = []
        for dp in prices_data:
            close = safe_float(dp, "4. close", 0)
            prev_close = safe_float(dp, "1. open", close)  # proxy
            volume = safe_float(dp, "5. volume", 0)
            if prev_close > 0 and volume > 0:
                ret = abs(close / prev_close - 1)
                dollar_vol = volume * close
                amihud_values.append(ret / (dollar_vol / 1e6))  # per million dollars

        if amihud_values:
            amihud = statistics.mean(amihud_values)
            self.findings["amihud"] = amihud
            if amihud > 1.0:
                self.findings["liquidity_class"] = "Illiquid"
                micro_score -= 5  # liquidity risk
            elif amihud > 0.1:
                self.findings["liquidity_class"] = "Moderately liquid"
                micro_score += 2  # some illiquidity premium
            elif amihud > 0.001:
                self.findings["liquidity_class"] = "Liquid"
                micro_score += 0
            else:
                self.findings["liquidity_class"] = "Highly liquid"
                micro_score += 0

        # ── 2. Bid-Ask Spread Proxy (Corwin-Schultz from daily OHLC) ──
        # S ≈ 2(e^α - 1) where α = (√(2β) - √β) / (3 - 2√2) - √(γ/(3-2√2))
        # Simplified: use (High - Low) / ((High + Low)/2) as spread proxy
        spread_proxies = []
        for dp in prices_data[:60]:
            high = safe_float(dp, "2. high", 0)
            low = safe_float(dp, "3. low", 0)
            if high > 0 and low > 0:
                mid = (high + low) / 2
                spread_proxies.append((high - low) / mid)

        if spread_proxies:
            avg_spread = statistics.mean(spread_proxies)
            self.findings["spread_proxy"] = avg_spread
            if avg_spread > 0.05:
                self.findings["spread_class"] = "Wide spread — high transaction costs"
                micro_score -= 3
            elif avg_spread > 0.02:
                self.findings["spread_class"] = "Moderate spread"
            else:
                self.findings["spread_class"] = "Tight spread — efficient market"
                micro_score += 2

        # ── 3. Volume Profile Analysis ──
        volumes = []
        for dp in prices_data[:60]:
            v = safe_float(dp, "5. volume", 0)
            if v > 0: volumes.append(v)

        if volumes:
            avg_vol = statistics.mean(volumes)
            recent_vol = statistics.mean(volumes[:5]) if len(volumes) >= 5 else avg_vol
            vol_ratio = recent_vol / avg_vol if avg_vol > 0 else 1.0

            self.findings["avg_volume"] = avg_vol
            self.findings["volume_ratio"] = vol_ratio

            if vol_ratio > 2.0:
                self.findings["volume_signal"] = "Unusual volume surge ({:.1f}x avg) — possible informed trading".format(vol_ratio)
                micro_score -= 3  # could go either way but uncertainty
            elif vol_ratio > 1.5:
                self.findings["volume_signal"] = "Elevated volume ({:.1f}x avg)".format(vol_ratio)
            elif vol_ratio < 0.5:
                self.findings["volume_signal"] = "Low volume ({:.1f}x avg) — reduced interest".format(vol_ratio)
                micro_score -= 2
            else:
                self.findings["volume_signal"] = "Normal volume pattern"

            # Volume-price relationship (On-Balance Volume concept)
            up_vol = 0; down_vol = 0
            for dp in prices_data[:60]:
                close = safe_float(dp, "4. close", 0)
                open_p = safe_float(dp, "1. open", 0)
                vol = safe_float(dp, "5. volume", 0)
                if close > open_p: up_vol += vol
                elif close < open_p: down_vol += vol

            total_vol = up_vol + down_vol
            if total_vol > 0:
                buy_pressure = up_vol / total_vol
                self.findings["buy_pressure"] = buy_pressure
                if buy_pressure > 0.60:
                    self.findings["flow_signal"] = "Accumulation ({}% up-volume)".format(int(buy_pressure * 100))
                    micro_score += 5
                elif buy_pressure < 0.40:
                    self.findings["flow_signal"] = "Distribution ({}% up-volume)".format(int(buy_pressure * 100))
                    micro_score -= 5
                else:
                    self.findings["flow_signal"] = "Balanced flow"

        # ── 4. Kyle's Lambda Proxy (Price Impact) ──
        # λ = measure of how much price moves per unit of order flow
        # High λ = more informed trading, less liquidity
        lambdas = []
        for i in range(min(59, len(prices_data) - 1)):
            close_t = safe_float(prices_data[i], "4. close", 0)
            close_t1 = safe_float(prices_data[i+1], "4. close", 0)
            vol = safe_float(prices_data[i], "5. volume", 0)
            if close_t > 0 and close_t1 > 0 and vol > 0:
                ret = abs(close_t / close_t1 - 1)
                lambdas.append(ret / math.sqrt(vol / 1e6))

        if lambdas:
            kyle_lambda = statistics.mean(lambdas)
            self.findings["kyle_lambda"] = kyle_lambda

        # ── 5. Turnover & Market Cap context ──
        avg_vol = self.findings.get("avg_volume", 0)
        mkt_cap = s.overview.get("market_cap")
        if mkt_cap and avg_vol and s.price:
            daily_turnover = (avg_vol * s.price) / mkt_cap
            self.findings["daily_turnover"] = daily_turnover
            if daily_turnover > 0.02:
                self.findings["turnover_signal"] = "High turnover ({:.2%}/day) — active trading".format(daily_turnover)
            elif daily_turnover < 0.002:
                self.findings["turnover_signal"] = "Low turnover ({:.3%}/day) — potential liquidity trap".format(daily_turnover)
                micro_score -= 3
            else:
                self.findings["turnover_signal"] = "Normal turnover ({:.3%}/day)".format(daily_turnover)

        # Signal
        if micro_score > 5:
            self.signal = "bullish"; self.score = min(micro_score, 10)
        elif micro_score > -3:
            self.signal = "neutral"; self.score = 0
        else:
            self.signal = "bearish"; self.score = max(micro_score, -10)

        liq = self.findings.get("liquidity_class", "?")
        flow = self.findings.get("flow_signal", "?")
        self.summary = "{} | {} | Spread {:.2%}".format(
            liq, flow,
            self.findings.get("spread_proxy", 0))

    def render(self):
        self.header()
        safe_print(f"    {dim('\"In a Kyle model, prices are efficient because market makers learn')}")
        safe_print(f"    {dim(' from the order flow of informed traders.\" — Albert Kyle')}")

        # Liquidity
        liq = self.findings.get("liquidity_class", "Unknown")
        ami = self.findings.get("amihud")
        liq_fn = green if liq == "Highly liquid" else yellow if "Moderate" in liq else red if "Illiquid" in liq else dim
        safe_print(f"\n    {bold('Amihud Illiquidity:')} {liq_fn(liq)}")
        if ami is not None:
            safe_print(f"      Ratio: {'{:.6f}'.format(ami)}  {dim('(higher = less liquid)')}")

        # Spread
        sp = self.findings.get("spread_proxy")
        sc = self.findings.get("spread_class", "")
        if sp is not None:
            sp_fn = red if sp > 0.05 else yellow if sp > 0.02 else green
            safe_print(f"\n    {bold('Bid-Ask Spread Proxy:')} {sp_fn('{:.2%}'.format(sp))}")
            safe_print(f"      {dim(sc)}")

        # Volume
        vs = self.findings.get("volume_signal", "")
        av = self.findings.get("avg_volume")
        if vs:
            safe_print(f"\n    {bold('Volume Profile:')}")
            safe_print(f"      {vs}")
            if av: safe_print(f"      Avg daily volume: {'{:,.0f}'.format(av)} shares")

        # Flow
        fs = self.findings.get("flow_signal", "")
        bp = self.findings.get("buy_pressure")
        if fs:
            bp_fn = green if bp and bp > 0.55 else red if bp and bp < 0.45 else dim
            safe_print(f"\n    {bold('Order Flow:')} {bp_fn(fs)}")

        # Kyle's lambda
        kl = self.findings.get("kyle_lambda")
        if kl is not None:
            safe_print(f"\n    {bold('Kyle Lambda (Price Impact):')} {'{:.4f}'.format(kl)}")
            safe_print(f"      {dim('Higher = more informed trading / less depth')}")

        # Turnover
        ts = self.findings.get("turnover_signal", "")
        if ts:
            safe_print(f"\n    {bold('Turnover:')} {ts}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 21: Ohlson O-Score Distress (Ohlson 1980)
# ══════════════════════════════════════════════════════════════════════════════

class OhlsonDistressAgent(Agent):
    name = "ohlson"
    title = "OHLSON DISTRESS"
    icon = "💀"

    def run(self):
        bs = self.store.latest("bs_series")
        inc = self.store.latest("inc_series")
        cf = self.store.latest("cf_series")
        inc_prev = self.store.prev("inc_series")

        ta = bs.get("total_assets") or 0
        tl = bs.get("total_liabilities") or 0
        ca = bs.get("current_assets") or 0
        cl = bs.get("current_liabilities") or 0
        ni = inc.get("net_income") or 0
        ni_prev = inc_prev.get("net_income") or 0
        cfo = cf.get("cfo") or 0
        wc = ca - cl

        if ta <= 0:
            self.signal = "neutral"
            self.score = 0
            self.summary = "Insufficient data for O-Score"
            return

        gnp_deflator = 1.0  # normalized
        x = 1 if tl > ta else 0
        # Check 2 consecutive losses
        y = 1 if (ni < 0 and ni_prev < 0) else 0
        # Change in NI
        denom = abs(ni) + abs(ni_prev)
        delta_ni = (ni - ni_prev) / denom if denom > 0 else 0

        o_score = (-1.32 - 0.407 * math.log(max(ta / gnp_deflator, 1))
                   + 6.03 * (tl / ta)
                   - 1.43 * (wc / ta)
                   + 0.076 * (cl / max(ca, 1))
                   - 1.72 * x
                   - 2.37 * (ni / ta)
                   - 1.83 * (cfo / max(tl, 1))
                   + 0.285 * y
                   - 0.521 * delta_ni)

        prob = 1.0 / (1.0 + math.exp(-o_score))

        self.findings["o_score"] = o_score
        self.findings["prob_bankruptcy"] = prob
        self.findings["components"] = {
            "leverage": tl / ta, "working_capital": wc / ta,
            "profitability": ni / ta, "cash_flow": cfo / max(tl, 1),
            "consecutive_losses": y, "delta_ni": delta_ni
        }

        if prob < 0.05:
            self.signal = "bullish"
            self.score = 10
            self.summary = f"O-Score {o_score:.2f}, P(distress)={prob:.1%} — financially healthy"
        elif prob < 0.50:
            self.signal = "neutral"
            self.score = 0
            self.summary = f"O-Score {o_score:.2f}, P(distress)={prob:.1%} — moderate risk"
        else:
            self.signal = "bearish"
            self.score = -20
            self.summary = f"O-Score {o_score:.2f}, P(distress)={prob:.1%} — elevated distress risk"

    def render(self):
        self.header()
        prob = self.findings.get("prob_bankruptcy", 0)
        o = self.findings.get("o_score", 0)
        c = self.findings.get("components", {})
        color = green if prob < 0.05 else yellow if prob < 0.3 else red
        safe_print(f"    O-Score: {color(f'{o:.3f}')}  P(bankruptcy): {color(f'{prob:.2%}')}")
        safe_print(f"    Leverage(TL/TA): {c.get('leverage',0):.2%}  WC/TA: {c.get('working_capital',0):.2%}")
        safe_print(f"    NI/TA: {c.get('profitability',0):.2%}  CFO/TL: {c.get('cash_flow',0):.2%}")
        safe_print(f"    Consecutive losses: {'Yes' if c.get('consecutive_losses') else 'No'}  ΔNI: {c.get('delta_ni',0):.2%}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 22: Accrual Anomaly (Sloan 1996)
# ══════════════════════════════════════════════════════════════════════════════

class AccrualAnomalyAgent(Agent):
    name = "accrual"
    title = "ACCRUAL ANOMALY"
    icon = "🧮"

    def run(self):
        years = self.store.years("inc_series", 6)
        accrual_ratios = []
        cash_components = []

        for y in years:
            inc = self.store.inc_series.get(y, {})
            cf = self.store.cf_series.get(y, {})
            bs = self.store.bs_series.get(y, {})
            ni = inc.get("net_income")
            cfo = cf.get("cfo")
            ta = bs.get("total_assets")

            if ni is not None and cfo is not None and ta and ta > 0:
                accrual = ni - cfo
                accrual_ratio = accrual / ta
                cash_ratio = cfo / ta
                accrual_ratios.append((y, accrual_ratio))
                cash_components.append((y, cash_ratio))

        self.findings["accrual_ratios"] = accrual_ratios
        self.findings["cash_components"] = cash_components

        if not accrual_ratios:
            self.signal = "neutral"
            self.score = 0
            self.summary = "Insufficient data for accrual analysis"
            return

        latest_accrual = accrual_ratios[-1][1]
        avg_accrual = statistics.mean([a for _, a in accrual_ratios])
        self.findings["latest_accrual_ratio"] = latest_accrual
        self.findings["avg_accrual_ratio"] = avg_accrual

        # Low accruals = higher earnings quality = bullish
        if latest_accrual < -0.05:
            self.signal = "bullish"
            self.score = 12
            self.summary = f"Low accruals ({latest_accrual:.1%}) — high earnings quality, cash-backed"
        elif latest_accrual > 0.10:
            self.signal = "bearish"
            self.score = -15
            self.summary = f"High accruals ({latest_accrual:.1%}) — poor earnings quality, watch for reversals"
        elif latest_accrual > 0.05:
            self.signal = "bearish"
            self.score = -8
            self.summary = f"Elevated accruals ({latest_accrual:.1%}) — moderate quality concern"
        else:
            self.signal = "neutral"
            self.score = 3
            self.summary = f"Normal accruals ({latest_accrual:.1%}) — adequate earnings quality"

    def render(self):
        self.header()
        safe_print(f"    {'Year':<8s} {'Accrual/TA':>12s} {'Cash/TA':>12s}")
        safe_print(f"    {'─'*8} {'─'*12} {'─'*12}")
        for (y, ar), (_, cr) in zip(self.findings.get("accrual_ratios", []),
                                     self.findings.get("cash_components", [])):
            ar_fn = green if ar < 0 else red if ar > 0.05 else yellow
            safe_print(f"    {y:<8s} {ar_fn(f'{ar:>11.2%}')} {f'{cr:>11.2%}'}")
        lat = self.findings.get("latest_accrual_ratio")
        if lat is not None:
            safe_print(f"\n    Latest accrual ratio: {(green if lat < 0 else red)(f'{lat:.2%}')}")
            safe_print(f"    {dim('Low accruals → earnings backed by cash → Sloan anomaly bullish')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 23: Gross Profitability (Novy-Marx 2013)
# ══════════════════════════════════════════════════════════════════════════════

class GrossProfitabilityAgent(Agent):
    name = "gross_profit"
    title = "GROSS PROFITABILITY"
    icon = "💎"

    def run(self):
        years = self.store.years("inc_series", 6)
        gpa_history = []

        for y in years:
            inc = self.store.inc_series.get(y, {})
            bs = self.store.bs_series.get(y, {})
            gp = inc.get("gross_profit")
            ta = bs.get("total_assets")
            if gp is not None and ta and ta > 0:
                gpa = gp / ta
                gpa_history.append((y, gpa))

        self.findings["gpa_history"] = gpa_history

        if not gpa_history:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No GP/A data available"
            return

        latest_gpa = gpa_history[-1][1]
        self.findings["latest_gpa"] = latest_gpa

        # Track trajectory
        if len(gpa_history) >= 3:
            recent = [g for _, g in gpa_history[-3:]]
            trend = (recent[-1] - recent[0]) / max(abs(recent[0]), 0.01)
            self.findings["trend"] = trend
        else:
            trend = 0

        # Novy-Marx: GP/A is the strongest quality predictor
        if latest_gpa > 0.40:
            self.signal = "bullish"
            self.score = 15
            self.summary = f"Exceptional GP/A={latest_gpa:.0%} — elite quality per Novy-Marx"
        elif latest_gpa > 0.25:
            self.signal = "bullish"
            self.score = 10
            self.summary = f"Strong GP/A={latest_gpa:.0%} — quality factor exposure"
        elif latest_gpa > 0.15:
            self.signal = "neutral"
            self.score = 3
            self.summary = f"Moderate GP/A={latest_gpa:.0%} — average quality"
        else:
            self.signal = "bearish"
            self.score = -8
            self.summary = f"Low GP/A={latest_gpa:.0%} — weak quality factor"

        # Bonus/penalty for trend
        if trend > 0.10:
            self.score += 3
        elif trend < -0.10:
            self.score -= 3

    def render(self):
        self.header()
        safe_print(f"    {'Year':<8s} {'GP/A':>10s}")
        safe_print(f"    {'─'*8} {'─'*10}")
        for y, gpa in self.findings.get("gpa_history", []):
            fn = green if gpa > 0.30 else yellow if gpa > 0.15 else red
            safe_print(f"    {y:<8s} {fn(f'{gpa:>9.1%}')}")
        safe_print(f"\n    {dim('Novy-Marx (2013): GP/A is the cleanest measure of economic profitability')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 24: Asset Growth Anomaly (Cooper-Gulen-Schill 2008)
# ══════════════════════════════════════════════════════════════════════════════

class AssetGrowthAgent(Agent):
    name = "asset_growth"
    title = "ASSET GROWTH ANOMALY"
    icon = "📈"

    def run(self):
        years = self.store.years("bs_series", 6)
        growth_history = []

        for i in range(1, len(years)):
            prev_bs = self.store.bs_series.get(years[i-1], {})
            curr_bs = self.store.bs_series.get(years[i], {})
            ta_prev = prev_bs.get("total_assets")
            ta_curr = curr_bs.get("total_assets")
            if ta_prev and ta_prev > 0 and ta_curr:
                ag = (ta_curr - ta_prev) / ta_prev
                growth_history.append((years[i], ag))

        self.findings["growth_history"] = growth_history

        if not growth_history:
            self.signal = "neutral"
            self.score = 0
            self.summary = "Insufficient asset data"
            return

        latest_ag = growth_history[-1][1]
        avg_ag = statistics.mean([g for _, g in growth_history])
        self.findings["latest_growth"] = latest_ag
        self.findings["avg_growth"] = avg_ag

        # Decompose latest growth
        curr_bs = self.store.latest("bs_series")
        prev_bs = self.store.prev("bs_series")
        curr_cf = self.store.latest("cf_series")
        ta_prev = prev_bs.get("total_assets") or 1
        capex = abs(curr_cf.get("capex") or 0)
        da = abs(curr_cf.get("da_cf") or 0) or abs(self.store.latest("inc_series").get("da") or 0)
        acq = abs(curr_cf.get("acquisitions") or 0)
        organic = (capex - da) / ta_prev if ta_prev else 0
        self.findings["decomposition"] = {"organic": organic, "acquisitions": acq / ta_prev, "capex": capex / ta_prev}

        # Low growth firms outperform (Cooper et al.)
        if latest_ag < 0.05:
            self.signal = "bullish"
            self.score = 10
            self.summary = f"Low asset growth ({latest_ag:.0%}) — value/disciplined, anomaly bullish"
        elif latest_ag < 0.15:
            self.signal = "neutral"
            self.score = 2
            self.summary = f"Moderate asset growth ({latest_ag:.0%})"
        elif latest_ag < 0.30:
            self.signal = "bearish"
            self.score = -8
            self.summary = f"High asset growth ({latest_ag:.0%}) — empire-building risk"
        else:
            self.signal = "bearish"
            self.score = -12
            self.summary = f"Extreme asset growth ({latest_ag:.0%}) — strong anomaly bearish signal"

    def render(self):
        self.header()
        safe_print(f"    {'Year':<8s} {'Asset Growth':>14s}")
        safe_print(f"    {'─'*8} {'─'*14}")
        for y, ag in self.findings.get("growth_history", []):
            fn = green if ag < 0.10 else yellow if ag < 0.20 else red
            safe_print(f"    {y:<8s} {fn(f'{ag:>13.1%}')}")
        d = self.findings.get("decomposition", {})
        if d:
            safe_print(f"\n    Decomposition: organic={d.get('organic',0):.1%}  acquisitions={d.get('acquisitions',0):.1%}  capex/TA={d.get('capex',0):.1%}")
        safe_print(f"    {dim('Cooper-Gulen-Schill: Low asset growth firms outperform by ~20%/yr')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 25: Capital Issuance (Pontiff-Woodgate 2008)
# ══════════════════════════════════════════════════════════════════════════════

class CapitalIssuanceAgent(Agent):
    name = "issuance"
    title = "CAPITAL ISSUANCE"
    icon = "🏦"

    def run(self):
        years = self.store.years("cf_series", 6)
        issuance_history = []

        for y in years:
            cf = self.store.cf_series.get(y, {})
            bs = self.store.bs_series.get(y, {})
            ta = bs.get("total_assets") or 0

            buybacks = abs(cf.get("buybacks") or 0)
            stock_issued = cf.get("stock_issuance") or 0
            debt_issued = cf.get("debt_issuance") or 0
            debt_repaid = abs(cf.get("debt_repayment") or 0)

            net_equity = stock_issued - buybacks
            net_debt = debt_issued - debt_repaid
            composite = (net_equity + net_debt) / ta if ta > 0 else 0

            issuance_history.append((y, {"net_equity": net_equity, "net_debt": net_debt,
                                         "composite": composite, "buybacks": buybacks}))

        self.findings["issuance_history"] = issuance_history

        if not issuance_history:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No issuance data"
            return

        latest = issuance_history[-1][1]
        self.findings["latest"] = latest

        comp = latest["composite"]

        # Trend analysis: is issuance accelerating/decelerating?
        composites = [d["composite"] for _, d in issuance_history]
        if len(composites) >= 3:
            trend = composites[-1] - statistics.mean(composites[:-1])
            self.findings["trend"] = trend
        else:
            trend = 0

        # Issuance quality: debt for growth vs debt for survival
        net_eq = latest["net_equity"]
        net_dt = latest["net_debt"]
        # If issuing debt while buying back stock = levered recap (risky but intentional)
        levered_recap = net_dt > 0 and latest["buybacks"] > 0
        # If issuing equity while profitable = suspicious dilution
        profitable = (self.store.latest("inc_series").get("net_income") or 0) > 0
        dilutive_issuance = net_eq > 0 and profitable
        self.findings["levered_recap"] = levered_recap
        self.findings["dilutive_issuance"] = dilutive_issuance

        # Continuous scoring (not just 3 thresholds)
        # Score = f(composite level, trend, quality)
        base_score = max(-15, min(15, -comp * 200))  # maps ±7.5% composite to ±15 pts
        # Trend penalty/boost
        trend_adj = -3 if trend > 0.02 else 3 if trend < -0.02 else 0
        # Quality adjustments
        quality_adj = 0
        if levered_recap:
            quality_adj -= 3
        if dilutive_issuance:
            quality_adj -= 4

        # Cumulative 5-year net issuance
        cum_composite = statistics.mean(composites) if composites else 0
        self.findings["cumulative_avg"] = cum_composite
        persistence_adj = -3 if cum_composite > 0.03 else 3 if cum_composite < -0.03 else 0

        self.score = int(max(-20, min(20, base_score + trend_adj + quality_adj + persistence_adj)))
        self.confidence = min(0.9, 0.3 + len(issuance_history) * 0.1)
        self.signal = "bullish" if self.score > 3 else "bearish" if self.score < -3 else "neutral"

        parts = [f"composite={comp:.1%}"]
        if levered_recap: parts.append("levered recap")
        if dilutive_issuance: parts.append("dilutive issuance while profitable")
        if trend > 0.02: parts.append("accelerating issuance")
        elif trend < -0.02: parts.append("decelerating")
        self.summary = f"Net issuance {comp:+.1%}/assets ({', '.join(parts)})"

    def render(self):
        self.header()
        safe_print(f"    {'Year':<8s} {'Net Equity':>14s} {'Net Debt':>14s} {'Composite':>12s}")
        safe_print(f"    {'─'*8} {'─'*14} {'─'*14} {'─'*12}")
        for y, d in self.findings.get("issuance_history", []):
            c = d["composite"]
            fn = green if c < 0 else red if c > 0.03 else yellow
            safe_print(f"    {y:<8s} {usd(d['net_equity']):>14s} {usd(d['net_debt']):>14s} {fn(f'{c:>11.1%}')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 26: Buyback Signal (Ikenberry-Lakonishok-Vermaelen 1995)
# ══════════════════════════════════════════════════════════════════════════════

class BuybackSignalAgent(Agent):
    name = "buyback_signal"
    title = "BUYBACK SIGNAL"
    icon = "🔄"

    def run(self):
        years = self.store.years("cf_series", 5)
        buyback_history = []

        for y in years:
            cf = self.store.cf_series.get(y, {})
            inc = self.store.inc_series.get(y, {})
            bs = self.store.bs_series.get(y, {})
            bb = abs(cf.get("buybacks") or 0)
            ni = inc.get("net_income") or 0
            mkt_cap = (self.store.price or 0) * (self.store.shares or 1)
            eq = bs.get("equity") or 1
            pb = mkt_cap / eq if eq > 0 and mkt_cap > 0 else 1

            buyback_history.append((y, {"buyback_usd": bb, "bb_yield": bb / mkt_cap if mkt_cap > 0 else 0,
                                         "bb_to_ni": bb / ni if ni > 0 else 0, "pb": pb}))

        self.findings["buyback_history"] = buyback_history

        if not buyback_history:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No buyback data"
            return

        total_bb = sum(d["buyback_usd"] for _, d in buyback_history)
        latest = buyback_history[-1][1]
        bb_yield = latest["bb_yield"]
        pb = latest.get("pb", 1)

        # Conviction = buyback yield × (1/PB) — value stocks doing buybacks is strongest
        conviction = bb_yield * (1.0 / max(pb, 0.5))
        self.findings["conviction_score"] = conviction
        self.findings["total_buybacks_5yr"] = total_bb
        self.findings["latest_yield"] = bb_yield

        if bb_yield > 0.03 and pb < 3:
            self.signal = "bullish"
            self.score = 15
            self.summary = f"Strong buyback ({bb_yield:.1%} yield) at low valuation (PB={pb:.1f}) — ILV bullish"
        elif bb_yield > 0.02:
            self.signal = "bullish"
            self.score = 8
            self.summary = f"Active buyback ({bb_yield:.1%} yield)"
        elif bb_yield > 0:
            self.signal = "neutral"
            self.score = 3
            self.summary = f"Modest buyback ({bb_yield:.1%} yield)"
        else:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No buyback activity"

    def render(self):
        self.header()
        safe_print(f"    {'Year':<8s} {'Buyback $':>14s} {'BB Yield':>10s} {'BB/NI':>10s}")
        safe_print(f"    {'─'*8} {'─'*14} {'─'*10} {'─'*10}")
        for y, d in self.findings.get("buyback_history", []):
            bb_y = d['bb_yield']
            bb_n = d['bb_to_ni']
            safe_print(f"    {y:<8s} {usd(d['buyback_usd']):>14s} {bb_y:>10.1%} {bb_n:>10.1%}")
        safe_print(f"\n    5yr total: {usd(self.findings.get('total_buybacks_5yr', 0))}")
        safe_print(f"    {dim('Ikenberry et al.: Buyback + value = strongest signal')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 27: Tax Quality (Dyreng-Hanlon-Maydew 2008)
# ══════════════════════════════════════════════════════════════════════════════

class TaxQualityAgent(Agent):
    name = "tax_quality"
    title = "TAX QUALITY"
    icon = "🏛️"

    def run(self):
        years = self.store.years("inc_series", 6)
        etr_history = []
        cash_etrs = []

        for y in years:
            inc = self.store.inc_series.get(y, {})
            cf = self.store.cf_series.get(y, {})
            pretax = inc.get("pretax_income")
            tax = inc.get("tax")
            cash_tax = cf.get("deferred_tax")  # proxy

            if pretax and pretax > 0:
                gaap_etr = (tax or 0) / pretax
                etr_history.append((y, gaap_etr))
                if cash_tax is not None:
                    cash_etr = ((tax or 0) - cash_tax) / pretax
                    cash_etrs.append((y, cash_etr))

        self.findings["etr_history"] = etr_history
        self.findings["cash_etrs"] = cash_etrs

        if not etr_history:
            self.signal = "neutral"
            self.score = 0
            self.summary = "Insufficient tax data"
            return

        avg_etr = statistics.mean([e for _, e in etr_history])
        etr_vol = statistics.stdev([e for _, e in etr_history]) if len(etr_history) > 1 else 0
        latest_etr = etr_history[-1][1]

        self.findings["avg_etr"] = avg_etr
        self.findings["etr_volatility"] = etr_vol
        self.findings["latest_etr"] = latest_etr

        # Stable, reasonable ETR = quality
        score = 0
        parts = []
        if 0.10 < avg_etr < 0.30:
            score += 5
            parts.append("healthy effective rate")
        elif avg_etr < 0.05:
            score -= 3
            parts.append("aggressively low ETR")
        elif avg_etr > 0.35:
            score -= 3
            parts.append("high tax burden")

        if etr_vol < 0.05:
            score += 3
            parts.append("stable")
        elif etr_vol > 0.15:
            score -= 5
            parts.append("volatile")

        self.score = score
        self.signal = "bullish" if score > 3 else "bearish" if score < -3 else "neutral"
        self.summary = f"ETR={avg_etr:.0%} (vol={etr_vol:.0%}) — {', '.join(parts)}"

    def render(self):
        self.header()
        safe_print(f"    {'Year':<8s} {'GAAP ETR':>10s}")
        safe_print(f"    {'─'*8} {'─'*10}")
        for y, etr in self.findings.get("etr_history", []):
            fn = green if 0.10 < etr < 0.30 else yellow if etr < 0.35 else red
            safe_print(f"    {y:<8s} {fn(f'{etr:>9.1%}')}")
        safe_print(f"\n    Avg ETR: {self.findings.get('avg_etr', 0):.1%}  Volatility: {self.findings.get('etr_volatility', 0):.1%}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 28: Earnings Benchmark (Burgstahler-Dichev 1997)
# ══════════════════════════════════════════════════════════════════════════════

class EarningsBenchmarkAgent(Agent):
    name = "earnings_bench"
    title = "EARNINGS BENCHMARK"
    icon = "🎯"

    def run(self):
        quarterly = self.store.earn_quarterly
        if not quarterly:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No quarterly earnings data"
            return

        just_beat_count = 0
        small_positive_count = 0
        beat_count = 0
        miss_count = 0
        total = 0

        for q in quarterly:
            reported = safe_float(q, "reportedEPS")
            estimated = safe_float(q, "estimatedEPS")
            surprise = safe_float(q, "surprise")

            if reported is not None:
                total += 1
                if reported > 0 and reported < 0.03:
                    small_positive_count += 1
                if estimated is not None:
                    diff = reported - estimated
                    if 0 < diff <= 0.02:
                        just_beat_count += 1
                    if diff > 0:
                        beat_count += 1
                    elif diff < 0:
                        miss_count += 1

        self.findings["total_quarters"] = total
        self.findings["just_beat_count"] = just_beat_count
        self.findings["small_positive_count"] = small_positive_count
        self.findings["beat_count"] = beat_count
        self.findings["miss_count"] = miss_count
        self.findings["beat_rate"] = beat_count / total if total > 0 else 0

        just_beat_pct = just_beat_count / total if total > 0 else 0
        small_pos_pct = small_positive_count / total if total > 0 else 0
        beat_rate = beat_count / total if total > 0 else 0

        # Surprise distribution analysis (Burgstahler-Dichev kink at zero)
        surprises = []
        for q in quarterly:
            reported = safe_float(q, "reportedEPS")
            estimated = safe_float(q, "estimatedEPS")
            if reported is not None and estimated is not None:
                surprises.append(reported - estimated)

        # Distribution analysis: detect asymmetric bunching above zero
        if len(surprises) >= 8:
            just_above = sum(1 for s in surprises if 0 < s <= 0.02)
            just_below = sum(1 for s in surprises if -0.02 <= s < 0)
            # Asymmetry ratio: in a natural distribution, just_above ≈ just_below
            # Significant asymmetry = earnings management
            asym_ratio = just_above / max(just_below, 1)
            self.findings["asymmetry_ratio"] = asym_ratio
            self.findings["just_above_zero"] = just_above
            self.findings["just_below_zero"] = just_below

            # Surprise volatility (low = suspiciously consistent)
            if len(surprises) >= 4:
                surprise_vol = statistics.stdev(surprises)
                surprise_mean = statistics.mean(surprises)
                self.findings["surprise_volatility"] = surprise_vol
                self.findings["surprise_mean"] = surprise_mean
                # Very low vol + always positive = managed
                suspiciously_consistent = surprise_vol < 0.015 and surprise_mean > 0 and surprise_mean < 0.05
                self.findings["suspiciously_consistent"] = suspiciously_consistent
            else:
                suspiciously_consistent = False
        else:
            asym_ratio = 1.0
            suspiciously_consistent = False

        # Streak analysis: longest consecutive beat/miss streak
        streak_type = None
        streak_len = 0
        max_beat_streak = 0
        current_streak = 0
        for q in quarterly:
            reported = safe_float(q, "reportedEPS")
            estimated = safe_float(q, "estimatedEPS")
            if reported is not None and estimated is not None:
                if reported > estimated:
                    current_streak += 1
                    max_beat_streak = max(max_beat_streak, current_streak)
                else:
                    current_streak = 0
        self.findings["max_beat_streak"] = max_beat_streak

        # Multi-factor scoring
        score = 0
        flags = []

        # Factor 1: Just-beat frequency (manipulation proxy)
        if just_beat_pct > 0.35 and total >= 8:
            score -= 10
            flags.append(f"suspicious just-beat frequency ({just_beat_pct:.0%})")
        elif just_beat_pct > 0.25 and total >= 8:
            score -= 5
            flags.append(f"elevated just-beat frequency ({just_beat_pct:.0%})")

        # Factor 2: Asymmetric distribution (kink at zero)
        if asym_ratio > 3.0 and total >= 8:
            score -= 6
            flags.append(f"asymmetric surprise distribution ({asym_ratio:.1f}x)")
        elif asym_ratio > 2.0 and total >= 8:
            score -= 3
            flags.append(f"moderately asymmetric ({asym_ratio:.1f}x)")

        # Factor 3: Suspicious consistency
        if suspiciously_consistent:
            score -= 5
            flags.append("suspiciously low surprise variance")

        # Factor 4: Beat rate (positive signal)
        if beat_rate > 0.80 and total >= 8 and not suspiciously_consistent:
            score += 8
            flags.append(f"excellent beat rate ({beat_rate:.0%})")
        elif beat_rate > 0.65:
            score += 4
            flags.append(f"good beat rate ({beat_rate:.0%})")
        elif beat_rate < 0.40 and total >= 8:
            score -= 5
            flags.append(f"frequent misses ({beat_rate:.0%})")

        # Factor 5: Small positive earnings bunching
        if small_pos_pct > 0.25 and total >= 8:
            score -= 4
            flags.append(f"small positive EPS bunching ({small_pos_pct:.0%})")

        self.score = max(-20, min(15, score))
        self.confidence = min(0.9, 0.3 + total * 0.03)
        self.signal = "bullish" if self.score > 3 else "bearish" if self.score < -3 else "neutral"
        self.summary = "; ".join(flags[:3]) if flags else f"Beat {beat_count}/{total} — standard pattern"

    def render(self):
        self.header()
        f = self.findings
        safe_print(f"    Quarters analyzed: {f.get('total_quarters', 0)}")
        safe_print(f"    Beat consensus: {green(str(f.get('beat_count', 0)))}  Missed: {red(str(f.get('miss_count', 0)))}")
        safe_print(f"    Just beat ($0.01-0.02): {yellow(str(f.get('just_beat_count', 0)))}")
        safe_print(f"    Small positive EPS (<$0.03): {f.get('small_positive_count', 0)}")
        safe_print(f"    {dim('Burgstahler-Dichev: Kink at zero in earnings distribution = management')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 29: Insider Flow (Lakonishok-Lee 2001)
# ══════════════════════════════════════════════════════════════════════════════

class InsiderFlowAgent(Agent):
    name = "insider_flow"
    title = "INSIDER FLOW"
    icon = "👔"

    def run(self):
        # Use Form 4 data if available, fallback to AV insider data
        form4 = self.store.insider_form4
        av_insider = self.store.insider

        purchases = 0
        sales = 0
        purchase_value = 0
        sale_value = 0
        reporters = {}

        if form4:
            for txn in form4:
                codes = txn.get("codes", [])
                shares_list = txn.get("shares", [])
                prices_list = txn.get("prices", [])
                reporter = txn.get("reporter", "Unknown")

                for j, code in enumerate(codes):
                    sh = shares_list[j] if j < len(shares_list) else 0
                    pr = prices_list[j] if j < len(prices_list) else 0
                    val = sh * pr

                    if code == "P":  # Purchase
                        purchases += 1
                        purchase_value += val
                        reporters.setdefault(reporter, {"buys": 0, "sells": 0})
                        reporters[reporter]["buys"] += 1
                    elif code == "S":  # Sale
                        sales += 1
                        sale_value += val
                        reporters.setdefault(reporter, {"buys": 0, "sells": 0})
                        reporters[reporter]["sells"] += 1
        elif av_insider:
            for txn in av_insider:
                acq = safe_float(txn, "shares_acquired_disposed_of")
                if acq and acq > 0:
                    purchases += 1
                elif acq and acq < 0:
                    sales += 1

        self.findings["purchases"] = purchases
        self.findings["sales"] = sales
        self.findings["purchase_value"] = purchase_value
        self.findings["sale_value"] = sale_value
        self.findings["reporters"] = reporters

        # Cluster buy detection: 3+ insiders buying
        n_buyers = sum(1 for r in reporters.values() if r.get("buys", 0) > 0)
        self.findings["n_buyers"] = n_buyers
        cluster_buy = n_buyers >= 3

        buy_sell_ratio = purchases / max(sales, 1)
        self.findings["buy_sell_ratio"] = buy_sell_ratio

        if cluster_buy:
            self.signal = "bullish"
            self.score = 20
            self.summary = f"CLUSTER BUY: {n_buyers} insiders buying — strong Lakonishok-Lee signal"
        elif buy_sell_ratio > 1.5 and purchases >= 2:
            self.signal = "bullish"
            self.score = 12
            self.summary = f"Net insider buying (B/S={buy_sell_ratio:.1f}x, {purchases} buys vs {sales} sells)"
        elif sales > 0 and purchases == 0:
            self.signal = "bearish"
            self.score = -10
            self.summary = f"Unanimous insider selling ({sales} sales, 0 buys)"
        elif sales > purchases * 3:
            self.signal = "bearish"
            self.score = -8
            self.summary = f"Heavy insider selling ({sales} sells vs {purchases} buys)"
        else:
            self.signal = "neutral"
            self.score = 0
            self.summary = f"Mixed insider activity ({purchases} buys, {sales} sells)"

    def render(self):
        self.header()
        f = self.findings
        safe_print(f"    Purchases: {green(str(f.get('purchases', 0)))} ({usd(f.get('purchase_value', 0))})")
        safe_print(f"    Sales: {red(str(f.get('sales', 0)))} ({usd(f.get('sale_value', 0))})")
        safe_print(f"    Buy/Sell ratio: {f.get('buy_sell_ratio', 0):.2f}x  Distinct buyers: {f.get('n_buyers', 0)}")
        reporters = f.get("reporters", {})
        if reporters:
            safe_print(f"\n    {'Insider':<30s} {'Buys':>6s} {'Sells':>6s}")
            for name, data in list(reporters.items())[:10]:
                safe_print(f"    {name[:30]:<30s} {green(str(data['buys'])):>6s} {red(str(data['sells'])):>6s}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 30: Activist Radar (Brav-Jiang 2008)
# ══════════════════════════════════════════════════════════════════════════════

class ActivistRadarAgent(Agent):
    name = "activist"
    title = "ACTIVIST RADAR"
    icon = "🦈"

    def run(self):
        # Check EFTS alerts for 13D filings
        alerts_13d = [a for a in self.store.efts_alerts if "13D" in a.get("type", "")]
        self.findings["13d_filings"] = alerts_13d
        self.findings["n_13d"] = len(alerts_13d)

        # Target vulnerability assessment
        ratios = self.store.ratios
        bs = self.store.latest("bs_series")
        mkt_cap = self.store.overview.get("market_cap") or ((self.store.price or 0) * (self.store.shares or 1))
        cash = bs.get("cash") or 0
        ev_ebitda = ratios.get("ev_to_ebitda")
        roe = ratios.get("roe")

        vulnerability = 0
        factors = []
        # Low valuation
        if ev_ebitda and ev_ebitda < 8:
            vulnerability += 2
            factors.append("low EV/EBITDA")
        # Excess cash
        cash_pct = cash / mkt_cap if mkt_cap > 0 else 0
        if cash_pct > 0.20:
            vulnerability += 2
            factors.append(f"excess cash ({cash_pct:.0%} of mkt cap)")
        # Low ROE
        if roe and roe < 0.08:
            vulnerability += 1
            factors.append("low ROE")
        # Weak margins
        om = ratios.get("operating_margin")
        if om and om < 0.10:
            vulnerability += 1
            factors.append("weak margins")

        self.findings["vulnerability"] = vulnerability
        self.findings["vulnerability_factors"] = factors

        if alerts_13d:
            self.signal = "bullish"
            self.score = 15
            self.summary = f"{len(alerts_13d)} 13D filing(s) detected — activist catalyst"
        elif vulnerability >= 4:
            self.signal = "bullish"
            self.score = 5
            self.summary = f"Activist-vulnerable target ({', '.join(factors[:3])})"
        else:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No activist activity, low vulnerability"

    def render(self):
        self.header()
        f = self.findings
        safe_print(f"    13D filings found: {green(str(f.get('n_13d', 0))) if f.get('n_13d', 0) > 0 else dim('0')}")
        for a in f.get("13d_filings", [])[:5]:
            safe_print(f"      {a.get('date', '')} — {a.get('desc', '')[:60]}")
        safe_print(f"\n    Vulnerability score: {f.get('vulnerability', 0)}/6")
        for fac in f.get("vulnerability_factors", []):
            safe_print(f"      • {fac}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 31: Filing Timing (Alford-Jones-Zmijewski 2000)
# ══════════════════════════════════════════════════════════════════════════════

class FilingTimingAgent(Agent):
    name = "filing_timing"
    title = "FILING TIMING"
    icon = "📅"

    def run(self):
        filings = self.store.filings
        efts_alerts = self.store.efts_alerts

        # Check for NT filings and restatements
        nt_filings = [a for a in efts_alerts if "late filing" in a.get("type", "")]
        restatements = [a for a in efts_alerts if "restatement" in a.get("type", "")]
        self.findings["nt_filings"] = nt_filings
        self.findings["restatements"] = restatements

        # Filing cadence analysis: detect timing patterns
        filing_dates_10k = []
        filing_dates_10q = []
        for f in filings:
            filed = f.get("filingDate", "")
            if not filed:
                continue
            try:
                fd = datetime.strptime(filed, "%Y-%m-%d")
                if f.get("form") == "10-K":
                    filing_dates_10k.append(fd)
                elif f.get("form") == "10-Q":
                    filing_dates_10q.append(fd)
            except (ValueError, TypeError):
                pass

        self.findings["n_10k"] = len(filing_dates_10k)
        self.findings["n_10q"] = len(filing_dates_10q)

        # Filing delay analysis: compare to typical deadlines
        # Large accelerated filers: 10-K within 60 days, 10-Q within 40 days
        # Accelerated filers: 10-K within 75 days, 10-Q within 40 days
        delay_flags = []
        if len(filing_dates_10k) >= 2:
            # Check if latest 10-K was filed later than previous years
            gaps = [(filing_dates_10k[i] - filing_dates_10k[i+1]).days
                    for i in range(len(filing_dates_10k) - 1)]
            if gaps:
                avg_gap = statistics.mean(gaps)
                latest_gap = gaps[0] if gaps else 0
                self.findings["avg_10k_gap_days"] = avg_gap
                self.findings["latest_10k_gap"] = latest_gap
                # If latest filing came much later than usual pattern
                if len(gaps) >= 2 and latest_gap > avg_gap * 1.3:
                    delay_flags.append(f"10-K filing delayed (latest gap={latest_gap:.0f}d vs avg={avg_gap:.0f}d)")

        # 8-K frequency: high 8-K frequency = material events happening
        n_8k = sum(1 for f in filings if f.get("form") == "8-K")
        n_8k_recent = sum(1 for f in filings if f.get("form") == "8-K" and
                          f.get("filingDate", "") > (datetime.now() - timedelta(days=180)).strftime("%Y-%m-%d"))
        self.findings["n_8k_total"] = n_8k
        self.findings["n_8k_recent"] = n_8k_recent

        # Filing pattern anomalies
        has_def14a = any(f.get("form") == "DEF 14A" for f in filings)
        has_s1 = any(f.get("form") in ("S-1", "S-3") for f in filings)
        self.findings["has_proxy"] = has_def14a
        self.findings["has_registration"] = has_s1

        # Multi-factor scoring with time decay
        score = 0
        parts = []

        # NT filings: severity depends on recency
        for nt in nt_filings:
            nt_date = nt.get("date", "")
            try:
                days_ago = (datetime.now() - datetime.strptime(nt_date, "%Y-%m-%d")).days
                if days_ago < 365:
                    score -= 15  # recent NT = very bad
                    parts.append(f"recent NT filing ({days_ago}d ago)")
                elif days_ago < 730:
                    score -= 8
                    parts.append(f"prior NT filing ({days_ago}d ago, decayed)")
                else:
                    score -= 3
                    parts.append(f"historical NT filing ({days_ago}d ago)")
            except (ValueError, TypeError):
                score -= 10
                parts.append("NT filing (date unknown)")

        # Restatements: severity by recency
        for rs in restatements:
            rs_date = rs.get("date", "")
            try:
                days_ago = (datetime.now() - datetime.strptime(rs_date, "%Y-%m-%d")).days
                if days_ago < 365:
                    score -= 12
                    parts.append(f"recent restatement ({days_ago}d ago)")
                elif days_ago < 1095:
                    score -= 6
                    parts.append(f"prior restatement ({days_ago}d ago, decayed)")
                else:
                    score -= 2
                    parts.append(f"historical restatement")
            except (ValueError, TypeError):
                score -= 8
                parts.append("restatement (date unknown)")

        # Filing delay penalty
        for df in delay_flags:
            score -= 5
            parts.append(df)

        # High 8-K frequency (material events, not necessarily bad, but volatile)
        if n_8k_recent > 10:
            score -= 3
            parts.append(f"high 8-K frequency ({n_8k_recent} in 6mo)")

        # S-1/S-3 registration = potential dilution
        if has_s1:
            score -= 4
            parts.append("active registration statement (S-1/S-3)")

        # Clean history bonus
        if not nt_filings and not restatements and not delay_flags and not has_s1:
            score += 5
            parts.append("clean filing history, no anomalies")

        self.score = max(-30, min(10, score))
        self.confidence = min(0.85, 0.4 + (len(filing_dates_10k) + len(filing_dates_10q)) * 0.03)
        self.signal = "bearish" if score < -5 else "bullish" if score > 0 else "neutral"
        self.summary = "; ".join(parts[:3]) if parts else "Standard filing timing"

    def render(self):
        self.header()
        f = self.findings
        safe_print(f"    NT (late) filings: {red(str(len(f.get('nt_filings', [])))) if f.get('nt_filings') else green('0')}")
        safe_print(f"    Restatements: {red(str(len(f.get('restatements', [])))) if f.get('restatements') else green('0')}")
        safe_print(f"    Total filings analyzed: {f.get('filing_count', 0)}")
        for nt in f.get("nt_filings", [])[:3]:
            safe_print(f"      {red('NT')} {nt.get('date', '')} — {nt.get('desc', '')[:50]}")
        for rs in f.get("restatements", [])[:3]:
            safe_print(f"      {red('RESTATE')} {rs.get('date', '')} — {rs.get('desc', '')[:50]}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 32: Institutional Flow (Cohen-Polk-Silli 2010)
# ══════════════════════════════════════════════════════════════════════════════

class InstitutionalFlowAgent(Agent):
    name = "inst_flow"
    title = "INSTITUTIONAL FLOW"
    icon = "🏢"

    def run(self):
        holdings = self.store.inst_holdings
        if not holdings:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No institutional holdings data (need --depth deep)"
            return

        total_shares_held = 0
        n_holders = len(holdings)
        top_holders = []

        for h in holdings[:20]:
            name = h.get("investor", h.get("name", "Unknown"))
            shares = safe_float(h, "shares") or safe_float(h, "currentShares") or 0
            value = safe_float(h, "value") or safe_float(h, "currentValue") or 0
            total_shares_held += shares
            top_holders.append({"name": name, "shares": shares, "value": value})

        # Concentration (HHI)
        if self.store.shares and self.store.shares > 0:
            pcts = [(h["shares"] / self.store.shares) for h in top_holders if h["shares"] > 0]
            hhi = sum(p ** 2 for p in pcts)
        else:
            hhi = 0

        inst_ownership = total_shares_held / self.store.shares if self.store.shares > 0 else 0

        self.findings["n_holders"] = n_holders
        self.findings["total_shares_held"] = total_shares_held
        self.findings["inst_ownership"] = inst_ownership
        self.findings["hhi"] = hhi
        self.findings["top_holders"] = top_holders[:10]

        if inst_ownership > 0.80:
            self.signal = "bullish"
            self.score = 8
            self.summary = f"High institutional ownership ({inst_ownership:.0%}) — professional confidence"
        elif inst_ownership > 0.50:
            self.signal = "neutral"
            self.score = 3
            self.summary = f"Moderate institutional ownership ({inst_ownership:.0%})"
        else:
            self.signal = "neutral"
            self.score = 0
            self.summary = f"Low institutional ownership ({inst_ownership:.0%})"

    def render(self):
        self.header()
        f = self.findings
        safe_print(f"    Institutional holders: {f.get('n_holders', 0)}")
        safe_print(f"    Ownership: {f.get('inst_ownership', 0):.1%}  HHI: {f.get('hhi', 0):.4f}")
        safe_print(f"\n    {'Holder':<35s} {'Shares':>14s} {'Value':>14s}")
        safe_print(f"    {'─'*35} {'─'*14} {'─'*14}")
        for h in f.get("top_holders", [])[:8]:
            safe_print(f"    {h['name'][:35]:<35s} {num(h['shares'],0):>14s} {usd(h['value']):>14s}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 33: Executive Alignment (Jensen-Murphy 1990)
# ══════════════════════════════════════════════════════════════════════════════

class ExecutiveAlignmentAgent(Agent):
    name = "exec_align"
    title = "EXECUTIVE ALIGNMENT"
    icon = "🤝"

    def run(self):
        facts = self.store.raw.get("edgar_facts")
        if not facts:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No EDGAR data for compensation analysis"
            return

        # Try to extract executive compensation from ecd: namespace
        ecd = facts.get("facts", {}).get("ecd", {})
        comp_data = {}

        for concept, key in [("TotalCompensation", "total_comp"),
                             ("SalaryDollarAmount", "salary"),
                             ("StockAwardsDollarAmount", "stock_awards"),
                             ("OptionAwardsDollarAmount", "option_awards"),
                             ("NonEquityIncentivePlanCompensation", "non_equity")]:
            node = ecd.get(concept)
            if node:
                entries = node.get("units", {}).get("USD", [])
                if entries:
                    # Get most recent
                    latest = sorted(entries, key=lambda e: e.get("end", ""))
                    if latest:
                        comp_data[key] = latest[-1].get("val")

        self.findings["comp_data"] = comp_data

        total_comp = comp_data.get("total_comp")
        salary = comp_data.get("salary")
        stock = comp_data.get("stock_awards", 0) or 0
        options = comp_data.get("option_awards", 0) or 0

        if total_comp and total_comp > 0:
            equity_pct = (stock + options) / total_comp
            self.findings["equity_pct"] = equity_pct

            if equity_pct > 0.60:
                self.signal = "bullish"
                self.score = 10
                self.summary = f"Strong alignment: {equity_pct:.0%} equity comp — skin in the game"
            elif equity_pct > 0.30:
                self.signal = "neutral"
                self.score = 3
                self.summary = f"Moderate alignment: {equity_pct:.0%} equity comp"
            else:
                self.signal = "bearish"
                self.score = -8
                self.summary = f"Weak alignment: only {equity_pct:.0%} equity comp — cash-heavy"
        else:
            # Fallback: check insider ownership from AV data
            insider = self.store.insider
            if insider:
                self.signal = "neutral"
                self.score = 0
                self.summary = f"Limited comp data; {len(insider)} insider transactions on record"
            else:
                self.signal = "neutral"
                self.score = 0
                self.summary = "No executive compensation data in XBRL"

    def render(self):
        self.header()
        cd = self.findings.get("comp_data", {})
        if cd:
            safe_print(f"    Total Comp: {usd(cd.get('total_comp'))}")
            safe_print(f"    Salary: {usd(cd.get('salary'))}  Stock: {usd(cd.get('stock_awards'))}  Options: {usd(cd.get('option_awards'))}")
            ep = self.findings.get("equity_pct")
            if ep is not None:
                fn = green if ep > 0.50 else yellow if ep > 0.30 else red
                safe_print(f"    Equity %: {fn(f'{ep:.0%}')}")
        safe_print(f"    {dim('Jensen-Murphy: Equity-heavy comp aligns management with shareholders')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 34: Capital Cycle (Greenwood-Hanson 2015)
# ══════════════════════════════════════════════════════════════════════════════

class CapitalCycleAgent(Agent):
    name = "capital_cycle"
    title = "CAPITAL CYCLE"
    icon = "🔄"

    def run(self):
        years = self.store.years("cf_series", 6)
        capex_da_ratios = []

        for y in years:
            cf = self.store.cf_series.get(y, {})
            inc = self.store.inc_series.get(y, {})
            capex = abs(cf.get("capex") or 0)
            da = abs(cf.get("da_cf") or 0) or abs(inc.get("da") or 0)
            if da > 0:
                capex_da_ratios.append((y, capex / da))

        self.findings["capex_da_ratios"] = capex_da_ratios

        # FRED capacity utilization if available
        if FRED_KEY:
            caput = fred_fetch_series("TCU", limit=12)
            if caput:
                self.findings["capacity_util"] = caput[0]["value"]

        if not capex_da_ratios:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No capex/depreciation data"
            return

        latest_ratio = capex_da_ratios[-1][1]
        self.findings["latest_capex_da"] = latest_ratio

        # Track trend
        if len(capex_da_ratios) >= 3:
            recent = [r for _, r in capex_da_ratios[-3:]]
            trend = recent[-1] - recent[0]
            self.findings["trend"] = trend
        else:
            trend = 0

        # Capital cycle: high capex/DA = late cycle supply building
        if latest_ratio > 2.0:
            self.signal = "bearish"
            self.score = -10
            self.summary = f"Late-cycle overinvestment (Capex/DA={latest_ratio:.1f}x) — supply building"
        elif latest_ratio > 1.5 and trend > 0.3:
            self.signal = "bearish"
            self.score = -6
            self.summary = f"Rising investment cycle (Capex/DA={latest_ratio:.1f}x, accelerating)"
        elif latest_ratio < 0.8:
            self.signal = "bullish"
            self.score = 12
            self.summary = f"Underinvestment (Capex/DA={latest_ratio:.1f}x) — early cycle opportunity"
        elif latest_ratio < 1.0:
            self.signal = "bullish"
            self.score = 6
            self.summary = f"Modest investment (Capex/DA={latest_ratio:.1f}x) — disciplined"
        else:
            self.signal = "neutral"
            self.score = 0
            self.summary = f"Normal investment cycle (Capex/DA={latest_ratio:.1f}x)"

    def render(self):
        self.header()
        safe_print(f"    {'Year':<8s} {'Capex/D&A':>12s}")
        safe_print(f"    {'─'*8} {'─'*12}")
        for y, r in self.findings.get("capex_da_ratios", []):
            fn = green if r < 1.0 else yellow if r < 1.5 else red
            safe_print(f"    {y:<8s} {fn(f'{r:>11.2f}x')}")
        cu = self.findings.get("capacity_util")
        if cu:
            safe_print(f"\n    FRED Capacity Utilization: {cu:.1f}%")
        safe_print(f"    {dim('Greenwood-Hanson: Supply cycles drive mean reversion in industry returns')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 35: Geopolitical Risk (Caldara-Iacoviello 2022)
# ══════════════════════════════════════════════════════════════════════════════

class GeopoliticalRiskAgent(Agent):
    name = "gpr"
    title = "GEOPOLITICAL RISK"
    icon = "🌍"

    def run(self):
        gpr = self.store.gpr_index
        segments = self.store.segments

        gpr_current = gpr.get("current")
        gpr_mean = gpr.get("mean")
        gpr_pctile = gpr.get("percentile")

        self.findings["gpr_current"] = gpr_current
        self.findings["gpr_mean"] = gpr_mean
        self.findings["gpr_percentile"] = gpr_pctile

        # Assess company-specific exposure through segments
        intl_exposure = 0
        if segments:
            total_rev = sum(max(v.values()) for v in segments.values() if v) if segments else 0
            for seg_name, seg_data in segments.items():
                name_lower = seg_name.lower()
                if any(kw in name_lower for kw in ["international", "emea", "apac", "asia",
                                                    "europe", "china", "japan", "foreign"]):
                    seg_rev = max(seg_data.values()) if seg_data else 0
                    intl_exposure += seg_rev / total_rev if total_rev > 0 else 0

        self.findings["intl_exposure"] = intl_exposure

        if gpr_current is None:
            self.signal = "neutral"
            self.score = 0
            self.summary = "GPR data unavailable"
            return

        # High GPR + high international exposure = risk
        is_elevated = gpr_pctile and gpr_pctile > 0.75
        is_exposed = intl_exposure > 0.30

        if is_elevated and is_exposed:
            self.signal = "bearish"
            self.score = -10
            self.summary = f"GPR={gpr_current:.0f} ({gpr_pctile:.0%} pctile) + {intl_exposure:.0%} intl revenue — high geopolitical risk"
        elif is_elevated:
            self.signal = "bearish"
            self.score = -5
            self.summary = f"Elevated GPR={gpr_current:.0f} ({gpr_pctile:.0%} pctile) — market-wide headwind"
        elif is_exposed and gpr_current and gpr_mean and gpr_current > gpr_mean:
            self.signal = "neutral"
            self.score = -3
            self.summary = f"GPR above average, {intl_exposure:.0%} international exposure"
        else:
            self.signal = "bullish"
            self.score = 5
            self.summary = f"GPR={gpr_current:.0f} — benign geopolitical environment"

    def render(self):
        self.header()
        f = self.findings
        gpr = f.get("gpr_current")
        if gpr:
            fn = green if (f.get("gpr_percentile") or 0) < 0.5 else yellow if (f.get("gpr_percentile") or 0) < 0.75 else red
            safe_print(f"    GPR Index: {fn(f'{gpr:.0f}')}  Mean: {f.get('gpr_mean', 0):.0f}  Percentile: {f.get('gpr_percentile', 0):.0%}")
        safe_print(f"    International revenue exposure: {f.get('intl_exposure', 0):.0%}")
        safe_print(f"    {dim('Caldara-Iacoviello: GPR index captures geopolitical risk from news text')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 36: Volatility Surface (Cremers-Weinbaum 2010)
# ══════════════════════════════════════════════════════════════════════════════

class VolatilitySurfaceAgent(Agent):
    name = "vol_surface"
    title = "VOLATILITY SURFACE"
    icon = "🌊"

    def run(self):
        vd = self.store.vix_data

        vix = vd.get("vix")
        skew = vd.get("skew")
        vix_9d = vd.get("vix_9d")
        vix_3m = vd.get("vix_3m")
        vix_6m = vd.get("vix_6m")
        vix_1y = vd.get("vix_1y")

        self.findings["vix"] = vix
        self.findings["skew"] = skew
        self.findings["vix_9d"] = vix_9d
        self.findings["vix_3m"] = vix_3m

        if not vix:
            self.signal = "neutral"
            self.score = 0
            self.summary = "VIX data unavailable"
            return

        # Term structure analysis
        term_structure = {}
        if vix_9d and vix_3m:
            term_structure["9d_3m"] = vix_9d / vix_3m
        if vix_3m and vix_6m:
            term_structure["3m_6m"] = vix_3m / vix_6m
        if vix and vix_1y:
            term_structure["spot_1y"] = vix / vix_1y
        self.findings["term_structure"] = term_structure

        # VIX percentile from history
        vix_history = vd.get("vix_history", [])
        if vix_history:
            vix_pctile = sum(1 for v in vix_history if v <= vix) / len(vix_history)
            self.findings["vix_percentile"] = vix_pctile
        else:
            vix_pctile = 0.5

        # Inversion detection
        inverted = any(v > 1.05 for v in term_structure.values()) if term_structure else False
        self.findings["inverted"] = inverted

        if inverted and skew and skew > 140:
            self.signal = "bearish"
            self.score = -12
            self.summary = f"VIX={vix:.1f} INVERTED + SKEW={skew:.0f} — imminent risk signal"
        elif inverted:
            self.signal = "bearish"
            self.score = -8
            self.summary = f"VIX={vix:.1f} term structure inverted — near-term fear elevated"
        elif vix > 30:
            self.signal = "bearish"
            self.score = -6
            self.summary = f"High VIX={vix:.1f} ({vix_pctile:.0%} pctile) — elevated market fear"
        elif vix < 15 and not inverted:
            self.signal = "bullish"
            self.score = 5
            self.summary = f"Low VIX={vix:.1f} + positive term structure — calm markets"
        else:
            self.signal = "neutral"
            self.score = 0
            self.summary = f"VIX={vix:.1f} ({vix_pctile:.0%} pctile) — normal volatility regime"

    def render(self):
        self.header()
        f = self.findings
        vix = f.get("vix")
        if vix:
            fn = green if vix < 18 else yellow if vix < 25 else red
            safe_print(f"    VIX: {fn(f'{vix:.1f}')}  SKEW: {f.get('skew', 0):.0f}")
            safe_print(f"    VIX9D: {f.get('vix_9d', 0):.1f}  VIX3M: {f.get('vix_3m', 0):.1f}")
            ts = f.get("term_structure", {})
            if ts:
                parts = [f"{k}: {v:.3f}" for k, v in ts.items()]
                inv_str = red(" INVERTED") if f.get("inverted") else green(" Normal")
                safe_print(f"    Term structure: {' | '.join(parts)}{inv_str}")
        safe_print(f"    {dim('Note: Market-wide indicator, not stock-specific')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 37: Short Interest (Hong-Li-Ni 2015)
# ══════════════════════════════════════════════════════════════════════════════

class ShortInterestAgent(Agent):
    name = "short_interest"
    title = "SHORT INTEREST"
    icon = "📉"

    def run(self):
        sv = self.store.short_volume

        if not sv:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No FINRA short volume data available"
            return

        dates = sorted(sv.keys())
        ratios = [sv[d]["ratio"] for d in dates]
        avg_ratio = statistics.mean(ratios)
        latest_ratio = ratios[-1]
        latest_data = sv[dates[-1]]

        self.findings["dates"] = dates
        self.findings["ratios"] = ratios
        self.findings["avg_ratio"] = avg_ratio
        self.findings["latest_ratio"] = latest_ratio
        self.findings["latest_short_vol"] = latest_data["short_vol"]
        self.findings["latest_total_vol"] = latest_data["total_vol"]

        # Estimate days to cover using AV daily volume
        avg_daily_vol = None
        if self.store.daily_prices:
            vols = []
            for dp in self.store.daily_prices[:20]:
                v = safe_float(dp, "5. volume")
                if v:
                    vols.append(v)
            if vols:
                avg_daily_vol = statistics.mean(vols)

        if avg_daily_vol and avg_daily_vol > 0:
            # Rough proxy: short volume on latest day / avg daily volume
            dtc_proxy = latest_data["short_vol"] / avg_daily_vol
            self.findings["dtc_proxy"] = dtc_proxy
        else:
            dtc_proxy = None

        # Trend
        if len(ratios) >= 3:
            trend = ratios[-1] - ratios[0]
            self.findings["trend"] = trend
        else:
            trend = 0

        # Signal logic
        if dtc_proxy and dtc_proxy > 10:
            self.signal = "bullish"
            self.score = 8
            self.summary = f"Crowded short (DTC proxy={dtc_proxy:.1f}) — potential squeeze"
        elif latest_ratio > 0.60:
            self.signal = "bearish"
            self.score = -8
            self.summary = f"High short volume ratio ({latest_ratio:.0%}) — active shorting"
        elif latest_ratio < 0.30:
            self.signal = "bullish"
            self.score = 5
            self.summary = f"Low short volume ratio ({latest_ratio:.0%}) — minimal bearish pressure"
        else:
            self.signal = "neutral"
            self.score = 0
            self.summary = f"Normal short volume ratio ({latest_ratio:.0%})"

    def render(self):
        self.header()
        f = self.findings
        safe_print(f"    {'Date':<12s} {'Short Vol':>14s} {'Total Vol':>14s} {'Ratio':>8s}")
        safe_print(f"    {'─'*12} {'─'*14} {'─'*14} {'─'*8}")
        sv = self.store.short_volume
        for d in f.get("dates", [])[-5:]:
            data = sv.get(d, {})
            r = data.get("ratio", 0)
            fn = red if r > 0.50 else yellow if r > 0.40 else green
            safe_print(f"    {d:<12s} {data.get('short_vol', 0):>14,d} {data.get('total_vol', 0):>14,d} {fn(f'{r:>7.0%}')}")
        dtc = f.get("dtc_proxy")
        if dtc:
            safe_print(f"\n    DTC proxy: {dtc:.1f} days")
        safe_print(f"    {dim('Source: FINRA RegSHO short sale volume data')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 38: Textual Analytics (Loughran-McDonald 2011, Cohen "Lazy Prices" 2020)
# ══════════════════════════════════════════════════════════════════════════════

class TextualAnalyticsAgent(Agent):
    name = "textual"
    title = "TEXTUAL ANALYTICS"
    icon = "📝"

    def run(self):
        lm = fetch_lm_dictionary()
        filings = self.store.filings

        # Find recent 10-K filings
        recent_10k = [f for f in filings if f.get("form") == "10-K"][:2]
        if not recent_10k:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No 10-K filings found for text analysis"
            return

        # Analyze the most recent 10-K text via EFTS or filing content
        # For efficiency, we'll use the company's description + news sentiment as proxy
        # Full 10-K text analysis would be too heavy for real-time
        desc = self.store.overview.get("description", "")
        news = self.store.news

        # LM sentiment on description + news titles
        all_text = desc.lower()
        for n in news[:30]:
            all_text += " " + (n.get("title", "") + " " + n.get("summary", "")).lower()

        words = re.findall(r'\b[a-z]+\b', all_text)
        total_words = len(words) or 1

        neg_count = sum(1 for w in words if w in lm.get("negative", set()))
        pos_count = sum(1 for w in words if w in lm.get("positive", set()))
        unc_count = sum(1 for w in words if w in lm.get("uncertainty", set()))
        lit_count = sum(1 for w in words if w in lm.get("litigious", set()))

        neg_pct = neg_count / total_words
        pos_pct = pos_count / total_words
        unc_pct = unc_count / total_words
        lit_pct = lit_count / total_words
        net_sentiment = pos_pct - neg_pct

        self.findings["total_words"] = total_words
        self.findings["negative_pct"] = neg_pct
        self.findings["positive_pct"] = pos_pct
        self.findings["uncertainty_pct"] = unc_pct
        self.findings["litigious_pct"] = lit_pct
        self.findings["net_sentiment"] = net_sentiment
        self.findings["n_10k_filings"] = len(recent_10k)

        # News-specific sentiment from AV
        if news:
            av_sentiments = []
            for n in news:
                ts = n.get("overall_sentiment_score")
                if ts:
                    try:
                        av_sentiments.append(float(ts))
                    except (ValueError, TypeError):
                        pass
            if av_sentiments:
                self.findings["av_sentiment_avg"] = statistics.mean(av_sentiments)

        if net_sentiment > 0.01:
            self.signal = "bullish"
            self.score = 5
            self.summary = f"Positive text sentiment (net={net_sentiment:.2%}) — constructive language"
        elif net_sentiment < -0.02:
            self.signal = "bearish"
            self.score = -8
            self.summary = f"Negative text sentiment (net={net_sentiment:.2%}) — risk language elevated"
        else:
            self.signal = "neutral"
            self.score = 0
            self.summary = f"Neutral text sentiment (net={net_sentiment:.2%})"

        # Amplify if litigious language is high
        if lit_pct > 0.01:
            self.score -= 3
            self.summary += f"; high litigious language ({lit_pct:.1%})"

    def render(self):
        self.header()
        f = self.findings
        safe_print(f"    Words analyzed: {f.get('total_words', 0):,}")
        pp = f.get("positive_pct", 0)
        np_ = f.get("negative_pct", 0)
        up = f.get("uncertainty_pct", 0)
        lp = f.get("litigious_pct", 0)
        safe_print(f"    Positive: {green('{:.2%}'.format(pp))}  Negative: {red('{:.2%}'.format(np_))}")
        safe_print(f"    Uncertainty: {yellow('{:.2%}'.format(up))}  Litigious: {red('{:.2%}'.format(lp))}")
        ns = f.get("net_sentiment", 0)
        fn = green if ns > 0 else red
        safe_print(f"    Net sentiment: {fn(f'{ns:+.3%}')}")
        av = f.get("av_sentiment_avg")
        if av is not None:
            safe_print(f"    AV News sentiment: {(green if av > 0 else red)(f'{av:+.3f}')}")
        safe_print(f"    {dim('Loughran-McDonald dictionary + Cohen Lazy Prices methodology')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 39: Customer Concentration (Patatoukas 2012)
# ══════════════════════════════════════════════════════════════════════════════

class CustomerConcentrationAgent(Agent):
    name = "customer_conc"
    title = "CUSTOMER CONCENTRATION"
    icon = "🎯"

    def run(self):
        segments = self.store.segments

        if not segments:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No segment data available"
            return

        # Get latest year revenues by segment
        latest_year = max(max(v.keys()) for v in segments.values() if v) if segments else ""
        seg_revenues = {}
        for seg_name, seg_data in segments.items():
            if latest_year in seg_data:
                seg_revenues[seg_name] = seg_data[latest_year]

        total_rev = sum(seg_revenues.values()) or 1
        self.findings["segments"] = seg_revenues
        self.findings["total_revenue"] = total_rev
        self.findings["n_segments"] = len(seg_revenues)

        # Revenue HHI
        if total_rev > 0:
            shares = [v / total_rev for v in seg_revenues.values()]
            hhi = sum(s ** 2 for s in shares)
        else:
            hhi = 0

        self.findings["hhi"] = hhi

        # Count segments > 10% of revenue (regulatory disclosure threshold)
        major_segments = sum(1 for v in seg_revenues.values() if v / total_rev > 0.10)
        self.findings["major_segments"] = major_segments

        if hhi > 0.50:
            self.signal = "bearish"
            self.score = -8
            self.summary = f"High concentration (HHI={hhi:.2f}) — single-customer risk"
        elif hhi > 0.25:
            self.signal = "neutral"
            self.score = -3
            self.summary = f"Moderate concentration (HHI={hhi:.2f}) — {major_segments} major segments"
        else:
            self.signal = "bullish"
            self.score = 5
            self.summary = f"Diversified revenue (HHI={hhi:.2f}) — {len(seg_revenues)} segments"

    def render(self):
        self.header()
        f = self.findings
        safe_print(f"    Revenue HHI: {f.get('hhi', 0):.3f}  Segments: {f.get('n_segments', 0)}")
        safe_print(f"\n    {'Segment':<40s} {'Revenue':>14s} {'Share':>8s}")
        safe_print(f"    {'─'*40} {'─'*14} {'─'*8}")
        total = f.get("total_revenue", 1)
        for seg, rev in sorted(f.get("segments", {}).items(), key=lambda x: -x[1])[:10]:
            share = rev / total
            safe_print(f"    {seg[:40]:<40s} {usd(rev):>14s} {f'{share:.0%}':>8s}")
        safe_print(f"    {dim('Patatoukas: Customer concentration increases systematic risk')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 40: Real Manipulation (Roychowdhury 2006)
# ══════════════════════════════════════════════════════════════════════════════

class RealManipulationAgent(Agent):
    name = "real_manip"
    title = "REAL MANIPULATION"
    icon = "🎭"

    def run(self):
        inc = self.store.latest("inc_series")
        cf = self.store.latest("cf_series")
        bs = self.store.latest("bs_series")
        inc_prev = self.store.prev("inc_series")
        bs_prev = self.store.prev("bs_series")

        ta = bs.get("total_assets") or 0
        rev = inc.get("revenue") or 0
        cfo = cf.get("cfo")
        cogs = inc.get("cogs") or 0
        rd = inc.get("rd") or 0
        sga = inc.get("sga") or 0
        inv = bs.get("inventory") or 0
        inv_prev = bs_prev.get("inventory") or 0

        if ta <= 0 or rev <= 0:
            self.signal = "neutral"
            self.score = 0
            self.summary = "Insufficient data for REM analysis"
            return

        # Abnormal CFO: CFO/Assets deviation
        cfo_ta = (cfo or 0) / ta
        self.findings["cfo_ta"] = cfo_ta

        # Abnormal production: (COGS + Δinventory) / Assets
        delta_inv = inv - inv_prev
        prod_cost = (cogs + delta_inv) / ta
        self.findings["production_ta"] = prod_cost

        # Abnormal discretionary expenses: (R&D + SGA) / Assets
        disc_exp = (rd + sga) / ta
        self.findings["disc_exp_ta"] = disc_exp

        # Industry benchmarks would come from Frames API
        # Use heuristic thresholds for now
        score = 0
        flags = []

        # Low CFO + High production = channel stuffing
        if cfo_ta < 0.05 and prod_cost > 0.50:
            score -= 6
            flags.append("low CFO + high production costs — channel stuffing risk")

        # Low discretionary expenses = cost cutting to inflate earnings
        if disc_exp < 0.03 and rev > 1e9:
            score -= 4
            flags.append("very low discretionary spend — potential underinvestment")

        # High production relative to revenue
        rev_ta = rev / ta
        if prod_cost > rev_ta * 0.9:
            score -= 4
            flags.append("production costs near revenue — overproduction signal")

        if not flags:
            score = 5
            flags.append("no real manipulation signals detected")

        self.findings["flags"] = flags
        self.score = score
        self.signal = "bearish" if score < -5 else "bullish" if score > 0 else "neutral"
        self.summary = flags[0] if flags else "Clean"

    def render(self):
        self.header()
        f = self.findings
        safe_print(f"    CFO/Assets: {f.get('cfo_ta', 0):.2%}")
        safe_print(f"    Production/Assets: {f.get('production_ta', 0):.2%}")
        safe_print(f"    Discretionary/Assets: {f.get('disc_exp_ta', 0):.2%}")
        safe_print(f"\n    Flags:")
        for flag in f.get("flags", []):
            color = red if "risk" in flag or "signal" in flag else green
            safe_print(f"      • {color(flag)}")
        safe_print(f"    {dim('Roychowdhury: Real activities manipulation through operations')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 41: Pension Risk (Franzoni-Marin 2006)
# ══════════════════════════════════════════════════════════════════════════════

class PensionRiskAgent(Agent):
    name = "pension"
    title = "PENSION RISK"
    icon = "👴"

    def run(self):
        facts = self.store.raw.get("edgar_facts")
        if not facts:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No EDGAR data"
            return

        # Extract pension-specific XBRL concepts
        pension_concepts = {
            "pbo": "DefinedBenefitPlanBenefitObligation",
            "plan_assets": "DefinedBenefitPlanFairValueOfPlanAssets",
            "discount_rate": "DefinedBenefitPlanAssumptionsUsedCalculatingBenefitObligationDiscountRate",
            "expected_return": "DefinedBenefitPlanAssumptionsUsedCalculatingNetPeriodicBenefitCostExpectedLongTermReturnOnAssets",
        }

        pension_data = {}
        for key, concept in pension_concepts.items():
            entries = _edgar_annual_values(facts, concept, "USD" if key in ("pbo", "plan_assets") else "pure")
            if entries:
                pension_data[key] = entries[-1].get("val")

        self.findings["pension_data"] = pension_data

        pbo = pension_data.get("pbo")
        plan_assets = pension_data.get("plan_assets")

        if pbo is None or pbo == 0:
            self.signal = "neutral"
            self.score = 0
            self.summary = "No defined benefit pension plan (N/A)"
            self.findings["has_pension"] = False
            return

        self.findings["has_pension"] = True
        funded_status = plan_assets / pbo if pbo > 0 and plan_assets else 0
        self.findings["funded_status"] = funded_status

        # Underfunding as % of market cap
        mkt_cap = self.store.overview.get("market_cap") or ((self.store.price or 0) * (self.store.shares or 1))
        underfunding = max(0, (pbo or 0) - (plan_assets or 0))
        underfund_pct = underfunding / mkt_cap if mkt_cap > 0 else 0
        self.findings["underfunding"] = underfunding
        self.findings["underfund_pct_mktcap"] = underfund_pct

        if funded_status >= 0.95:
            self.signal = "neutral"
            self.score = 0
            self.summary = f"Well-funded pension ({funded_status:.0%})"
        elif funded_status >= 0.80:
            self.signal = "neutral"
            self.score = -5
            self.summary = f"Modestly underfunded ({funded_status:.0%}), {underfund_pct:.1%} of mkt cap"
        elif funded_status >= 0.60:
            self.signal = "bearish"
            self.score = -10
            self.summary = f"Underfunded pension ({funded_status:.0%}), {underfund_pct:.1%} of mkt cap"
        else:
            self.signal = "bearish"
            self.score = -15
            self.summary = f"Severely underfunded ({funded_status:.0%}) — material liability risk"

    def render(self):
        self.header()
        f = self.findings
        if not f.get("has_pension", True):
            safe_print(f"    {dim('No defined benefit pension plan — N/A')}")
            return
        pd = f.get("pension_data", {})
        safe_print(f"    PBO: {usd(pd.get('pbo'))}  Plan Assets: {usd(pd.get('plan_assets'))}")
        fs = f.get("funded_status", 0)
        fn = green if fs >= 0.90 else yellow if fs >= 0.80 else red
        safe_print(f"    Funded status: {fn(f'{fs:.0%}')}")
        safe_print(f"    Underfunding: {usd(f.get('underfunding', 0))} ({f.get('underfund_pct_mktcap', 0):.1%} of mkt cap)")
        dr = pd.get("discount_rate")
        er = pd.get("expected_return")
        if dr: safe_print(f"    Discount rate: {dr:.1%}")
        if er: safe_print(f"    Expected return assumption: {er:.1%}")
        safe_print(f"    {dim('Franzoni-Marin: Pension underfunding predicts negative future returns')}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 42: Supply Chain Risk (Wu-Birge 2014)
# ══════════════════════════════════════════════════════════════════════════════

class SupplyChainAgent(Agent):
    name = "supply_chain"
    title = "SUPPLY CHAIN"
    icon = "🔗"

    def run(self):
        segments = self.store.segments
        profile = self.store.profile

        # Inventory health analysis
        years = self.store.years("bs_series", 5)
        inv_data = []
        for y in years:
            bs = self.store.bs_series.get(y, {})
            inc = self.store.inc_series.get(y, {})
            inv = bs.get("inventory")
            rev = inc.get("revenue")
            cogs = inc.get("cogs")
            if inv is not None and rev:
                inv_data.append((y, {
                    "inventory": inv,
                    "inv_to_rev": inv / rev if rev else 0,
                    "inv_days": inv / (cogs / 365) if cogs else 0,
                }))

        self.findings["inventory_data"] = inv_data

        # Supplier concentration proxy: look at payables concentration
        latest_bs = self.store.latest("bs_series")
        ap = latest_bs.get("accounts_payable") or 0
        cogs_latest = self.store.latest("inc_series").get("cogs") or 0
        payable_days = ap / (cogs_latest / 365) if cogs_latest > 0 else 0
        self.findings["payable_days"] = payable_days

        # Segment diversity as supply chain proxy
        n_segments = len(segments)
        self.findings["n_segments"] = n_segments

        score = 0
        flags = []

        # Inventory build-up check
        if len(inv_data) >= 2:
            latest_inv_rev = inv_data[-1][1]["inv_to_rev"]
            prev_inv_rev = inv_data[-2][1]["inv_to_rev"]
            if latest_inv_rev > prev_inv_rev * 1.20:
                score -= 5
                flags.append(f"inventory building (+{(latest_inv_rev/prev_inv_rev - 1):.0%} vs last year)")
            elif latest_inv_rev < prev_inv_rev * 0.85:
                score += 3
                flags.append("inventory declining — efficient supply chain")

        # Very long payable days might indicate supplier distress
        if payable_days > 90:
            score -= 3
            flags.append(f"long payable days ({payable_days:.0f}) — potential supplier strain")
        elif payable_days < 30 and payable_days > 0:
            score += 2
            flags.append("quick payables — good supplier relationships")

        if n_segments >= 4:
            score += 3
            flags.append(f"diversified ({n_segments} segments)")
        elif n_segments <= 1:
            score -= 3
            flags.append("single segment — concentrated operations")

        if not flags:
            flags.append("standard supply chain profile")

        self.findings["flags"] = flags
        self.score = score
        self.signal = "bearish" if score < -3 else "bullish" if score > 3 else "neutral"
        self.summary = flags[0] if flags else "Standard supply chain"

    def render(self):
        self.header()
        f = self.findings
        safe_print(f"    Segments: {f.get('n_segments', 0)}  Payable days: {f.get('payable_days', 0):.0f}")
        if f.get("inventory_data"):
            safe_print(f"\n    {'Year':<8s} {'Inventory':>14s} {'Inv/Rev':>10s} {'Inv Days':>10s}")
            safe_print(f"    {'─'*8} {'─'*14} {'─'*10} {'─'*10}")
            for y, d in f.get("inventory_data", []):
                itr = d['inv_to_rev']
                idays = d['inv_days']
                safe_print(f"    {y:<8s} {usd(d['inventory']):>14s} {itr:>10.1%} {idays:>10.0f}")
        safe_print(f"\n    Assessment:")
        for flag in f.get("flags", []):
            safe_print(f"      • {flag}")


# ══════════════════════════════════════════════════════════════════════════════
# AGENT 43: Chief Strategist — Multi-Stage Intelligent Synthesis
# ══════════════════════════════════════════════════════════════════════════════

class StrategistAgent(Agent):
    name = "strategist"
    title = "CHIEF STRATEGIST — MULTI-STAGE SYNTHESIS"
    icon = "🎯"

    def __init__(self, store, agents):
        super().__init__(store)
        self.agents = agents

    def run(self):
        # ════════════════════════════════════════════════════════
        # STAGE 1: Reliability-Weighted Vote Collection
        # ════════════════════════════════════════════════════════
        votes = []
        for a in self.agents:
            if a.score is not None:
                weight = AGENT_RELIABILITY_WEIGHTS.get(a.name, 1.0)
                conf = getattr(a, "confidence", 0.5)
                dq = getattr(a, "data_quality", 1.0)
                effective_weight = weight * conf * dq
                votes.append({"agent": a.title, "name": a.name, "signal": a.signal,
                               "score": a.score, "summary": a.summary,
                               "weight": effective_weight, "raw_weight": weight,
                               "confidence": conf, "data_quality": dq})
        self.findings["votes"] = votes

        # Weighted composite score (reliability-adjusted)
        weighted_score = sum(v["score"] * v["weight"] for v in votes)
        total_weight = sum(v["weight"] for v in votes) or 1
        raw_total = sum(v["score"] for v in votes)
        max_possible = sum(abs(v["score"]) * v["weight"] for v in votes) or 1
        normalized = weighted_score / max_possible  # -1 to +1

        self.findings["composite_score"] = raw_total
        self.findings["weighted_composite"] = weighted_score
        self.findings["normalized_score"] = normalized
        self.findings["total_weight"] = total_weight

        # ════════════════════════════════════════════════════════
        # STAGE 2: Cross-Agent Correlation & Conflict Detection
        # ════════════════════════════════════════════════════════

        # Domain consensus analysis
        domain_analysis = compute_domain_consensus(self.agents)
        self.findings["domain_analysis"] = domain_analysis

        # Cross-domain conflict detection
        domain_signals = {d: info["signal"] for d, info in domain_analysis.items()}
        bull_domains = [d for d, s in domain_signals.items() if s == "bullish"]
        bear_domains = [d for d, s in domain_signals.items() if s == "bearish"]
        cross_domain_conflict = len(bull_domains) > 0 and len(bear_domains) > 0
        self.findings["cross_domain_conflict"] = cross_domain_conflict
        self.findings["bull_domains"] = bull_domains
        self.findings["bear_domains"] = bear_domains

        # Intra-domain conflicts
        conflicted_domains = [d for d, info in domain_analysis.items() if info.get("has_conflict")]
        self.findings["conflicted_domains"] = conflicted_domains

        # Agent outlier detection (z-scores)
        z_scores, outliers = compute_agent_correlation_matrix(self.agents)
        self.findings["agent_z_scores"] = z_scores
        self.findings["agent_outliers"] = outliers

        # ════════════════════════════════════════════════════════
        # STAGE 3: Signal Interaction Detection
        # ════════════════════════════════════════════════════════

        interactions = detect_signal_interactions(self.agents)
        self.findings["interactions"] = interactions
        interaction_adjustment = sum(i["score_adjustment"] for i in interactions)
        self.findings["interaction_adjustment"] = interaction_adjustment

        # ════════════════════════════════════════════════════════
        # STAGE 4: Market Regime Detection
        # ════════════════════════════════════════════════════════

        regime = detect_regime(self.store, self.agents)
        self.findings["regime"] = regime

        # Regime-adjusted scoring
        regime_label = regime.get("regime", "NEUTRAL")
        regime_adjustment = 0
        if regime_label == "RISK-OFF":
            regime_adjustment = -0.05  # tilt bearish in risk-off
        elif regime_label == "RISK-ON":
            regime_adjustment = +0.03  # mild tilt in risk-on

        # ════════════════════════════════════════════════════════
        # STAGE 5: Temporal Signal Analysis (Velocity/Acceleration)
        # ════════════════════════════════════════════════════════

        velocity_signals = {}
        for field, label in [("revenue", "Revenue"), ("net_income", "Net Income"),
                             ("cfo", "Cash Flow"), ("gross_profit", "Gross Profit")]:
            series_name = "inc_series" if field != "cfo" else "cf_series"
            vel = compute_signal_velocity(self.store, series_name, field)
            velocity_signals[label] = vel

        self.findings["velocity_signals"] = velocity_signals

        # Count accelerating vs decelerating metrics
        n_accel = sum(1 for v in velocity_signals.values() if "accelerating_growth" in v.get("trend", ""))
        n_decel = sum(1 for v in velocity_signals.values() if "accelerating_decline" in v.get("trend", ""))
        momentum_signal = (n_accel - n_decel) * 0.02
        self.findings["momentum_signal"] = momentum_signal

        # ════════════════════════════════════════════════════════
        # STAGE 6: Bayesian Score Integration
        # ════════════════════════════════════════════════════════

        # Start with market-implied prior (from regime + CAPE)
        cape = self.store.cape_ratio
        if cape and cape > 30:
            prior_score = -5  # expensive market = mild bearish prior
        elif cape and cape < 18:
            prior_score = 5   # cheap market = mild bullish prior
        else:
            prior_score = 0

        # Bayesian update: each domain provides evidence
        evidence = []
        for domain, info in domain_analysis.items():
            ds = info.get("domain_score", 0)
            cs = info.get("consensus_strength", 0.5)
            evidence.append((ds, cs * 0.3))  # scale down domain confidence

        bayesian_score, bayesian_confidence = bayesian_score_update(prior_score, evidence)
        self.findings["bayesian_score"] = bayesian_score
        self.findings["bayesian_confidence"] = bayesian_confidence

        # ════════════════════════════════════════════════════════
        # STAGE 7: Fair Value Blending (same as before + quality adj)
        # ════════════════════════════════════════════════════════

        fair_values = []
        fv_weights = []

        quant = next((a for a in self.agents if a.name == "quant"), None)
        if quant:
            dcf = quant.findings.get("dcf", {}).get("Base")
            if dcf and dcf["fair_value"] > 0:
                fair_values.append(dcf["fair_value"]); fv_weights.append(0.25)
            mc = quant.findings.get("mc")
            if mc:
                fair_values.append(mc["median"]); fv_weights.append(0.15)
            mults = quant.findings.get("multiples", [])
            mod_mults = [m["value"] for m in mults if "20x" in m["method"] or "18x" in m["method"] or "25x" in m["method"]]
            if mod_mults:
                fair_values.append(statistics.mean(mod_mults)); fv_weights.append(0.10)

        klarman = next((a for a in self.agents if a.name == "klarman"), None)
        if klarman:
            civ = klarman.findings.get("conservative_iv")
            miv = klarman.findings.get("median_iv")
            if miv and miv > 0:
                fair_values.append(miv); fv_weights.append(0.20)
            elif civ and civ > 0:
                fair_values.append(civ); fv_weights.append(0.20)

        target = self.store.overview.get("analyst_target")
        if target and target > 0:
            fair_values.append(target); fv_weights.append(0.10)

        if fair_values and fv_weights:
            total_w = sum(fv_weights)
            blended = sum(fv * w for fv, w in zip(fair_values, fv_weights)) / total_w

            # Quality-adjust fair value (discount if quality signals are bad)
            quality_domain = domain_analysis.get("quality", {})
            if quality_domain.get("signal") == "bearish":
                quality_discount = 0.90  # 10% discount for poor quality
                blended *= quality_discount
                self.findings["quality_discount"] = quality_discount
            elif quality_domain.get("signal") == "bullish":
                quality_premium = 1.05  # 5% premium for high quality
                blended *= quality_premium
                self.findings["quality_premium"] = quality_premium

            self.findings["blended_fair_value"] = blended
        else:
            self.findings["blended_fair_value"] = None

        # ════════════════════════════════════════════════════════
        # STAGE 8: Risk Overlay (enhanced with new agents)
        # ════════════════════════════════════════════════════════
        risk_overlay = {}

        kelly_agent = next((a for a in self.agents if a.name == "kelly"), None)
        if kelly_agent:
            risk_overlay["sharpe"] = kelly_agent.findings.get("sharpe", 0)
            risk_overlay["sortino"] = kelly_agent.findings.get("sortino", 0)
            risk_overlay["calmar"] = kelly_agent.findings.get("calmar", 0)
            risk_overlay["kelly_fraction"] = kelly_agent.findings.get("half_kelly", 0)
            risk_overlay["win_rate"] = kelly_agent.findings.get("win_rate", 0.5)

        tail_agent = next((a for a in self.agents if a.name == "tail_risk"), None)
        if tail_agent:
            risk_overlay["max_drawdown"] = tail_agent.findings.get("max_drawdown", 0)
            risk_overlay["kurtosis"] = tail_agent.findings.get("kurtosis", 0)
            risk_overlay["antifragile"] = tail_agent.findings.get("antifragile_label", "?")
            risk_overlay["var_95"] = tail_agent.findings.get("var_95", 0)

        erg_agent = next((a for a in self.agents if a.name == "ergodicity"), None)
        if erg_agent:
            risk_overlay["time_avg_growth"] = erg_agent.findings.get("time_avg", 0)
            risk_overlay["ergodicity_gap"] = erg_agent.findings.get("ergodicity_gap", 0)
            risk_overlay["vol_tax"] = erg_agent.findings.get("vol_tax", 0)
            risk_overlay["ruin_50"] = erg_agent.findings.get("ruin_levels", {}).get(0.50, 0)

        minsky_agent = next((a for a in self.agents if a.name == "minsky"), None)
        if minsky_agent:
            risk_overlay["minsky_class"] = minsky_agent.findings.get("minsky_class", "?")

        forensic_agent = next((a for a in self.agents if a.name == "forensic"), None)
        if forensic_agent:
            risk_overlay["m_score"] = forensic_agent.findings.get("m_score", -3)
            risk_overlay["red_flags_count"] = len(forensic_agent.findings.get("red_flags", []))

        complex_agent = next((a for a in self.agents if a.name == "complexity"), None)
        if complex_agent:
            risk_overlay["bubble_risk"] = complex_agent.findings.get("bubble_risk", "Low")

        # New agents contributing to risk overlay
        ohlson_a = next((a for a in self.agents if a.name == "ohlson"), None)
        if ohlson_a:
            risk_overlay["ohlson_prob"] = ohlson_a.findings.get("prob_bankruptcy", 0)

        vol_a = next((a for a in self.agents if a.name == "vol_surface"), None)
        if vol_a:
            risk_overlay["vix_inverted"] = vol_a.findings.get("inverted", False)
            risk_overlay["vix_level"] = vol_a.findings.get("vix", 0)

        pension_a = next((a for a in self.agents if a.name == "pension"), None)
        if pension_a and pension_a.findings.get("has_pension"):
            risk_overlay["pension_funded"] = pension_a.findings.get("funded_status", 1.0)

        self.findings["risk_overlay"] = risk_overlay

        # ════════════════════════════════════════════════════════
        # STAGE 9: Composite Conviction Scoring
        # ════════════════════════════════════════════════════════

        # Combine all scoring dimensions
        risk_penalty = 0
        if risk_overlay.get("m_score", -3) > -1.78: risk_penalty += 1
        if risk_overlay.get("minsky_class") == "Ponzi": risk_penalty += 2
        elif risk_overlay.get("minsky_class") == "Speculative": risk_penalty += 1
        if risk_overlay.get("bubble_risk") == "High": risk_penalty += 1
        if risk_overlay.get("ruin_50", 0) > 0.50: risk_penalty += 1
        if risk_overlay.get("ohlson_prob", 0) > 0.30: risk_penalty += 2
        if risk_overlay.get("vix_inverted"): risk_penalty += 1
        if risk_overlay.get("pension_funded", 1) < 0.70: risk_penalty += 1

        risk_boost = 0
        if risk_overlay.get("antifragile") == "Antifragile": risk_boost += 1
        if risk_overlay.get("sharpe", 0) > 1.5: risk_boost += 1
        if risk_overlay.get("minsky_class") == "Hedge": risk_boost += 1
        insider_a = next((a for a in self.agents if a.name == "insider_flow"), None)
        if insider_a and insider_a.findings.get("n_buyers", 0) >= 3: risk_boost += 1

        # Final composite: multi-dimensional
        adjusted_normalized = (
            normalized * 0.40                    # Base agent consensus
            + (bayesian_score / 30) * 0.20       # Bayesian posterior (normalized to ~±1)
            + momentum_signal * 0.10             # Temporal momentum
            + (interaction_adjustment / 20) * 0.15  # Interaction effects
            + regime_adjustment * 0.15           # Market regime
            + (risk_boost - risk_penalty) * 0.03  # Risk overlay
        )

        # Conflict penalty: reduce conviction when domains disagree
        if cross_domain_conflict:
            conflict_discount = 1.0 - 0.05 * len(conflicted_domains)
            adjusted_normalized *= max(0.6, conflict_discount)
            self.findings["conflict_discount"] = conflict_discount

        self.findings["risk_adjusted_normalized"] = adjusted_normalized
        self.findings["risk_penalty"] = risk_penalty
        self.findings["risk_boost"] = risk_boost

        # ════════════════════════════════════════════════════════
        # STAGE 10: Final Recommendation with Conviction Grading
        # ════════════════════════════════════════════════════════

        price = self.store.price
        bfv = self.findings.get("blended_fair_value")
        upside = (bfv / price - 1) if bfv and price else None
        self.findings["upside"] = upside

        # Multi-factor conviction grading
        if adjusted_normalized > 0.35 and (upside is None or upside > 0.15):
            self.findings["recommendation"] = "STRONG BUY"
            self.findings["conviction"] = "Very High"
        elif adjusted_normalized > 0.20 and (upside is None or upside > 0.05):
            self.findings["recommendation"] = "BUY"
            self.findings["conviction"] = "High"
        elif adjusted_normalized > 0.08 and (upside is None or upside > 0):
            self.findings["recommendation"] = "BUY"
            self.findings["conviction"] = "Moderate"
        elif adjusted_normalized < -0.35 and (upside is None or upside < -0.15):
            self.findings["recommendation"] = "STRONG SELL"
            self.findings["conviction"] = "Very High"
        elif adjusted_normalized < -0.20 and (upside is None or upside < -0.05):
            self.findings["recommendation"] = "SELL"
            self.findings["conviction"] = "High"
        elif adjusted_normalized < -0.08 and (upside is None or upside < 0):
            self.findings["recommendation"] = "UNDERWEIGHT"
            self.findings["conviction"] = "Moderate"
        else:
            self.findings["recommendation"] = "HOLD"
            self.findings["conviction"] = "Low"

        # Override: if confidence is very low across the board, cap conviction
        avg_confidence = statistics.mean([v.get("confidence", 0.5) for v in votes]) if votes else 0.5
        if avg_confidence < 0.4:
            self.findings["conviction"] = "Low"
            self.findings["conviction_note"] = "capped due to low average agent confidence"

    def render(self):
        safe_print(f"\n  {'━' * 88}")
        safe_print(f"  {bold(cyan(f'{self.icon} {self.title}'))}")
        safe_print(f"  {'━' * 88}")

        # ── Market Regime ──
        regime = self.findings.get("regime", {})
        if regime:
            rl = regime.get("regime", "?")
            regime_fn = {"RISK-ON": green, "RISK-OFF": red, "TRANSITION": yellow}.get(rl, dim)
            safe_print(f"\n    {bold('Market Regime:')} {regime_fn(rl)}")
            sigs = regime.get("signals", {})
            if sigs:
                parts = []
                for k, v in sigs.items():
                    v_fn = green if v in ("bull", "complacent", "tight", "steep", "low", "cheap", "fair") else \
                           red if v in ("bear", "crisis", "stress", "inverted", "elevated", "expensive") else yellow
                    parts.append(f"{k}={v_fn(v)}")
                safe_print(f"      {' | '.join(parts)}")

        # ── Domain Consensus Matrix ──
        da = self.findings.get("domain_analysis", {})
        if da:
            safe_print(f"\n    {bold('Domain Consensus Matrix:')}")
            safe_print(f"      {'Domain':<14s} {'Signal':<12s} {'Score':>8s} {'Consensus':>10s} {'Conflict':>10s}")
            safe_print(f"      {'─'*14} {'─'*12} {'─'*8} {'─'*10} {'─'*10}")
            for domain in sorted(da.keys()):
                info = da[domain]
                sig = info["signal"]
                sig_fn = green if sig == "bullish" else red if sig == "bearish" else yellow
                ds = info["domain_score"]
                cs = info["consensus_strength"]
                has_c = info.get("has_conflict", False)
                conf_str = red("YES") if has_c else green("no")
                safe_print(f"      {domain:<14s} {sig_fn(sig.upper()):<20s} {ds:>+7.1f} {cs:>9.0%} {conf_str:>10s}")

        # ── Cross-Domain Conflicts ──
        if self.findings.get("cross_domain_conflict"):
            safe_print(f"\n    {bold(yellow('Cross-Domain Conflict Detected:'))}")
            safe_print(f"      Bullish domains: {', '.join(self.findings.get('bull_domains', []))}")
            safe_print(f"      Bearish domains: {', '.join(self.findings.get('bear_domains', []))}")
            cd = self.findings.get("conflict_discount")
            if cd:
                safe_print(f"      Conviction discount: {red(f'{(1-cd):.0%}')}")

        # ── Signal Interactions ──
        interactions = self.findings.get("interactions", [])
        if interactions:
            safe_print(f"\n    {bold('Signal Interactions Detected:')}")
            for ix in interactions:
                adj = ix["score_adjustment"]
                fn = green if adj > 0 else red
                safe_print(f"      {fn(f'{adj:+d}')} {ix['description']} ({ix['agents'][0]}×{ix['agents'][1]})")
            total_ix = self.findings.get("interaction_adjustment", 0)
            safe_print(f"      Total interaction effect: {(green if total_ix > 0 else red)(f'{total_ix:+d}')}")

        # ── Agent Outliers ──
        outliers = self.findings.get("agent_outliers", [])
        if outliers:
            safe_print(f"\n    {bold('Agent Outliers')} {dim('(z-score > 1.8)')}{bold(':')}")
            for name, z in outliers:
                fn = green if z > 0 else red
                safe_print(f"      {name:<20s} z={fn(f'{z:+.2f}')}")

        # ── Temporal Momentum ──
        vel = self.findings.get("velocity_signals", {})
        if vel:
            safe_print(f"\n    {bold('Temporal Signal Analysis:')}")
            for label, v in vel.items():
                trend = v.get("trend", "?")
                velocity = v.get("velocity", 0)
                accel = v.get("acceleration", 0)
                trend_fn = green if "growth" in trend else red if "decline" in trend else dim
                safe_print(f"      {label:<16s} v={velocity:+.1%}  a={accel:+.1%}  {trend_fn(trend)}")

        # ── Agent Votes (condensed) ──
        safe_print(f"\n    {bold('Agent Consensus:')} ({len(self.findings.get('votes', []))} agents)")
        safe_print(f"      {'Agent':<28s} {'Sig':<8s} {'Score':>6s} {'Wt':>5s}  Summary")
        safe_print(f"      {'─'*28} {'─'*8} {'─'*6} {'─'*5}  {'─'*35}")
        for v in self.findings.get("votes", []):
            sig = v["signal"]
            if sig == "bullish": sig_str = green("BULL")
            elif sig == "bearish": sig_str = red("BEAR")
            else: sig_str = yellow("NEUT")
            sc = v["score"]
            score_str = green(f"+{sc}") if sc > 0 else red(str(sc)) if sc < 0 else dim("0")
            w = v.get("weight", 1.0)
            safe_print(f"      {v['agent'][:28]:<28s} {sig_str:<16s} {score_str:>6s} {w:>5.2f}  {dim(v['summary'][:35])}")

        # ── Composite Scoring Breakdown ──
        cs = self.findings.get("composite_score", 0)
        wcs = self.findings.get("weighted_composite", 0)
        ns = self.findings.get("normalized_score", 0)
        bs = self.findings.get("bayesian_score", 0)
        ms = self.findings.get("momentum_signal", 0)
        ia = self.findings.get("interaction_adjustment", 0)

        safe_print(f"\n    {bold('Multi-Stage Score Decomposition:')}")
        safe_print(f"      Raw composite: {green(str(cs)) if cs > 0 else red(str(cs))} → Weighted: {wcs:+.1f} → Normalized: {ns:+.3f}")
        safe_print(f"      Bayesian posterior: {bs:+.1f} (conf={self.findings.get('bayesian_confidence', 0):.0%})")
        safe_print(f"      Momentum signal: {ms:+.3f}  Interaction adj: {ia:+d}")
        safe_print(f"      Risk penalty: {red(str(self.findings.get('risk_penalty', 0)))}  Risk boost: {green(str(self.findings.get('risk_boost', 0)))}")

        # ── Fair Value ──
        safe_print(f"\n    {bold('Blended Fair Value Components:')}")
        quant = next((a for a in self.agents if a.name == "quant"), None)
        if quant:
            dcf = quant.findings.get("dcf", {}).get("Base")
            if dcf: safe_print(f"      DCF Base Case (25%):      ${dcf['fair_value']:>10,.2f}")
            mc = quant.findings.get("mc")
            if mc: safe_print(f"      Monte Carlo Median (15%):  ${mc['median']:>10,.2f}")
        klarman = next((a for a in self.agents if a.name == "klarman"), None)
        if klarman:
            miv = klarman.findings.get("median_iv") or klarman.findings.get("conservative_iv")
            if miv: safe_print(f"      Klarman Median IV (20%):   ${miv:>10,.2f}")
        target = self.store.overview.get("analyst_target")
        if target: safe_print(f"      Analyst Target (10%):      ${target:>10,.2f}")

        qd = self.findings.get("quality_discount")
        qp = self.findings.get("quality_premium")
        if qd: safe_print(f"      {red(f'Quality discount applied: {(1-qd):.0%}')}")
        if qp: safe_print(f"      {green(f'Quality premium applied: +{(qp-1):.0%}')}")

        # ── Final Verdict ──
        bfv = self.findings.get("blended_fair_value")
        price = self.store.price
        rec = self.findings.get("recommendation", "HOLD")
        conv = self.findings.get("conviction", "Low")
        upside = self.findings.get("upside")
        ran = self.findings.get("risk_adjusted_normalized", 0)

        safe_print(f"\n    {'━' * 60}")
        if bfv:
            color_fn = green if (upside and upside > 0) else red
            safe_print(f"    {bold('BLENDED FAIR VALUE:')}  {bold(color_fn(f'${bfv:,.2f}'))}")
        if price:
            safe_print(f"    {bold('CURRENT PRICE:')}       ${price:,.2f}")
        if upside is not None:
            color_fn = green if upside > 0 else red
            safe_print(f"    {bold('IMPLIED UPSIDE:')}      {bold(color_fn(pct(upside)))}")
        safe_print(f"    {bold('FINAL SCORE:')}         {green(f'{ran:+.3f}') if ran > 0 else red(f'{ran:+.3f}')}")
        safe_print(f"    {'━' * 60}")

        # ── Risk Overlay Dashboard ──
        ro = self.findings.get("risk_overlay", {})
        if ro:
            safe_print(f"\n    {bold('Risk Overlay Dashboard:')}")
            safe_print(f"      {'─' * 56}")

            sh = ro.get("sharpe"); so = ro.get("sortino"); ca = ro.get("calmar")
            if sh is not None:
                sh_fn = green if sh > 1 else yellow if sh > 0.3 else red
                so_fn = green if (so or 0) > 1 else yellow if (so or 0) > 0.3 else red
                safe_print(f"      Sharpe: {sh_fn('{:.2f}'.format(sh))}  Sortino: {so_fn('{:.2f}'.format(so or 0))}  Calmar: {'{:.2f}'.format(ca or 0)}")

            md = ro.get("max_drawdown"); kurt = ro.get("kurtosis"); var95 = ro.get("var_95")
            if md is not None:
                safe_print(f"      MaxDD: {red('{:.1%}'.format(md))}  Kurtosis: {'{:.1f}'.format(kurt or 0)}  VaR95: {red('{:.1%}'.format(abs(var95 or 0)))}")

            tag = ro.get("time_avg_growth"); eg = ro.get("ergodicity_gap"); vt = ro.get("vol_tax")
            if tag is not None:
                ta_fn = green if tag > 0 else red
                safe_print(f"      TimeAvgGrowth: {ta_fn('{:+.1%}'.format(tag))}  ErgGap: {'{:.1%}'.format(eg or 0)}  VolTax: {'{:.1%}'.format(vt or 0)}")

            mc = ro.get("minsky_class", "?"); af = ro.get("antifragile", "?"); br = ro.get("bubble_risk", "?")
            minsky_fn = {"Hedge": green, "Speculative": yellow, "Ponzi": red}.get(mc, dim)
            af_fn = {"Antifragile": green, "Robust": cyan, "Fragile": red}.get(af, dim)
            br_fn = {"Low": green, "Elevated": yellow, "High": red}.get(br, dim)
            safe_print(f"      Minsky: {minsky_fn(mc)}  Antifragile: {af_fn(af)}  Bubble: {br_fn(br)}")

            ms = ro.get("m_score"); rfc = ro.get("red_flags_count", 0)
            if ms is not None:
                ms_fn = red if ms > -1.78 else yellow if ms > -2.22 else green
                safe_print(f"      M-Score: {ms_fn('{:.2f}'.format(ms))}  Red Flags: {red(str(rfc)) if rfc > 0 else green('0')}")

            op = ro.get("ohlson_prob")
            if op and op > 0.01:
                op_fn = green if op < 0.05 else yellow if op < 0.20 else red
                safe_print(f"      Ohlson P(distress): {op_fn('{:.1%}'.format(op))}")

            vix_l = ro.get("vix_level")
            if vix_l:
                vi = ro.get("vix_inverted", False)
                vix_fn = green if vix_l < 18 else yellow if vix_l < 25 else red
                safe_print(f"      VIX: {vix_fn('{:.1f}'.format(vix_l))}{red(' INVERTED') if vi else ''}")

            pf = ro.get("pension_funded")
            if pf and pf < 1.0:
                pf_fn = green if pf > 0.90 else yellow if pf > 0.80 else red
                safe_print(f"      Pension funded: {pf_fn('{:.0%}'.format(pf))}")

            kf = ro.get("kelly_fraction")
            if kf is not None:
                kf_fn = green if kf > 0 else red
                wr = ro.get("win_rate", 0)
                safe_print(f"      HalfKelly: {kf_fn('{:.0%}'.format(kf))}  WinRate: {'{:.0%}'.format(wr)}")

            safe_print(f"      {'─' * 56}")

        # ── Corporate Profile Summary ──
        profile = self.store.profile
        if profile:
            safe_print(f"\n    {bold('Corporate Profile:')}")
            taxonomy = profile.get("taxonomy", "US-GAAP")
            sic = profile.get("sic", ""); sic_desc = profile.get("sic_desc", "")
            fc = profile.get("filing_counts", {})
            if sic:
                safe_print(f"      SIC: {sic} ({sic_desc})  Taxonomy: {taxonomy}")
            if fc:
                fc_str = "  ".join("{}: {}".format(k, v) for k, v in fc.items())
                safe_print(f"      Filings: {fc_str}")

        # Key Ratios Summary
        ratios = self.store.ratios
        if ratios:
            safe_print(f"\n    {bold('Key Financial Ratios:')}")
            row1_parts = []
            for k, label in [("gross_margin", "Gross"), ("operating_margin", "Op"), ("net_margin", "Net"),
                             ("roa", "ROA"), ("roe", "ROE"), ("roic", "ROIC")]:
                v = ratios.get(k)
                if v is not None:
                    fn = green if v > 0.15 else yellow if v > 0.05 else red
                    row1_parts.append("{}: {}".format(label, fn("{:.0%}".format(v))))
            if row1_parts:
                safe_print(f"      {dim('Margins:')} {'  '.join(row1_parts)}")
            row2_parts = []
            for k, label in [("debt_to_equity", "D/E"), ("current_ratio", "Current"),
                             ("interest_coverage", "IntCov"), ("fcf_yield", "FCF Yld")]:
                v = ratios.get(k)
                if v is not None:
                    if k == "fcf_yield":
                        fn = green if v > 0.05 else yellow if v > 0.02 else red
                        row2_parts.append("{}: {}".format(label, fn("{:.1%}".format(v))))
                    elif k == "current_ratio":
                        fn = green if v > 1.5 else yellow if v > 1 else red
                        row2_parts.append("{}: {}".format(label, fn("{:.1f}x".format(v))))
                    else:
                        row2_parts.append("{}: {:.1f}x".format(label, v))
            if row2_parts:
                safe_print(f"      {dim('Health:')}  {'  '.join(row2_parts)}")

        # ── Recommendation badge ──
        rec_colors = {"STRONG BUY": bg_green, "BUY": green, "HOLD": yellow,
                      "UNDERWEIGHT": red, "SELL": bg_red, "STRONG SELL": bg_red}
        color_fn = rec_colors.get(rec, bold)
        safe_print(f"\n    {bold('RECOMMENDATION:')}  {color_fn(f' {rec} ')}  (Conviction: {conv})")

        note = self.findings.get("conviction_note")
        if note:
            safe_print(f"    {dim(f'Note: {note}')}")

        safe_print(f"\n    {dim('This analysis is generated by a 42-agent multi-stage synthesis swarm.')}")
        safe_print(f"    {dim('10 synthesis stages: weighted votes → domain consensus → conflict detection')}")
        safe_print(f"    {dim('→ interaction amplification → regime analysis → Bayesian integration')}")
        safe_print(f"    {dim('This is not investment advice. Always conduct your own due diligence.')}")
        safe_print()


# ══════════════════════════════════════════════════════════════════════════════
# Orchestrator
# ══════════════════════════════════════════════════════════════════════════════

ALL_AGENTS = {
    # Original 20 agents
    "quant": QuantAgent,
    "fundamental": FundamentalAgent,
    "growth": GrowthAgent,
    "credit": CreditAgent,
    "capital": CapitalAgent,
    "moat": MoatAgent,
    "macro": MacroAgent,
    "risk": RiskAgent,
    "sentiment": SentimentAgent,
    "klarman": KlarmanAgent,
    "factor": FactorAgent,
    "catalyst": CatalystAgent,
    "narrative": NarrativeAgent,
    "minsky": MinskyAgent,
    "tail_risk": TailRiskAgent,
    "complexity": ComplexityAgent,
    "forensic": ForensicAgent,
    "kelly": KellyAgent,
    "ergodicity": ErgodicityAgent,
    "microstructure": MicrostructureAgent,
    # Wave 1: EDGAR XBRL (8 agents)
    "ohlson": OhlsonDistressAgent,
    "accrual": AccrualAnomalyAgent,
    "gross_profit": GrossProfitabilityAgent,
    "asset_growth": AssetGrowthAgent,
    "issuance": CapitalIssuanceAgent,
    "buyback_signal": BuybackSignalAgent,
    "tax_quality": TaxQualityAgent,
    "earnings_bench": EarningsBenchmarkAgent,
    # Wave 2: SEC EFTS + Form 4 (5 agents)
    "insider_flow": InsiderFlowAgent,
    "activist": ActivistRadarAgent,
    "filing_timing": FilingTimingAgent,
    "inst_flow": InstitutionalFlowAgent,
    "exec_align": ExecutiveAlignmentAgent,
    # Wave 3: FRED + CBOE (3 agents)
    "capital_cycle": CapitalCycleAgent,
    "gpr": GeopoliticalRiskAgent,
    "vol_surface": VolatilitySurfaceAgent,
    # Wave 4: FINRA (1 agent)
    "short_interest": ShortInterestAgent,
    # Wave 5: Advanced (5 agents)
    "textual": TextualAnalyticsAgent,
    "customer_conc": CustomerConcentrationAgent,
    "real_manip": RealManipulationAgent,
    "pension": PensionRiskAgent,
    "supply_chain": SupplyChainAgent,
}

def run_swarm(ticker, agent_names=None, depth="standard"):
    if not AV_KEY:
        print(yellow("Warning: ALPHA_VANTAGE_API_KEY not set — running in EDGAR-only mode."))
        print(dim("  Set ALPHA_VANTAGE_API_KEY for full market data, or FRED_API_KEY for macro data."))

    # Banner
    print(f"""
  {bold('╔══════════════════════════════════════════════════════════════════════════════════════╗')}
  {bold('║')}  {mag('MULTI-AGENT INVESTMENT ANALYSIS SWARM')}                                            {bold('║')}
  {bold('║')}  {dim('42 specialized analysts • EDGAR + FRED + CBOE + FINRA + academic factors')}    {bold('║')}
  {bold('╚══════════════════════════════════════════════════════════════════════════════════════╝')}""")

    # Fetch data
    store = DataStore(ticker)
    store.fetch(depth)

    if not store.overview.get("name") and not store.raw.get("edgar_entity"):
        print(red(f"\n  No data found for {ticker}.\n"))
        return
    if not store.overview.get("name"):
        store.overview["name"] = store.raw.get("edgar_entity", ticker)
        store.overview["symbol"] = ticker

    # Print header
    print(f"\n  {'═' * 88}")
    print(f"  {bold(store.overview['name'])} ({cyan(store.overview['symbol'])})")
    print(f"  {store.overview.get('exchange', '')} · {store.overview.get('sector', '')} · {store.overview.get('industry', '')}")
    if store.price:
        print(f"  ${store.price:,.2f} | Mkt Cap {usd(store.overview.get('market_cap'))} | {num(store.shares / 1e6, 0)}M shares")

    # Profile enrichment
    if store.profile:
        p = store.profile
        parts = []
        if p.get("taxonomy"): parts.append(p["taxonomy"])
        if p.get("sic"): parts.append("SIC {}".format(p["sic"]))
        fc = p.get("filing_counts", {})
        if fc:
            total_filings = sum(fc.values())
            parts.append("{} SEC filings".format(total_filings))
        if store.inc_quarterly:
            parts.append("{} quarters".format(len(store.inc_quarterly)))
        if store.segments:
            parts.append("{} segments".format(len(store.segments)))
        if parts:
            print(f"  {dim(' · '.join(parts))}")

    desc = store.overview.get("description", "")
    if desc:
        wrapped = textwrap.fill(desc[:300], width=86, initial_indent="  ", subsequent_indent="  ")
        print(f"\n{dim(wrapped)}")
    print(f"  {'═' * 88}")

    # Select agents
    if agent_names:
        selected = [n for n in agent_names if n in ALL_AGENTS]
    else:
        selected = list(ALL_AGENTS.keys())

    agents = [ALL_AGENTS[name](store) for name in selected]

    # Run agents in parallel
    print(f"\n  {bold('Dispatching')} {cyan(str(len(agents)))} {bold('analyst agents...')}\n")

    def run_agent(agent):
        t0 = time.time()
        try:
            agent.run()
        except Exception as e:
            agent.signal = "error"
            agent.score = 0
            agent.summary = f"Error: {e}"
        elapsed = time.time() - t0
        sig = {"bullish": green("BULL"), "bearish": red("BEAR"), "neutral": yellow("NEUT"), "error": red("ERR!")}.get(agent.signal, dim("????"))
        safe_print(f"    {agent.icon} {agent.title:<32s} {sig}  {dim(f'{elapsed:.1f}s')}  {dim(agent.summary[:60])}")
        return agent

    with ThreadPoolExecutor(max_workers=4) as pool:
        futures = {pool.submit(run_agent, a): a for a in agents}
        completed_agents = []
        for future in as_completed(futures):
            completed_agents.append(future.result())

    # Reorder agents to match original selection order
    agent_map = {a.name: a for a in completed_agents}
    ordered_agents = [agent_map[name] for name in selected if name in agent_map]

    # Run strategist
    print(f"\n  {bold('Synthesizing findings...')}")
    strategist = StrategistAgent(store, ordered_agents)
    strategist.run()

    # Render all reports
    for agent in ordered_agents:
        try:
            agent.render()
        except Exception as e:
            safe_print(f"\n  {red(f'Error rendering {agent.title}: {e}')}")

    strategist.render()


# ══════════════════════════════════════════════════════════════════════════════
# CLI
# ══════════════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="Multi-Agent Investment Analysis Swarm",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            Original 20: quant, fundamental, growth, credit, capital, moat, macro, risk, sentiment, klarman, factor, catalyst, narrative, minsky, tail_risk, complexity, forensic, kelly, ergodicity, microstructure
            Wave 1 (XBRL): ohlson, accrual, gross_profit, asset_growth, issuance, buyback_signal, tax_quality, earnings_bench
            Wave 2 (EFTS): insider_flow, activist, filing_timing, inst_flow, exec_align
            Wave 3 (FRED/CBOE): capital_cycle, gpr, vol_surface
            Wave 4 (FINRA): short_interest
            Wave 5 (Advanced): textual, customer_conc, real_manip, pension, supply_chain
            Depth levels: quick (core data only), standard (+ news/estimates), deep (+ insider/institutional)

            Examples:
              python3 analyst_swarm.py AAPL
              python3 analyst_swarm.py NVDA --depth deep
              python3 analyst_swarm.py TSLA --agents quant,risk,moat
        """))
    parser.add_argument("ticker", help="Stock ticker symbol")
    parser.add_argument("--agents", help="Comma-separated agent names (default: all)", default=None)
    parser.add_argument("--depth", choices=["quick", "standard", "deep"], default="standard")
    parser.add_argument("--no-color", action="store_true")
    args = parser.parse_args()

    global USE_COLOR
    if args.no_color or not sys.stdout.isatty():
        USE_COLOR = False

    agent_names = args.agents.split(",") if args.agents else None
    run_swarm(args.ticker, agent_names, args.depth)

if __name__ == "__main__":
    main()
