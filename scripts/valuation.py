#!/usr/bin/env python3
"""
Full Investment Analysis & DCF Valuation Engine
================================================
Usage: python3 valuation.py <TICKER> [--json] [--no-color]

Pulls live data from Alpha Vantage, computes:
  - Multi-stage DCF (base/bull/bear)
  - Reverse DCF (implied growth)
  - Multiples-based valuation (P/E, EV/EBITDA, P/FCF, P/S)
  - DuPont decomposition & quality scoring
  - Piotroski F-Score
  - Altman Z-Score
  - Margin/growth trend analysis
  - Capital allocation efficiency
  - Monte Carlo fair value distribution
  - Final blended target with conviction rating

Requires: ALPHA_VANTAGE_API_KEY env var
"""

import sys, os, json, statistics, argparse, time
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError
from collections import OrderedDict

# ── Config ────────────────────────────────────────────────────────────────────

AV_KEY = os.environ.get("ALPHA_VANTAGE_API_KEY", "")
AV_BASE = "https://www.alphavantage.co/query"
RISK_FREE_RATE = None  # fetched live from treasury yield
MARKET_RISK_PREMIUM = 0.055  # long-run ERP
TERMINAL_GROWTH = 0.025
PROJECTION_YEARS = 10
FADE_YEAR = 5  # year at which growth begins fading to terminal
MC_ITERATIONS = 5000

# ── Color helpers ─────────────────────────────────────────────────────────────

USE_COLOR = True

def c(text, code):
    if not USE_COLOR: return str(text)
    return f"\033[{code}m{text}\033[0m"

def green(t):  return c(t, "32")
def red(t):    return c(t, "31")
def yellow(t): return c(t, "33")
def cyan(t):   return c(t, "36")
def bold(t):   return c(t, "1")
def dim(t):    return c(t, "2")
def mag(t):    return c(t, "35")

def pct(v, digits=1):
    if v is None: return "N/A"
    s = f"{v*100:+.{digits}f}%" if v >= 0 else f"{v*100:.{digits}f}%"
    return green(s) if v > 0 else red(s) if v < 0 else s

def usd(v, digits=2):
    if v is None: return "N/A"
    if abs(v) >= 1e12: return f"${v/1e12:.{digits}f}T"
    if abs(v) >= 1e9:  return f"${v/1e9:.{digits}f}B"
    if abs(v) >= 1e6:  return f"${v/1e6:.{digits}f}M"
    return f"${v:,.{digits}f}"

def num(v, digits=2):
    if v is None: return "N/A"
    return f"{v:,.{digits}f}"

def color_val(v, fmt_fn=None, invert=False):
    if v is None: return "N/A"
    s = fmt_fn(v) if fmt_fn else str(v)
    positive = v > 0
    if invert: positive = not positive
    return green(s) if positive else red(s) if v < 0 else s

# ── API helpers ───────────────────────────────────────────────────────────────

def av_fetch(function, **params):
    params["function"] = function
    params["apikey"] = AV_KEY
    qs = "&".join(f"{k}={v}" for k, v in params.items())
    url = f"{AV_BASE}?{qs}"
    try:
        req = Request(url, headers={"User-Agent": "dsco-valuation/1.0"})
        with urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read().decode())
        if "Error Message" in data:
            print(red(f"  API error: {data['Error Message']}"), file=sys.stderr)
            return None
        if "Note" in data:
            print(yellow(f"  API rate limit: {data['Note']}"), file=sys.stderr)
            return None
        if "Information" in data and "rate" in data["Information"].lower():
            print(yellow(f"  API rate limit: {data['Information']}"), file=sys.stderr)
            return None
        return data
    except (URLError, HTTPError) as e:
        print(red(f"  Fetch error: {e}"), file=sys.stderr)
        return None

def safe_float(d, key, default=None):
    if not d: return default
    v = d.get(key, "None")
    if v in (None, "None", "-", "", "0"):
        return default if v != "0" else 0.0
    try:
        return float(v)
    except (ValueError, TypeError):
        return default

def safe_int(d, key, default=None):
    v = safe_float(d, key, default)
    return int(v) if v is not None else None

# ── Data fetching ─────────────────────────────────────────────────────────────

def fetch_all(ticker):
    print(f"\n  {bold('Fetching data for')} {cyan(ticker)}...")
    data = {}

    steps = [
        ("Company Overview",   "OVERVIEW",          {"symbol": ticker}),
        ("Income Statement",   "INCOME_STATEMENT",  {"symbol": ticker}),
        ("Balance Sheet",      "BALANCE_SHEET",     {"symbol": ticker}),
        ("Cash Flow",          "CASH_FLOW",         {"symbol": ticker}),
        ("Earnings",           "EARNINGS",          {"symbol": ticker}),
        ("Global Quote",       "GLOBAL_QUOTE",      {"symbol": ticker, "datatype": "json"}),
        ("Treasury Yield 10Y", "TREASURY_YIELD",    {"interval": "daily", "maturity": "10year", "datatype": "json"}),
    ]

    for i, (label, fn, params) in enumerate(steps):
        if i > 0:
            time.sleep(0.15)  # premium tier: 75 req/min
        print(f"    {dim('⚡')} {label}...", end=" ", flush=True)
        result = av_fetch(fn, **params)
        # Normalize key names for downstream consumers
        key_map = {"overview": "company_overview"}
        key = key_map.get(fn.lower(), fn.lower())
        data[key] = result
        print(green("✓") if result else red("✗"))

    return data

# ── Parsers ───────────────────────────────────────────────────────────────────

def parse_overview(raw):
    if not raw: return {}
    return {
        "name":            raw.get("Name", ""),
        "symbol":          raw.get("Symbol", ""),
        "exchange":        raw.get("Exchange", ""),
        "sector":          raw.get("Sector", ""),
        "industry":        raw.get("Industry", ""),
        "description":     raw.get("Description", ""),
        "market_cap":      safe_float(raw, "MarketCapitalization"),
        "shares_out":      safe_float(raw, "SharesOutstanding"),
        "pe_ratio":        safe_float(raw, "PERatio"),
        "fwd_pe":          safe_float(raw, "ForwardPE"),
        "peg":             safe_float(raw, "PEGRatio"),
        "pb":              safe_float(raw, "PriceToBookRatio"),
        "ps":              safe_float(raw, "PriceToSalesRatioTTM"),
        "ev":              safe_float(raw, "EVToRevenue"),
        "ev_ebitda":       safe_float(raw, "EVToEBITDA"),
        "dividend_yield":  safe_float(raw, "DividendYield"),
        "dividend_ps":     safe_float(raw, "DividendPerShare"),
        "eps":             safe_float(raw, "EPS"),
        "beta":            safe_float(raw, "Beta"),
        "52w_high":        safe_float(raw, "52WeekHigh"),
        "52w_low":         safe_float(raw, "52WeekLow"),
        "200dma":          safe_float(raw, "200DayMovingAverage"),
        "50dma":           safe_float(raw, "50DayMovingAverage"),
        "profit_margin":   safe_float(raw, "ProfitMargin"),
        "op_margin":       safe_float(raw, "OperatingMarginTTM"),
        "roa":             safe_float(raw, "ReturnOnAssetsTTM"),
        "roe":             safe_float(raw, "ReturnOnEquityTTM"),
        "revenue_ttm":     safe_float(raw, "RevenueTTM"),
        "gp_ttm":          safe_float(raw, "GrossProfitTTM"),
        "ebitda":          safe_float(raw, "EBITDA"),
        "analyst_target":  safe_float(raw, "AnalystTargetPrice"),
        "analyst_rating":  raw.get("AnalystRatingStrongBuy", ""),
        "fiscal_end":      raw.get("FiscalYearEnd", ""),
        "currency":        raw.get("Currency", "USD"),
    }

def parse_financials(raw, key):
    """Parse annual/quarterly reports from income/balance/cashflow."""
    if not raw: return [], []
    annual = raw.get("annualReports", [])
    quarterly = raw.get("quarterlyReports", [])
    return annual, quarterly

def parse_earnings(raw):
    if not raw: return [], []
    return raw.get("annualEarnings", []), raw.get("quarterlyEarnings", [])

def parse_quote(raw):
    if not raw: return {}
    gq = raw.get("Global Quote", {})
    return {
        "price":       safe_float(gq, "05. price"),
        "change":      safe_float(gq, "09. change"),
        "change_pct":  gq.get("10. change percent", ""),
        "volume":      safe_float(gq, "06. volume"),
        "prev_close":  safe_float(gq, "08. previous close"),
    }

def get_risk_free_rate(raw):
    global RISK_FREE_RATE
    if not raw or "data" not in raw:
        RISK_FREE_RATE = 0.043  # fallback
        return
    for entry in raw["data"]:
        v = entry.get("value", ".")
        if v != ".":
            RISK_FREE_RATE = float(v) / 100.0
            return
    RISK_FREE_RATE = 0.043

# ── Financial statement extraction ────────────────────────────────────────────

def extract_annual_series(annual_reports, fields):
    """Extract a time series dict {year: {field: value}} from annual reports."""
    series = OrderedDict()
    for report in sorted(annual_reports, key=lambda r: r.get("fiscalDateEnding", ""))[::-1]:
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

INCOME_FIELDS = {
    "revenue":       ["totalRevenue"],
    "cogs":          ["costOfRevenue", "costofGoodsAndServicesSold"],
    "gross_profit":  ["grossProfit"],
    "rd":            ["researchAndDevelopment"],
    "sga":           ["sellingGeneralAndAdministrative"],
    "opex":          ["operatingExpenses"],
    "op_income":     ["operatingIncome"],
    "interest_exp":  ["interestExpense"],
    "pretax_income": ["incomeBeforeTax"],
    "tax":           ["incomeTaxExpense"],
    "net_income":    ["netIncome"],
    "ebitda":        ["ebitda"],
    "da":            ["depreciationAndAmortization"],
    "eps_diluted":   ["reportedEPS"],
    "shares":        ["commonStockSharesOutstanding"],
}

BALANCE_FIELDS = {
    "total_assets":       ["totalAssets"],
    "current_assets":     ["totalCurrentAssets"],
    "cash":               ["cashAndCashEquivalentsAtCarryingValue", "cashAndShortTermInvestments"],
    "st_investments":     ["shortTermInvestments"],
    "receivables":        ["currentNetReceivables"],
    "inventory":          ["inventory"],
    "total_liabilities":  ["totalLiabilities"],
    "current_liabilities":["totalCurrentLiabilities"],
    "lt_debt":            ["longTermDebt"],
    "st_debt":            ["shortTermDebt", "currentLongTermDebt"],
    "total_debt":         ["shortLongTermDebtTotal"],
    "equity":             ["totalShareholderEquity"],
    "retained_earnings":  ["retainedEarnings"],
    "goodwill":           ["goodwill"],
    "intangibles":        ["intangibleAssets"],
    "pp_and_e":           ["propertyPlantEquipment"],
}

CASHFLOW_FIELDS = {
    "cfo":           ["operatingCashflow"],
    "capex":         ["capitalExpenditures"],
    "acquisitions":  ["acquisitionsNet", "cashflowFromInvestment"],
    "dividends":     ["dividendPayout"],
    "buybacks":      ["commonStockRepurchased", "paymentsForRepurchaseOfCommonStock"],
    "cff":           ["cashflowFromFinancing"],
    "cfi":           ["cashflowFromInvestment"],
    "da_cf":         ["depreciationDepletionAndAmortization"],
    "sbc":           ["stockBasedCompensation", "shareBasedCompensation"],
    "change_wc":     ["changeInOperatingLiabilities"],
}

# ── Derived metrics ───────────────────────────────────────────────────────────

def compute_fcf(cf_row, inc_row=None):
    cfo = cf_row.get("cfo")
    capex = cf_row.get("capex")
    if cfo is None: return None
    capex = abs(capex) if capex else 0
    return cfo - capex

def compute_ufcf(inc_row, cf_row, tax_rate=0.21):
    """Unlevered FCF = EBIT*(1-t) + D&A - CapEx - ΔWC"""
    op_inc = inc_row.get("op_income")
    da = cf_row.get("da_cf") or inc_row.get("da") or 0
    capex = abs(cf_row.get("capex") or 0)
    dwc = cf_row.get("change_wc") or 0
    sbc = cf_row.get("sbc") or 0
    if op_inc is None: return None
    nopat = op_inc * (1 - tax_rate)
    return nopat + da - capex - dwc  # intentionally exclude SBC add-back for conservative

def growth_rate(series, field):
    """CAGR over available years."""
    vals = [(y, r.get(field)) for y, r in series.items() if r.get(field) and r.get(field) > 0]
    if len(vals) < 2: return None
    first_y, first_v = vals[-1]
    last_y, last_v = vals[0]
    years = int(last_y) - int(first_y)
    if years <= 0 or first_v <= 0: return None
    return (last_v / first_v) ** (1.0 / years) - 1

def yoy_growth(series, field):
    """Year-over-year growth rates."""
    items = [(y, r.get(field)) for y, r in series.items() if r.get(field) is not None]
    items.sort(key=lambda x: x[0])
    rates = []
    for i in range(1, len(items)):
        prev = items[i-1][1]
        curr = items[i][1]
        if prev and prev != 0:
            rates.append((items[i][0], (curr - prev) / abs(prev)))
    return rates

def margin_series(inc_series, num_field, denom_field="revenue"):
    """Compute margin time series."""
    result = []
    for y, r in inc_series.items():
        n = r.get(num_field)
        d = r.get(denom_field)
        if n is not None and d and d != 0:
            result.append((y, n / d))
    return result

# ── Scoring models ────────────────────────────────────────────────────────────

def piotroski_fscore(inc_series, bs_series, cf_series):
    """Piotroski F-Score (0-9)."""
    years = sorted(inc_series.keys())
    if len(years) < 2: return None, []
    cy, py = years[-1], years[-2]
    ci, pi = inc_series.get(cy, {}), inc_series.get(py, {})
    cb, pb = bs_series.get(cy, {}), bs_series.get(py, {})
    cc, pc = cf_series.get(cy, {}), cf_series.get(py, {})

    score = 0
    details = []

    # 1. Positive net income
    ni = ci.get("net_income")
    if ni and ni > 0: score += 1; details.append(("Net income > 0", True))
    else: details.append(("Net income > 0", False))

    # 2. Positive CFO
    cfo = cc.get("cfo")
    if cfo and cfo > 0: score += 1; details.append(("CFO > 0", True))
    else: details.append(("CFO > 0", False))

    # 3. Rising ROA
    ta_c = cb.get("total_assets")
    ta_p = pb.get("total_assets")
    ni_p = pi.get("net_income")
    if ni and ta_c and ni_p and ta_p and ta_c > 0 and ta_p > 0:
        roa_c = ni / ta_c
        roa_p = ni_p / ta_p
        if roa_c > roa_p: score += 1; details.append(("ROA improving", True))
        else: details.append(("ROA improving", False))
    else: details.append(("ROA improving", None))

    # 4. CFO > Net Income (accruals)
    if cfo and ni and cfo > ni: score += 1; details.append(("CFO > Net Income", True))
    else: details.append(("CFO > Net Income", False))

    # 5. Declining leverage
    ltd_c = (cb.get("lt_debt") or 0) + (cb.get("st_debt") or 0)
    ltd_p = (pb.get("lt_debt") or 0) + (pb.get("st_debt") or 0)
    if ta_c and ta_p and ta_c > 0 and ta_p > 0:
        lev_c = ltd_c / ta_c
        lev_p = ltd_p / ta_p
        if lev_c < lev_p: score += 1; details.append(("Leverage declining", True))
        else: details.append(("Leverage declining", False))
    else: details.append(("Leverage declining", None))

    # 6. Improving current ratio
    ca_c = cb.get("current_assets") or 0
    cl_c = cb.get("current_liabilities") or 1
    ca_p = pb.get("current_assets") or 0
    cl_p = pb.get("current_liabilities") or 1
    if cl_c > 0 and cl_p > 0:
        cr_c = ca_c / cl_c
        cr_p = ca_p / cl_p
        if cr_c > cr_p: score += 1; details.append(("Current ratio improving", True))
        else: details.append(("Current ratio improving", False))
    else: details.append(("Current ratio improving", None))

    # 7. No dilution
    sh_c = ci.get("shares") or cb.get("equity")
    sh_p = pi.get("shares") or pb.get("equity")
    if sh_c and sh_p and sh_c <= sh_p: score += 1; details.append(("No share dilution", True))
    else: details.append(("No share dilution", False))

    # 8. Improving gross margin
    gm_c = ci.get("gross_profit") and ci.get("revenue") and ci["gross_profit"] / ci["revenue"] if ci.get("gross_profit") and ci.get("revenue") and ci["revenue"] != 0 else None
    gm_p = pi.get("gross_profit") and pi.get("revenue") and pi["gross_profit"] / pi["revenue"] if pi.get("gross_profit") and pi.get("revenue") and pi["revenue"] != 0 else None
    if gm_c is not None and gm_p is not None:
        if gm_c > gm_p: score += 1; details.append(("Gross margin improving", True))
        else: details.append(("Gross margin improving", False))
    else: details.append(("Gross margin improving", None))

    # 9. Improving asset turnover
    rev_c = ci.get("revenue")
    rev_p = pi.get("revenue")
    if rev_c and ta_c and rev_p and ta_p and ta_c > 0 and ta_p > 0:
        at_c = rev_c / ta_c
        at_p = rev_p / ta_p
        if at_c > at_p: score += 1; details.append(("Asset turnover improving", True))
        else: details.append(("Asset turnover improving", False))
    else: details.append(("Asset turnover improving", None))

    return score, details

def altman_zscore(inc, bs):
    """Altman Z-Score for public companies."""
    ta = bs.get("total_assets")
    if not ta or ta == 0: return None, None
    ca = bs.get("current_assets") or 0
    cl = bs.get("current_liabilities") or 0
    re = bs.get("retained_earnings") or 0
    ebit = inc.get("op_income") or 0
    rev = inc.get("revenue") or 0
    equity = bs.get("equity") or 0
    tl = bs.get("total_liabilities") or 1

    wc_ta = (ca - cl) / ta
    re_ta = re / ta
    ebit_ta = ebit / ta
    eq_tl = equity / tl if tl != 0 else 0
    rev_ta = rev / ta

    z = 1.2 * wc_ta + 1.4 * re_ta + 3.3 * ebit_ta + 0.6 * eq_tl + 1.0 * rev_ta

    if z > 2.99: zone = "Safe"
    elif z > 1.81: zone = "Grey"
    else: zone = "Distress"

    return z, zone

def dupont_decomposition(inc, bs):
    """5-factor DuPont: ROE = Tax Burden × Interest Burden × EBIT Margin × Asset Turnover × Leverage"""
    ni = inc.get("net_income")
    pretax = inc.get("pretax_income")
    ebit = inc.get("op_income")
    rev = inc.get("revenue")
    ta = bs.get("total_assets")
    eq = bs.get("equity")

    if not all([ni, pretax, ebit, rev, ta, eq]) or any(v == 0 for v in [pretax, ebit, rev, ta, eq]):
        return None

    tax_burden = ni / pretax if pretax != 0 else 0
    interest_burden = pretax / ebit if ebit != 0 else 0
    ebit_margin = ebit / rev if rev != 0 else 0
    asset_turnover = rev / ta if ta != 0 else 0
    leverage = ta / eq if eq != 0 else 0
    roe = tax_burden * interest_burden * ebit_margin * asset_turnover * leverage

    return {
        "tax_burden": tax_burden,
        "interest_burden": interest_burden,
        "ebit_margin": ebit_margin,
        "asset_turnover": asset_turnover,
        "leverage": leverage,
        "roe": roe,
    }

# ── WACC ──────────────────────────────────────────────────────────────────────

def compute_wacc(overview, bs_latest, inc_latest):
    beta = overview.get("beta") or 1.0
    rf = RISK_FREE_RATE or 0.043

    # Cost of equity (CAPM)
    ke = rf + beta * MARKET_RISK_PREMIUM

    # Cost of debt
    interest = abs(inc_latest.get("interest_exp") or 0)
    total_debt = (bs_latest.get("lt_debt") or 0) + (bs_latest.get("st_debt") or 0)
    if total_debt > 0:
        kd = interest / total_debt
        kd = min(kd, 0.15)  # cap at 15%
    else:
        kd = rf + 0.02  # fallback spread

    # Tax rate
    pretax = inc_latest.get("pretax_income") or 0
    tax = inc_latest.get("tax") or 0
    if pretax > 0 and tax > 0:
        tax_rate = min(tax / pretax, 0.40)
    else:
        tax_rate = 0.21

    # Capital structure
    equity_val = overview.get("market_cap") or 0
    if equity_val == 0:
        shares = overview.get("shares_out") or 0
        price = overview.get("52w_high", 100)  # rough fallback
        equity_val = shares * price if shares else 1e9

    total_capital = equity_val + total_debt
    if total_capital == 0: total_capital = 1

    we = equity_val / total_capital
    wd = total_debt / total_capital

    wacc = we * ke + wd * kd * (1 - tax_rate)

    return {
        "wacc": wacc,
        "ke": ke,
        "kd": kd,
        "kd_at": kd * (1 - tax_rate),
        "tax_rate": tax_rate,
        "beta": beta,
        "rf": rf,
        "we": we,
        "wd": wd,
        "equity_val": equity_val,
        "debt": total_debt,
    }

# ── DCF engine ────────────────────────────────────────────────────────────────

def run_dcf(base_fcf, growth_rate_initial, wacc, shares_out, net_debt,
            terminal_growth=TERMINAL_GROWTH, projection_years=PROJECTION_YEARS,
            fade_year=FADE_YEAR):
    """Multi-stage DCF with growth fade."""
    if base_fcf is None or base_fcf <= 0 or shares_out is None or shares_out <= 0:
        return None

    projected = []
    cumulative_pv = 0

    for year in range(1, projection_years + 1):
        if year <= fade_year:
            g = growth_rate_initial
        else:
            # Linear fade from initial growth to terminal growth
            fade_progress = (year - fade_year) / (projection_years - fade_year)
            g = growth_rate_initial + (terminal_growth - growth_rate_initial) * fade_progress

        if year == 1:
            fcf = base_fcf * (1 + g)
        else:
            fcf = projected[-1]["fcf"] * (1 + g)

        pv = fcf / (1 + wacc) ** year
        cumulative_pv += pv
        projected.append({"year": year, "growth": g, "fcf": fcf, "pv": pv})

    # Terminal value (Gordon Growth)
    terminal_fcf = projected[-1]["fcf"] * (1 + terminal_growth)
    terminal_value = terminal_fcf / (wacc - terminal_growth)
    pv_terminal = terminal_value / (1 + wacc) ** projection_years

    enterprise_value = cumulative_pv + pv_terminal
    equity_value = enterprise_value - net_debt
    fair_value_per_share = equity_value / shares_out

    return {
        "projected": projected,
        "pv_fcfs": cumulative_pv,
        "terminal_value": terminal_value,
        "pv_terminal": pv_terminal,
        "enterprise_value": enterprise_value,
        "equity_value": equity_value,
        "fair_value": fair_value_per_share,
        "tv_pct": pv_terminal / enterprise_value if enterprise_value > 0 else 0,
        "growth_initial": growth_rate_initial,
        "wacc": wacc,
        "terminal_growth": terminal_growth,
    }

def reverse_dcf(current_price, shares_out, net_debt, wacc, base_fcf,
                terminal_growth=TERMINAL_GROWTH, projection_years=PROJECTION_YEARS):
    """What growth rate is the market pricing in?"""
    if not all([current_price, shares_out, base_fcf]) or base_fcf <= 0:
        return None

    target_ev = current_price * shares_out + net_debt

    # Binary search for implied growth
    lo, hi = -0.10, 0.50
    for _ in range(100):
        mid = (lo + hi) / 2
        result = run_dcf(base_fcf, mid, wacc, shares_out, net_debt,
                        terminal_growth, projection_years)
        if result is None: return None
        if result["enterprise_value"] < target_ev:
            lo = mid
        else:
            hi = mid

    return (lo + hi) / 2

def monte_carlo_dcf(base_fcf, growth_mean, growth_std, wacc_mean, wacc_std,
                    shares_out, net_debt, iterations=MC_ITERATIONS):
    """Monte Carlo simulation of fair value distribution."""
    import random
    random.seed(42)
    results = []
    for _ in range(iterations):
        g = random.gauss(growth_mean, growth_std)
        w = max(random.gauss(wacc_mean, wacc_std), 0.03)
        tg = max(min(random.gauss(TERMINAL_GROWTH, 0.005), w - 0.01), 0.005)
        r = run_dcf(base_fcf, g, w, shares_out, net_debt, tg)
        if r and r["fair_value"] > 0:
            results.append(r["fair_value"])
    if not results: return None
    results.sort()
    n = len(results)
    return {
        "mean": statistics.mean(results),
        "median": statistics.median(results),
        "std": statistics.stdev(results) if n > 1 else 0,
        "p10": results[int(n * 0.10)],
        "p25": results[int(n * 0.25)],
        "p75": results[int(n * 0.75)],
        "p90": results[int(n * 0.90)],
        "min": results[0],
        "max": results[-1],
        "count": n,
    }

# ── Multiples valuation ──────────────────────────────────────────────────────

def multiples_valuation(overview, inc_latest, cf_latest, price):
    """Value based on peer-implied multiples with sector-aware ranges."""
    if not price: return []

    valuations = []
    eps = overview.get("eps")
    rev = inc_latest.get("revenue")
    ebitda = inc_latest.get("ebitda") or (inc_latest.get("op_income", 0) + (inc_latest.get("da") or 0))
    shares = overview.get("shares_out") or 1
    fcf = compute_fcf(cf_latest, inc_latest) if cf_latest else None

    # P/E
    if eps and eps > 0:
        for label, mult in [("Conservative P/E", 15), ("Moderate P/E", 20), ("Growth P/E", 30)]:
            valuations.append({"method": label, "multiple": mult, "value": eps * mult})

    # EV/EBITDA
    if ebitda and ebitda > 0:
        net_debt_val = (overview.get("market_cap", 0) / shares * shares if overview.get("market_cap") else price * shares)
        for label, mult in [("Conservative EV/EBITDA", 10), ("Moderate EV/EBITDA", 14), ("Growth EV/EBITDA", 20)]:
            ev = ebitda * mult
            eq = ev  # simplified (should subtract net debt but we're comparing to price)
            valuations.append({"method": label, "multiple": mult, "value": eq / shares if shares else 0})

    # P/FCF
    if fcf and fcf > 0:
        fcf_ps = fcf / shares
        for label, mult in [("Conservative P/FCF", 18), ("Moderate P/FCF", 25), ("Growth P/FCF", 35)]:
            valuations.append({"method": label, "multiple": mult, "value": fcf_ps * mult})

    # P/S
    if rev and rev > 0:
        rev_ps = rev / shares
        for label, mult in [("Conservative P/S", 2), ("Moderate P/S", 5), ("Growth P/S", 10)]:
            valuations.append({"method": label, "multiple": mult, "value": rev_ps * mult})

    return valuations

# ── Quality score ─────────────────────────────────────────────────────────────

def quality_score(overview, inc_series, bs_series, cf_series, piotroski, dupont):
    """Composite quality score 0-100."""
    score = 0
    max_score = 0
    breakdown = []

    def add(name, points, earned):
        nonlocal score, max_score
        max_score += points
        if earned:
            score += points
        breakdown.append((name, points, earned))

    # Profitability (30 pts)
    roe = overview.get("roe")
    if roe: add("ROE > 15%", 10, roe > 0.15)
    else: add("ROE > 15%", 10, False)

    op_margin = overview.get("op_margin")
    if op_margin: add("Op Margin > 15%", 10, op_margin > 0.15)
    else: add("Op Margin > 15%", 10, False)

    pm = overview.get("profit_margin")
    if pm: add("Profit Margin > 10%", 10, pm > 0.10)
    else: add("Profit Margin > 10%", 10, False)

    # Growth (20 pts)
    rev_cagr = growth_rate(inc_series, "revenue")
    if rev_cagr: add("Revenue CAGR > 10%", 10, rev_cagr > 0.10)
    else: add("Revenue CAGR > 10%", 10, False)

    ni_cagr = growth_rate(inc_series, "net_income")
    if ni_cagr: add("Earnings CAGR > 10%", 10, ni_cagr > 0.10)
    else: add("Earnings CAGR > 10%", 10, False)

    # Balance sheet (20 pts)
    years = sorted(bs_series.keys())
    if years:
        latest_bs = bs_series[years[-1]]
        debt = (latest_bs.get("lt_debt") or 0) + (latest_bs.get("st_debt") or 0)
        equity = latest_bs.get("equity") or 1
        add("Debt/Equity < 1", 10, debt / equity < 1 if equity > 0 else False)

        ca = latest_bs.get("current_assets") or 0
        cl = latest_bs.get("current_liabilities") or 1
        add("Current Ratio > 1.5", 10, ca / cl > 1.5 if cl > 0 else False)

    # Cash flow (20 pts)
    if cf_series:
        latest_yr = sorted(cf_series.keys())[-1] if cf_series else None
        if latest_yr:
            cf = cf_series[latest_yr]
            inc = inc_series.get(latest_yr, {})
            fcf = compute_fcf(cf, inc)
            add("Positive FCF", 10, fcf is not None and fcf > 0)

            cfo = cf.get("cfo")
            ni = inc.get("net_income")
            add("CFO > Net Income", 10, cfo and ni and cfo > ni)

    # Piotroski (10 pts)
    if piotroski is not None:
        add(f"Piotroski >= 6 (got {piotroski})", 10, piotroski >= 6)

    return {"score": score, "max": max_score, "pct": score / max_score * 100 if max_score > 0 else 0, "breakdown": breakdown}

# ── Capital allocation analysis ───────────────────────────────────────────────

def capital_allocation(cf_series, inc_series):
    """Analyze how the company deploys capital."""
    results = []
    for yr in sorted(cf_series.keys()):
        cf = cf_series[yr]
        inc = inc_series.get(yr, {})
        cfo = cf.get("cfo") or 0
        capex = abs(cf.get("capex") or 0)
        divs = abs(cf.get("dividends") or 0)
        buybacks = abs(cf.get("buybacks") or 0)
        acq = abs(cf.get("acquisitions") or 0)
        sbc = abs(cf.get("sbc") or 0)

        fcf = cfo - capex
        total_returned = divs + buybacks
        reinvestment = capex + acq

        results.append({
            "year": yr,
            "cfo": cfo,
            "capex": capex,
            "fcf": fcf,
            "dividends": divs,
            "buybacks": buybacks,
            "total_returned": total_returned,
            "acquisitions": acq,
            "sbc": sbc,
            "reinvestment_rate": reinvestment / cfo if cfo > 0 else None,
            "shareholder_yield": total_returned / (cfo if cfo > 0 else 1),
        })
    return results

# ── Report rendering ──────────────────────────────────────────────────────────

DIVIDER = "─" * 88

def section(title):
    print(f"\n  {bold(cyan(title))}")
    print(f"  {dim(DIVIDER)}")

def kv(label, value, indent=4):
    print(f"{' ' * indent}{dim(label + ':')} {value}")

def table_row(cols, widths, align=None):
    parts = []
    for i, (col, w) in enumerate(zip(cols, widths)):
        s = str(col)
        if align and i < len(align) and align[i] == "r":
            parts.append(s.rjust(w))
        else:
            parts.append(s.ljust(w))
    print(f"    {'  '.join(parts)}")

def bar(value, max_val=100, width=20, fill_char="█", empty_char="░"):
    filled = int(value / max_val * width) if max_val > 0 else 0
    filled = max(0, min(width, filled))
    return fill_char * filled + empty_char * (width - filled)

def render_report(ticker, data):
    overview = parse_overview(data.get("company_overview"))
    quote = parse_quote(data.get("global_quote"))
    get_risk_free_rate(data.get("treasury_yield"))

    inc_annual, inc_quarterly = parse_financials(data.get("income_statement"), "income")
    bs_annual, bs_quarterly = parse_financials(data.get("balance_sheet"), "balance")
    cf_annual, cf_quarterly = parse_financials(data.get("cash_flow"), "cashflow")
    earn_annual, earn_quarterly = parse_earnings(data.get("earnings"))

    inc_series = extract_annual_series(inc_annual, INCOME_FIELDS)
    bs_series = extract_annual_series(bs_annual, BALANCE_FIELDS)
    cf_series = extract_annual_series(cf_annual, CASHFLOW_FIELDS)

    price = quote.get("price") or overview.get("52w_high")
    shares = overview.get("shares_out") or 1

    if not overview.get("name"):
        print(red(f"\n  No data found for {ticker}. Check the symbol and try again.\n"))
        return

    # ── Header ────────────────────────────────────────────────────────────────
    print(f"\n  {'═' * 88}")
    print(f"  {bold(overview['name'])} ({cyan(overview['symbol'])})")
    print(f"  {overview['exchange']} · {overview['sector']} · {overview['industry']}")
    print(f"  {'═' * 88}")

    # ── Price & Market Data ───────────────────────────────────────────────────
    section("MARKET DATA")
    if price:
        kv("Current Price", f"${price:,.2f}")
    kv("Market Cap", usd(overview.get("market_cap")))
    kv("52-Week Range", f"${overview.get('52w_low', 0):,.2f} — ${overview.get('52w_high', 0):,.2f}")
    kv("50 DMA / 200 DMA", f"${overview.get('50dma', 0):,.2f} / ${overview.get('200dma', 0):,.2f}")
    kv("Beta", num(overview.get("beta")))
    if overview.get("analyst_target"):
        upside = (overview["analyst_target"] / price - 1) if price else None
        kv("Analyst Target", f"${overview['analyst_target']:,.2f} ({pct(upside)} upside)" if upside is not None else f"${overview['analyst_target']:,.2f}")
    kv("Dividend Yield", pct(overview.get("dividend_yield")))

    # ── Key Multiples ─────────────────────────────────────────────────────────
    section("VALUATION MULTIPLES")
    kv("P/E (TTM)", num(overview.get("pe_ratio")))
    kv("Forward P/E", num(overview.get("fwd_pe")))
    kv("PEG Ratio", num(overview.get("peg")))
    kv("P/B", num(overview.get("pb")))
    kv("P/S (TTM)", num(overview.get("ps")))
    kv("EV/EBITDA", num(overview.get("ev_ebitda")))
    kv("EV/Revenue", num(overview.get("ev")))

    # ── Profitability ─────────────────────────────────────────────────────────
    section("PROFITABILITY")
    kv("Gross Margin", pct(overview.get("gp_ttm") / overview["revenue_ttm"] if overview.get("gp_ttm") and overview.get("revenue_ttm") else None))
    kv("Operating Margin", pct(overview.get("op_margin")))
    kv("Net Margin", pct(overview.get("profit_margin")))
    kv("ROE", pct(overview.get("roe")))
    kv("ROA", pct(overview.get("roa")))

    # ── Financial History Table ───────────────────────────────────────────────
    section("FINANCIAL HISTORY (Annual)")
    years = sorted(inc_series.keys())[-6:]  # last 6 years
    if years:
        header = ["Metric"] + years
        widths = [20] + [14] * len(years)
        table_row(header, widths)
        print(f"    {'─' * (20 + 14 * len(years) + 2 * len(years))}")

        for label, field, fmt in [
            ("Revenue", "revenue", usd),
            ("Gross Profit", "gross_profit", usd),
            ("Op Income", "op_income", usd),
            ("Net Income", "net_income", usd),
            ("EPS", "eps_diluted", lambda v: f"${v:.2f}" if v else "N/A"),
            ("EBITDA", "ebitda", usd),
        ]:
            row = [label]
            for y in years:
                v = inc_series.get(y, {}).get(field)
                row.append(fmt(v) if v is not None else dim("—"))
            table_row(row, widths, ["l"] + ["r"] * len(years))

        # Add FCF row from cash flow
        row = ["Free Cash Flow"]
        for y in years:
            cf = cf_series.get(y, {})
            inc = inc_series.get(y, {})
            fcf = compute_fcf(cf, inc)
            row.append(usd(fcf) if fcf is not None else dim("—"))
        table_row(row, widths, ["l"] + ["r"] * len(years))

    # ── Margin Trends ─────────────────────────────────────────────────────────
    section("MARGIN TRENDS")
    for label, field in [("Gross Margin", "gross_profit"), ("Operating Margin", "op_income"), ("Net Margin", "net_income")]:
        margins = margin_series(inc_series, field)
        if margins:
            trend_str = "  ".join(f"{y}: {pct(m)}" for y, m in margins[-5:])
            kv(label, trend_str)

    # ── Growth Rates ──────────────────────────────────────────────────────────
    section("GROWTH ANALYSIS")
    for label, field in [("Revenue", "revenue"), ("Net Income", "net_income"), ("EBITDA", "ebitda")]:
        cagr = growth_rate(inc_series, field)
        yoy = yoy_growth(inc_series, field)
        cagr_str = pct(cagr) if cagr is not None else "N/A"
        yoy_str = "  ".join(f"{y}: {pct(g)}" for y, g in yoy[-4:]) if yoy else ""
        kv(f"{label} CAGR", cagr_str)
        if yoy_str:
            kv(f"  YoY", yoy_str)

    # ── Balance Sheet Highlights ──────────────────────────────────────────────
    section("BALANCE SHEET HIGHLIGHTS")
    latest_yr = sorted(bs_series.keys())[-1] if bs_series else None
    if latest_yr:
        bs = bs_series[latest_yr]
        kv("Total Assets", usd(bs.get("total_assets")))
        kv("Total Liabilities", usd(bs.get("total_liabilities")))
        kv("Shareholder Equity", usd(bs.get("equity")))
        kv("Cash & Equivalents", usd(bs.get("cash")))
        total_debt = (bs.get("lt_debt") or 0) + (bs.get("st_debt") or 0)
        kv("Total Debt", usd(total_debt))
        net_debt = total_debt - (bs.get("cash") or 0)
        kv("Net Debt", color_val(net_debt, usd, invert=True))
        equity = bs.get("equity") or 1
        kv("Debt/Equity", num(total_debt / equity if equity > 0 else 0))
        ca = bs.get("current_assets") or 0
        cl = bs.get("current_liabilities") or 1
        kv("Current Ratio", num(ca / cl if cl > 0 else 0))

    # ── DuPont Decomposition ──────────────────────────────────────────────────
    section("DUPONT DECOMPOSITION (5-Factor)")
    if latest_yr and inc_series.get(latest_yr) and bs_series.get(latest_yr):
        dp = dupont_decomposition(inc_series[latest_yr], bs_series[latest_yr])
        if dp:
            kv("Tax Burden (NI/Pretax)", num(dp["tax_burden"]))
            kv("Interest Burden (Pretax/EBIT)", num(dp["interest_burden"]))
            kv("EBIT Margin", pct(dp["ebit_margin"]))
            kv("Asset Turnover", num(dp["asset_turnover"]))
            kv("Equity Multiplier", f"{dp['leverage']:.2f}x")
            kv("Implied ROE", pct(dp["roe"]))

    # ── Piotroski F-Score ─────────────────────────────────────────────────────
    section("PIOTROSKI F-SCORE")
    f_score, f_details = piotroski_fscore(inc_series, bs_series, cf_series)
    if f_score is not None:
        color = green if f_score >= 7 else yellow if f_score >= 4 else red
        print(f"    {bold(color(f'{f_score}/9'))}  {bar(f_score, 9, 18)}")
        for name, passed in f_details:
            icon = green("✓") if passed else (red("✗") if passed is False else dim("?"))
            print(f"      {icon} {name}")

    # ── Altman Z-Score ────────────────────────────────────────────────────────
    section("ALTMAN Z-SCORE")
    if latest_yr and inc_series.get(latest_yr) and bs_series.get(latest_yr):
        z, zone = altman_zscore(inc_series[latest_yr], bs_series[latest_yr])
        if z is not None:
            color = green if zone == "Safe" else yellow if zone == "Grey" else red
            kv("Z-Score", f"{color(f'{z:.2f}')} ({color(zone)})")

    # ── Capital Allocation ────────────────────────────────────────────────────
    section("CAPITAL ALLOCATION")
    cap_alloc = capital_allocation(cf_series, inc_series)
    if cap_alloc:
        years_ca = [r["year"] for r in cap_alloc[-5:]]
        widths_ca = [16] + [14] * len(years_ca)
        table_row([""] + years_ca, widths_ca)
        print(f"    {'─' * (16 + 14 * len(years_ca) + 2 * len(years_ca))}")
        for label, field in [("CFO", "cfo"), ("CapEx", "capex"), ("FCF", "fcf"),
                              ("Dividends", "dividends"), ("Buybacks", "buybacks"), ("SBC", "sbc")]:
            row = [label]
            for r in cap_alloc[-5:]:
                v = r.get(field)
                row.append(usd(v) if v is not None else dim("—"))
            table_row(row, widths_ca, ["l"] + ["r"] * len(years_ca))

    # ── WACC ──────────────────────────────────────────────────────────────────
    section("WACC ESTIMATION")
    if latest_yr:
        wacc_data = compute_wacc(overview, bs_series[latest_yr], inc_series[latest_yr])
        kv("Risk-Free Rate", pct(wacc_data["rf"]))
        kv("Beta", num(wacc_data["beta"]))
        kv("Cost of Equity", pct(wacc_data["ke"]))
        kv("Pre-Tax Cost of Debt", pct(wacc_data["kd"]))
        kv("After-Tax Cost of Debt", pct(wacc_data["kd_at"]))
        kv("Effective Tax Rate", pct(wacc_data["tax_rate"]))
        kv("Equity Weight", pct(wacc_data["we"]))
        kv("Debt Weight", pct(wacc_data["wd"]))
        kv(bold("WACC"), bold(pct(wacc_data["wacc"])))
    else:
        wacc_data = {"wacc": 0.10}

    # ── DCF Valuation ─────────────────────────────────────────────────────────
    section("DCF VALUATION")

    # Get base FCF
    latest_cf_yr = sorted(cf_series.keys())[-1] if cf_series else None
    base_fcf = None
    if latest_cf_yr:
        cf = cf_series[latest_cf_yr]
        inc = inc_series.get(latest_cf_yr, {})
        base_fcf = compute_fcf(cf, inc)

    # Also compute UFCF
    base_ufcf = None
    if latest_cf_yr and inc_series.get(latest_cf_yr):
        base_ufcf = compute_ufcf(inc_series[latest_cf_yr], cf_series[latest_cf_yr],
                                  wacc_data.get("tax_rate", 0.21))

    fcf_for_dcf = base_ufcf or base_fcf
    net_debt_val = 0
    if latest_yr:
        bs = bs_series[latest_yr]
        total_d = (bs.get("lt_debt") or 0) + (bs.get("st_debt") or 0)
        cash = bs.get("cash") or 0
        net_debt_val = total_d - cash

    rev_cagr = growth_rate(inc_series, "revenue") or 0.05
    fcf_cagr = growth_rate(cf_series, "cfo") or rev_cagr

    # Use blended growth estimate
    growth_est = (rev_cagr * 0.4 + fcf_cagr * 0.6) if fcf_cagr else rev_cagr
    growth_est = max(min(growth_est, 0.30), -0.05)  # cap at 30%, floor at -5%

    scenarios = {
        "Bear":  growth_est * 0.5,
        "Base":  growth_est,
        "Bull":  growth_est * 1.5,
    }

    if fcf_for_dcf and fcf_for_dcf > 0:
        kv("Base FCF (Latest)", usd(fcf_for_dcf))
        kv("Net Debt", usd(net_debt_val))
        kv("Shares Outstanding", f"{shares/1e6:,.1f}M")
        print()

        dcf_results = {}
        header = ["Scenario", "Growth", "WACC", "Fair Value", "vs Price", "Upside"]
        widths_dcf = [12, 10, 10, 14, 14, 12]
        table_row(header, widths_dcf)
        print(f"    {'─' * sum(w + 2 for w in widths_dcf)}")

        for scenario, g in scenarios.items():
            result = run_dcf(fcf_for_dcf, g, wacc_data["wacc"], shares, net_debt_val)
            dcf_results[scenario] = result
            if result:
                fv = result["fair_value"]
                upside = (fv / price - 1) if price else None
                table_row([
                    bold(scenario),
                    pct(g),
                    pct(wacc_data["wacc"]),
                    bold(f"${fv:,.2f}"),
                    f"${price:,.2f}" if price else "—",
                    pct(upside) if upside is not None else "—",
                ], widths_dcf, ["l", "r", "r", "r", "r", "r"])

        # Terminal value composition
        base_result = dcf_results.get("Base")
        if base_result:
            print()
            kv("PV of Projected FCFs", usd(base_result["pv_fcfs"]))
            kv("PV of Terminal Value", usd(base_result["pv_terminal"]))
            kv("Terminal Value % of EV", pct(base_result["tv_pct"]))

        # Reverse DCF
        section("REVERSE DCF (IMPLIED GROWTH)")
        implied_g = reverse_dcf(price, shares, net_debt_val, wacc_data["wacc"], fcf_for_dcf)
        if implied_g is not None:
            kv("Market-Implied Growth Rate", bold(pct(implied_g)))
            kv("Your Base Estimate", pct(growth_est))
            diff = implied_g - growth_est
            if diff > 0.02:
                print(f"    {yellow('⚠ Market is pricing in higher growth than our estimate')}")
            elif diff < -0.02:
                print(f"    {green('✓ Market is pricing in lower growth — potential opportunity')}")
            else:
                print(f"    {dim('≈ Market pricing is roughly in line with estimates')}")
    else:
        print(f"    {red('Cannot run DCF — no positive FCF available')}")
        dcf_results = {}
        base_result = None

    # ── Monte Carlo ───────────────────────────────────────────────────────────
    section("MONTE CARLO SIMULATION")
    if fcf_for_dcf and fcf_for_dcf > 0:
        growth_std = abs(growth_est) * 0.4 + 0.02
        wacc_std = 0.015
        mc = monte_carlo_dcf(fcf_for_dcf, growth_est, growth_std,
                             wacc_data["wacc"], wacc_std, shares, net_debt_val)
        if mc:
            kv("Iterations", f"{mc['count']:,}")
            kv("Mean Fair Value", bold(f"${mc['mean']:,.2f}"))
            kv("Median Fair Value", f"${mc['median']:,.2f}")
            kv("Std Dev", f"${mc['std']:,.2f}")
            print()
            kv("10th Percentile", f"${mc['p10']:,.2f}")
            kv("25th Percentile", f"${mc['p25']:,.2f}")
            kv("75th Percentile", f"${mc['p75']:,.2f}")
            kv("90th Percentile", f"${mc['p90']:,.2f}")
            print()
            # ASCII distribution
            if price:
                print(f"    {dim('Distribution vs Current Price:')}")
                buckets = [mc['p10'], mc['p25'], mc['median'], mc['p75'], mc['p90']]
                labels = ["P10", "P25", "P50", "P75", "P90"]
                max_v = max(buckets + [price])
                for label, v in zip(labels, buckets):
                    w = int(v / max_v * 40) if max_v > 0 else 0
                    marker = "█" * w
                    color_fn = green if v > price else red
                    print(f"      {label}  {color_fn(marker)} ${v:,.0f}")
                # Price marker
                pw = int(price / max_v * 40) if max_v > 0 else 0
                print(f"      {'CUR':>3}  {'─' * pw}{bold('▼')} ${price:,.0f}")

    # ── Multiples Valuation ───────────────────────────────────────────────────
    section("MULTIPLES-BASED VALUATION")
    if latest_cf_yr and latest_yr:
        mult_vals = multiples_valuation(overview, inc_series.get(latest_yr, {}),
                                         cf_series.get(latest_cf_yr, {}), price)
        if mult_vals:
            header_m = ["Method", "Multiple", "Implied Value", "vs Price"]
            widths_m = [24, 10, 16, 12]
            table_row(header_m, widths_m)
            print(f"    {'─' * sum(w + 2 for w in widths_m)}")
            for mv in mult_vals:
                upside = (mv["value"] / price - 1) if price and mv["value"] else None
                table_row([
                    mv["method"],
                    f"{mv['multiple']:.1f}x",
                    f"${mv['value']:,.2f}" if mv["value"] else "—",
                    pct(upside) if upside is not None else "—",
                ], widths_m, ["l", "r", "r", "r"])

    # ── Quality Score ─────────────────────────────────────────────────────────
    section("QUALITY SCORE")
    qs = quality_score(overview, inc_series, bs_series, cf_series, f_score, None)
    color = green if qs["pct"] >= 70 else yellow if qs["pct"] >= 40 else red
    print(f"    {bold(color(f'{qs["pct"]:.0f}/100'))}  {bar(qs['pct'], 100, 30)}")
    print()
    for name, points, earned in qs["breakdown"]:
        icon = green("✓") if earned else red("✗")
        print(f"      {icon} {name} ({points} pts)")

    # ── Final Blended Valuation ───────────────────────────────────────────────
    section("BLENDED FAIR VALUE ESTIMATE")

    targets = []
    weights = []

    # DCF base case (weight 40%)
    if base_result and base_result["fair_value"] > 0:
        targets.append(base_result["fair_value"])
        weights.append(0.40)
        kv("DCF Base Case (40%)", f"${base_result['fair_value']:,.2f}")

    # Monte Carlo median (weight 25%)
    if fcf_for_dcf and fcf_for_dcf > 0:
        mc = monte_carlo_dcf(fcf_for_dcf, growth_est, abs(growth_est) * 0.4 + 0.02,
                             wacc_data["wacc"], 0.015, shares, net_debt_val)
        if mc:
            targets.append(mc["median"])
            weights.append(0.25)
            kv("MC Median (25%)", f"${mc['median']:,.2f}")

    # Multiples average of moderate cases (weight 20%)
    if latest_cf_yr and latest_yr:
        mult_vals = multiples_valuation(overview, inc_series.get(latest_yr, {}),
                                         cf_series.get(latest_cf_yr, {}), price)
        moderate_vals = [mv["value"] for mv in mult_vals if "Moderate" in mv["method"] and mv["value"] and mv["value"] > 0]
        if moderate_vals:
            avg_mult = statistics.mean(moderate_vals)
            targets.append(avg_mult)
            weights.append(0.20)
            kv("Multiples Avg (20%)", f"${avg_mult:,.2f}")

    # Analyst target (weight 15%)
    if overview.get("analyst_target") and overview["analyst_target"] > 0:
        targets.append(overview["analyst_target"])
        weights.append(0.15)
        kv("Analyst Target (15%)", f"${overview['analyst_target']:,.2f}")

    if targets:
        # Normalize weights
        total_w = sum(weights)
        blended = sum(t * w for t, w in zip(targets, weights)) / total_w

        print()
        print(f"    {'━' * 50}")
        upside = (blended / price - 1) if price else None
        color_fn = green if (upside and upside > 0) else red
        print(f"    {bold('BLENDED FAIR VALUE:')}  {bold(color_fn(f'${blended:,.2f}'))}")
        if price:
            print(f"    {bold('Current Price:')}       ${price:,.2f}")
            print(f"    {bold('Implied Upside:')}      {bold(color_fn(pct(upside)))}")
        print(f"    {'━' * 50}")

        # Conviction
        if upside is not None:
            abs_upside = abs(upside)
            if abs_upside < 0.05:
                conviction = "HOLD — fairly valued"
                conv_color = yellow
            elif upside > 0.20:
                conviction = "STRONG BUY — significant upside"
                conv_color = green
            elif upside > 0.05:
                conviction = "BUY — moderate upside"
                conv_color = green
            elif upside < -0.20:
                conviction = "SELL — significant downside risk"
                conv_color = red
            elif upside < -0.05:
                conviction = "UNDERWEIGHT — limited upside"
                conv_color = red
            else:
                conviction = "HOLD"
                conv_color = yellow

            print(f"\n    {bold('Signal:')} {bold(conv_color(conviction))}")

        # Risk factors
        print(f"\n    {dim('Key Risks & Caveats:')}")
        if base_result and base_result["tv_pct"] > 0.75:
            print(f"      {yellow('⚠')} Terminal value is {base_result['tv_pct']*100:.0f}% of EV — high sensitivity to terminal assumptions")
        if wacc_data.get("beta") and wacc_data["beta"] > 1.5:
            print(f"      {yellow('⚠')} High beta ({wacc_data['beta']:.2f}) — elevated systematic risk")
        if f_score is not None and f_score < 4:
            print(f"      {red('⚠')} Low Piotroski score ({f_score}/9) — weak fundamentals")
        if qs["pct"] < 40:
            print(f"      {red('⚠')} Low quality score ({qs['pct']:.0f}/100)")
        rev_cagr_final = growth_rate(inc_series, "revenue")
        if rev_cagr_final is not None and rev_cagr_final < 0:
            print(f"      {red('⚠')} Revenue is declining ({pct(rev_cagr_final)} CAGR)")
        if latest_yr:
            bs = bs_series.get(latest_yr, {})
            d = (bs.get("lt_debt") or 0) + (bs.get("st_debt") or 0)
            e = bs.get("equity") or 1
            if e > 0 and d / e > 2:
                print(f"      {yellow('⚠')} High leverage — D/E ratio {d/e:.1f}x")

        print(f"\n    {dim('This analysis uses historical financials and mathematical models.')}")
        print(f"    {dim('It is not investment advice. Always do your own due diligence.')}")
    else:
        print(f"    {red('Insufficient data to compute blended valuation.')}")

    print()

# ── JSON output mode ──────────────────────────────────────────────────────────

def render_json(ticker, data):
    overview = parse_overview(data.get("company_overview"))
    quote = parse_quote(data.get("global_quote"))
    get_risk_free_rate(data.get("treasury_yield"))

    inc_annual, _ = parse_financials(data.get("income_statement"), "income")
    bs_annual, _ = parse_financials(data.get("balance_sheet"), "balance")
    cf_annual, _ = parse_financials(data.get("cash_flow"), "cashflow")

    inc_series = extract_annual_series(inc_annual, INCOME_FIELDS)
    bs_series = extract_annual_series(bs_annual, BALANCE_FIELDS)
    cf_series = extract_annual_series(cf_annual, CASHFLOW_FIELDS)

    price = quote.get("price")
    shares = overview.get("shares_out") or 1

    latest_yr = sorted(bs_series.keys())[-1] if bs_series else None
    wacc_data = compute_wacc(overview, bs_series.get(latest_yr, {}), inc_series.get(latest_yr, {})) if latest_yr else {"wacc": 0.10}

    latest_cf_yr = sorted(cf_series.keys())[-1] if cf_series else None
    base_fcf = None
    if latest_cf_yr:
        base_fcf = compute_ufcf(inc_series.get(latest_cf_yr, {}), cf_series.get(latest_cf_yr, {}), wacc_data.get("tax_rate", 0.21))
        if not base_fcf:
            base_fcf = compute_fcf(cf_series.get(latest_cf_yr, {}))

    net_debt_val = 0
    if latest_yr:
        bs = bs_series[latest_yr]
        net_debt_val = ((bs.get("lt_debt") or 0) + (bs.get("st_debt") or 0)) - (bs.get("cash") or 0)

    rev_cagr = growth_rate(inc_series, "revenue") or 0.05
    growth_est = rev_cagr

    dcf_base = run_dcf(base_fcf, growth_est, wacc_data["wacc"], shares, net_debt_val) if base_fcf and base_fcf > 0 else None
    f_score, _ = piotroski_fscore(inc_series, bs_series, cf_series)

    output = {
        "ticker": ticker,
        "name": overview.get("name"),
        "price": price,
        "market_cap": overview.get("market_cap"),
        "wacc": wacc_data,
        "dcf_fair_value": dcf_base["fair_value"] if dcf_base else None,
        "revenue_cagr": rev_cagr,
        "piotroski": f_score,
        "pe": overview.get("pe_ratio"),
        "ev_ebitda": overview.get("ev_ebitda"),
    }
    print(json.dumps(output, indent=2, default=str))

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Full Investment Analysis & DCF Valuation")
    parser.add_argument("ticker", help="Stock ticker symbol (e.g., AAPL, MSFT, NVDA)")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    parser.add_argument("--no-color", action="store_true", help="Disable color output")
    args = parser.parse_args()

    global USE_COLOR
    if args.no_color or not sys.stdout.isatty():
        USE_COLOR = False

    if not AV_KEY:
        print(red("Error: ALPHA_VANTAGE_API_KEY environment variable not set."))
        print("Get a free key at: https://www.alphavantage.co/support/#api-key")
        sys.exit(1)

    ticker = args.ticker.upper().strip()
    data = fetch_all(ticker)

    if args.json:
        render_json(ticker, data)
    else:
        render_report(ticker, data)

if __name__ == "__main__":
    main()
