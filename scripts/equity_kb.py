#!/usr/bin/env python3
"""
Equity Knowledge Base — Full-Universe Research Engine
=====================================================
Usage:
  python3 equity_kb.py build [--limit N] [--sector TECH] [--workers N]
  python3 equity_kb.py update [--ticker AAPL] [--stale-days 30]
  python3 equity_kb.py search "cloud computing high margins"
  python3 equity_kb.py report AAPL [--depth deep]
  python3 equity_kb.py screen --min-roic 15 --min-revenue-cagr 10 --max-de 1.5
  python3 equity_kb.py export [--format csv|json] [--query "..."]
  python3 equity_kb.py stats

Builds a persistent SQLite knowledge base of all US-listed equities using:
  - SEC EDGAR XBRL (authoritative financials, free, unlimited)
  - Jina Reader (r.jina.ai) for clean web content extraction
  - Jina Search (s.jina.ai) for competitive/product research
  - Jina Embeddings for semantic search across the KB
  - Parallel.ai Tasks API for deep automated research reports

Requires env vars: JINA_API_KEY, PARALLEL_API_KEY (optional: ALPHA_VANTAGE_API_KEY)
"""

import sys, os, json, time, sqlite3, argparse, hashlib, struct, textwrap, csv, io
from concurrent.futures import ThreadPoolExecutor, as_completed
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError
from urllib.parse import quote_plus
from collections import OrderedDict
from threading import Lock, Semaphore
from datetime import datetime, timedelta
from pathlib import Path

# ══════════════════════════════════════════════════════════════════════════════
# Config
# ══════════════════════════════════════════════════════════════════════════════

DB_PATH = os.environ.get("DSCO_EQUITY_DB", os.path.expanduser("~/.dsco/equity_kb.db"))
JINA_KEY = os.environ.get("JINA_API_KEY", "")
PARALLEL_KEY = os.environ.get("PARALLEL_API_KEY", "")
AV_KEY = os.environ.get("ALPHA_VANTAGE_API_KEY", "")

EDGAR_FACTS_URL = "https://data.sec.gov/api/xbrl/companyfacts"
EDGAR_TICKERS_URL = "https://www.sec.gov/files/company_tickers.json"
EDGAR_UA = "dsco-equity-kb/1.0 research@dsco.dev"

JINA_READER = "https://r.jina.ai"
JINA_SEARCH = "https://s.jina.ai"
JINA_EMBED = "https://api.jina.ai/v1/embeddings"

PARALLEL_SEARCH = "https://api.parallel.ai/v1beta/search"
PARALLEL_EXTRACT = "https://api.parallel.ai/v1beta/extract"
PARALLEL_TASKS = "https://api.parallel.ai/v1/tasks/runs"

EMBED_DIM = 256  # truncated Matryoshka dimension for storage efficiency
EMBED_MODEL = "jina-embeddings-v3"

# ══════════════════════════════════════════════════════════════════════════════
# Terminal formatting
# ══════════════════════════════════════════════════════════════════════════════

USE_COLOR = sys.stdout.isatty()

def c(text, code):
    if not USE_COLOR: return str(text)
    return f"\033[{code}m{text}\033[0m"

def green(t):  return c(t, "32")
def red(t):    return c(t, "31")
def yellow(t): return c(t, "33")
def cyan(t):   return c(t, "36")
def bold(t):   return c(t, "1")
def dim(t):    return c(t, "2")

_print_lock = Lock()
def sprint(*args, **kw):
    with _print_lock:
        print(*args, **kw)

def pct(v):
    if v is None: return dim("N/A")
    return f"{v*100:+.1f}%"

def usd(v):
    if v is None: return dim("N/A")
    a = abs(v)
    s = f"${a/1e12:.1f}T" if a >= 1e12 else f"${a/1e9:.1f}B" if a >= 1e9 else f"${a/1e6:.0f}M" if a >= 1e6 else f"${a:,.0f}"
    return f"-{s}" if v < 0 else s

# ══════════════════════════════════════════════════════════════════════════════
# SQLite schema
# ══════════════════════════════════════════════════════════════════════════════

SCHEMA = """
CREATE TABLE IF NOT EXISTS companies (
    ticker          TEXT PRIMARY KEY,
    cik             TEXT,
    name            TEXT,
    sic_code        TEXT,
    sector          TEXT,
    industry        TEXT,
    exchange        TEXT,
    description     TEXT,
    market_cap      REAL,
    last_updated    TEXT,
    edgar_fetched   TEXT,
    research_date   TEXT
);

CREATE TABLE IF NOT EXISTS annual_financials (
    ticker          TEXT,
    fiscal_year     TEXT,
    -- Income statement
    revenue         REAL,
    gross_profit    REAL,
    op_income       REAL,
    net_income      REAL,
    ebitda          REAL,
    rd_expense      REAL,
    sga_expense     REAL,
    interest_exp    REAL,
    tax_expense     REAL,
    pretax_income   REAL,
    da              REAL,
    eps_diluted     REAL,
    shares_out      REAL,
    -- Balance sheet
    total_assets    REAL,
    current_assets  REAL,
    cash            REAL,
    receivables     REAL,
    inventory       REAL,
    total_liab      REAL,
    current_liab    REAL,
    lt_debt         REAL,
    st_debt         REAL,
    equity          REAL,
    retained_earn   REAL,
    goodwill        REAL,
    intangibles     REAL,
    ppe             REAL,
    -- Cash flow
    cfo             REAL,
    capex           REAL,
    fcf             REAL,
    dividends       REAL,
    buybacks        REAL,
    sbc             REAL,
    -- Computed metrics
    gross_margin    REAL,
    op_margin       REAL,
    net_margin      REAL,
    roe             REAL,
    roa             REAL,
    roic            REAL,
    de_ratio        REAL,
    current_ratio   REAL,
    fcf_yield       REAL,
    revenue_growth  REAL,
    earnings_growth REAL,
    PRIMARY KEY (ticker, fiscal_year)
);

CREATE TABLE IF NOT EXISTS computed_metrics (
    ticker              TEXT PRIMARY KEY,
    revenue_cagr_3y     REAL,
    revenue_cagr_5y     REAL,
    earnings_cagr_3y    REAL,
    earnings_cagr_5y    REAL,
    avg_roic_5y         REAL,
    avg_roe_5y          REAL,
    avg_gross_margin_5y REAL,
    avg_op_margin_5y    REAL,
    piotroski_score     INTEGER,
    altman_z            REAL,
    altman_zone         TEXT,
    moat_score          INTEGER,
    moat_width          TEXT,
    quality_score       REAL,
    latest_fcf          REAL,
    latest_revenue      REAL,
    latest_net_income   REAL,
    latest_equity       REAL,
    latest_total_debt   REAL,
    dcf_fair_value      REAL,
    implied_growth      REAL,
    last_computed       TEXT
);

CREATE TABLE IF NOT EXISTS research_notes (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ticker          TEXT,
    source          TEXT,    -- 'jina_reader', 'jina_search', 'parallel_research', 'edgar_filing'
    source_url      TEXT,
    title           TEXT,
    content         TEXT,
    content_hash    TEXT,
    created_at      TEXT,
    tokens_approx   INTEGER
);

CREATE TABLE IF NOT EXISTS embeddings (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ticker          TEXT,
    source          TEXT,    -- 'description', 'financials', 'research', 'filing'
    text_preview    TEXT,    -- first 200 chars
    embedding       BLOB,   -- packed float32 vector
    dim             INTEGER,
    model           TEXT,
    created_at      TEXT
);

CREATE TABLE IF NOT EXISTS research_jobs (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ticker          TEXT,
    provider        TEXT,    -- 'parallel', 'jina'
    job_id          TEXT,    -- external job ID
    status          TEXT,    -- 'pending', 'running', 'completed', 'failed'
    query           TEXT,
    result          TEXT,
    created_at      TEXT,
    completed_at    TEXT
);

CREATE TABLE IF NOT EXISTS scrape_sources (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    name            TEXT UNIQUE,
    url_template    TEXT,    -- {TICKER} placeholder
    data_type       TEXT,    -- 'quote', 'financials', 'profile', 'holders', 'news'
    parser          TEXT,    -- parser function name
    success_count   INTEGER DEFAULT 0,
    fail_count      INTEGER DEFAULT 0,
    avg_latency_ms  REAL DEFAULT 0,
    last_success    TEXT,
    last_failure    TEXT,
    enabled         INTEGER DEFAULT 1
);

CREATE TABLE IF NOT EXISTS av_cache (
    ticker          TEXT,
    function        TEXT,    -- 'OVERVIEW', 'GLOBAL_QUOTE', 'INCOME_STATEMENT', etc.
    data            TEXT,    -- raw JSON response
    fetched_at      TEXT,
    PRIMARY KEY (ticker, function)
);

CREATE INDEX IF NOT EXISTS idx_af_ticker ON annual_financials(ticker);
CREATE INDEX IF NOT EXISTS idx_rn_ticker ON research_notes(ticker);
CREATE INDEX IF NOT EXISTS idx_emb_ticker ON embeddings(ticker);
CREATE INDEX IF NOT EXISTS idx_rj_ticker ON research_jobs(ticker);
CREATE INDEX IF NOT EXISTS idx_rj_status ON research_jobs(status);
CREATE INDEX IF NOT EXISTS idx_avc_ticker ON av_cache(ticker);
"""

def get_db():
    Path(DB_PATH).parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.executescript(SCHEMA)
    return conn

_db_lock = Lock()

def db_execute(conn, sql, params=(), many=False):
    with _db_lock:
        cur = conn.cursor()
        if many:
            cur.executemany(sql, params)
        else:
            cur.execute(sql, params)
        conn.commit()
        return cur

def db_query(conn, sql, params=()):
    with _db_lock:
        return conn.execute(sql, params).fetchall()

def db_one(conn, sql, params=()):
    with _db_lock:
        return conn.execute(sql, params).fetchone()

# ══════════════════════════════════════════════════════════════════════════════
# SEC EDGAR
# ══════════════════════════════════════════════════════════════════════════════

def edgar_load_tickers():
    """Load full SEC ticker→CIK mapping."""
    req = Request(EDGAR_TICKERS_URL, headers={"User-Agent": EDGAR_UA})
    data = json.loads(urlopen(req, timeout=20).read().decode())
    tickers = {}
    for entry in data.values():
        t = entry.get("ticker", "").upper()
        cik = str(entry.get("cik_str", ""))
        name = entry.get("title", "")
        if t and cik:
            tickers[t] = {"cik": cik, "name": name}
    return tickers

_edgar_sem = Semaphore(8)  # max concurrent EDGAR requests

def edgar_fetch_facts(cik):
    """Fetch all XBRL facts for a CIK."""
    padded = str(cik).zfill(10)
    url = f"{EDGAR_FACTS_URL}/CIK{padded}.json"
    with _edgar_sem:
        try:
            req = Request(url, headers={"User-Agent": EDGAR_UA})
            data = json.loads(urlopen(req, timeout=20).read().decode())
            return data
        except Exception:
            return None

def edgar_annual_values(facts, concept, unit="USD"):
    """Extract deduped annual values for a GAAP concept."""
    ug = facts.get("facts", {}).get("us-gaap", {})
    node = ug.get(concept)
    if not node: return []
    entries = node.get("units", {}).get(unit, [])
    annual = [e for e in entries if e.get("form") == "10-K" and e.get("fp") == "FY"]
    by_end = {}
    for e in annual:
        end = e.get("end", "")
        if not end: continue
        existing = by_end.get(end)
        if not existing or e.get("filed", "") > existing.get("filed", ""):
            by_end[end] = e
    return sorted(by_end.values(), key=lambda e: e.get("end", ""))

# GAAP concept mapping
CONCEPT_MAP = {
    # Income
    "revenue":      ["RevenueFromContractWithCustomerExcludingAssessedTax", "Revenues", "SalesRevenueNet"],
    "gross_profit": ["GrossProfit"],
    "op_income":    ["OperatingIncomeLoss"],
    "net_income":   ["NetIncomeLoss"],
    "rd_expense":   ["ResearchAndDevelopmentExpense"],
    "sga_expense":  ["SellingGeneralAndAdministrativeExpense"],
    "interest_exp": ["InterestExpense", "InterestExpenseDebt"],
    "tax_expense":  ["IncomeTaxExpenseBenefit"],
    "pretax_income":["IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest"],
    "da":           ["DepreciationDepletionAndAmortization", "DepreciationAndAmortization"],
    "eps_diluted":  ["EarningsPerShareDiluted"],
    "shares_out":   ["CommonStockSharesOutstanding"],
    # Balance sheet
    "total_assets":  ["Assets"],
    "current_assets":["AssetsCurrent"],
    "cash":          ["CashAndCashEquivalentsAtCarryingValue", "CashCashEquivalentsAndShortTermInvestments"],
    "receivables":   ["AccountsReceivableNetCurrent"],
    "inventory":     ["InventoryNet"],
    "total_liab":    ["Liabilities"],
    "current_liab":  ["LiabilitiesCurrent"],
    "lt_debt":       ["LongTermDebtNoncurrent", "LongTermDebt"],
    "st_debt":       ["ShortTermBorrowings", "LongTermDebtCurrent", "CommercialPaper"],
    "equity":        ["StockholdersEquity"],
    "retained_earn": ["RetainedEarningsAccumulatedDeficit"],
    "goodwill":      ["Goodwill"],
    "intangibles":   ["IntangibleAssetsNetExcludingGoodwill"],
    "ppe":           ["PropertyPlantAndEquipmentNet"],
    # Cash flow
    "cfo":           ["NetCashProvidedByUsedInOperatingActivities"],
    "capex":         ["PaymentsToAcquirePropertyPlantAndEquipment"],
    "dividends":     ["PaymentsOfDividends", "PaymentsOfDividendsCommonStock"],
    "buybacks":      ["PaymentsForRepurchaseOfCommonStock"],
    "sbc":           ["ShareBasedCompensation", "AllocatedShareBasedCompensationExpense"],
}

# ══════════════════════════════════════════════════════════════════════════════
# NASDAQ FTP ticker universe
# ══════════════════════════════════════════════════════════════════════════════

NASDAQ_LISTED_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt"
NASDAQ_OTHER_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt"

def load_nasdaq_universe():
    """Load all common stocks from NASDAQ FTP (nasdaqlisted + otherlisted)."""
    tickers = {}

    # NASDAQ-listed
    try:
        data = urlopen(Request(NASDAQ_LISTED_URL, headers={"User-Agent": EDGAR_UA}),
                       timeout=15).read().decode().replace("\r", "")
        for line in data.strip().split("\n")[1:]:
            parts = line.split("|")
            if len(parts) < 8: continue
            sym, name, mkt_cat, test, fin_status, lot, etf, nextshares = parts[:8]
            if test != "N" or etf != "N": continue
            # Skip rights (R), warrants (W), units (U) — but keep short tickers
            if len(sym) > 4 and sym[-1] in ("R", "W", "U"): continue
            tickers[sym] = {"name": name, "exchange": "NASDAQ", "mkt_cat": mkt_cat}
    except Exception as e:
        sprint(f"    {red(f'NASDAQ list error: {e}')}")

    # Other listed (NYSE, AMEX, ARCA, BATS)
    exchange_map = {"N": "NYSE", "A": "NYSE MKT", "P": "NYSE ARCA", "Z": "BATS", "V": "IEX"}
    try:
        data = urlopen(Request(NASDAQ_OTHER_URL, headers={"User-Agent": EDGAR_UA}),
                       timeout=15).read().decode().replace("\r", "")
        for line in data.strip().split("\n")[1:]:
            parts = line.split("|")
            if len(parts) < 8: continue
            sym, name, exchange, cqs, etf, lot, test, nsym = parts[:8]
            if test != "N" or etf != "N": continue
            if len(sym) > 5 and sym[-1] in ("R", "W", "U"): continue
            exch_name = exchange_map.get(exchange, exchange)
            tickers[sym] = {"name": name, "exchange": exch_name, "mkt_cat": ""}
    except Exception as e:
        sprint(f"    {red(f'Other-listed error: {e}')}")

    return tickers


# ══════════════════════════════════════════════════════════════════════════════
# DCF valuation engine
# ══════════════════════════════════════════════════════════════════════════════

TERMINAL_GROWTH = 0.025
MARKET_RISK_PREMIUM = 0.055
RISK_FREE_RATE = 0.043  # updated from treasury if available

def compute_dcf(rows, shares_out, risk_free=RISK_FREE_RATE):
    """Run a multi-stage DCF from historical financials. Returns fair value per share."""
    if len(rows) < 3 or not shares_out or shares_out <= 0:
        return None, None

    latest = rows[-1]
    prev = rows[-2]

    # Base FCF
    base_fcf = latest.get("fcf")
    if not base_fcf or base_fcf <= 0:
        return None, None

    # WACC estimation
    beta = 1.0  # default; could be enriched later
    ke = risk_free + beta * MARKET_RISK_PREMIUM

    # Cost of debt from financials
    interest = abs(latest.get("interest_exp") or 0)
    total_debt = (latest.get("lt_debt") or 0) + (latest.get("st_debt") or 0)
    kd = min(interest / total_debt, 0.15) if total_debt > 0 else risk_free + 0.02

    # Tax rate
    pt = latest.get("pretax_income") or 0
    tx = latest.get("tax_expense") or 0
    tax_rate = min(tx / pt, 0.40) if pt > 0 and tx > 0 else 0.21

    # Capital structure from book values (market cap not available)
    equity_val = (latest.get("equity") or 0) * 3  # rough P/B proxy
    if equity_val <= 0:
        equity_val = base_fcf * 20  # rough P/FCF proxy
    total_cap = equity_val + total_debt
    if total_cap <= 0:
        total_cap = 1
    we = equity_val / total_cap
    wd = total_debt / total_cap
    wacc = we * ke + wd * kd * (1 - tax_rate)
    wacc = max(wacc, 0.05)  # floor at 5%

    # Growth estimation from revenue CAGR
    rev_vals = [(r.get("revenue") or 0) for r in rows if r.get("revenue") and r["revenue"] > 0]
    if len(rev_vals) >= 3:
        n = min(len(rev_vals), 5)
        rev_cagr = (rev_vals[-1] / rev_vals[-n]) ** (1.0 / (n - 1)) - 1
    else:
        rev_cagr = 0.05

    growth = max(min(rev_cagr, 0.30), -0.05)

    # 10-year DCF with fade
    pv_sum = 0
    last_fcf = base_fcf
    for yr in range(1, 11):
        if yr <= 5:
            g = growth
        else:
            fade = (yr - 5) / 5.0
            g = growth + (TERMINAL_GROWTH - growth) * fade
        last_fcf = last_fcf * (1 + g)
        pv_sum += last_fcf / (1 + wacc) ** yr

    # Terminal value
    tv = last_fcf * (1 + TERMINAL_GROWTH) / (wacc - TERMINAL_GROWTH) if wacc > TERMINAL_GROWTH else last_fcf * 25
    pv_tv = tv / (1 + wacc) ** 10

    ev = pv_sum + pv_tv
    net_debt = total_debt - (latest.get("cash") or 0)
    equity_value = ev - net_debt
    fair_value = equity_value / shares_out

    # Implied growth (reverse DCF at current book value)
    return max(fair_value, 0), growth


def edgar_extract_financials(facts):
    """Extract all annual financial data from EDGAR facts into year-keyed dicts."""
    all_years = set()
    for field, concepts in CONCEPT_MAP.items():
        for concept in concepts:
            for e in edgar_annual_values(facts, concept, "USD"):
                all_years.add(e["end"][:4])
            # Also check shares unit
            if field in ("shares_out", "eps_diluted"):
                for e in edgar_annual_values(facts, concept, "shares"):
                    all_years.add(e["end"][:4])

    rows = []
    prev_row = None
    for year in sorted(all_years):
        row = {"fiscal_year": year}
        for field, concepts in CONCEPT_MAP.items():
            for concept in concepts:
                unit = "shares" if field in ("shares_out", "eps_diluted") else "USD"
                for e in edgar_annual_values(facts, concept, unit):
                    if e["end"][:4] == year:
                        row[field] = e["val"]
                        break
                if field in row:
                    break

        # Compute derived metrics
        rev = row.get("revenue")
        ni = row.get("net_income")
        gp = row.get("gross_profit")
        op = row.get("op_income")
        ta = row.get("total_assets")
        eq = row.get("equity")
        ca = row.get("current_assets")
        cl = row.get("current_liab")
        cfo = row.get("cfo")
        cx = row.get("capex")
        da_val = row.get("da")
        ltd = row.get("lt_debt") or 0
        std = row.get("st_debt") or 0

        if rev and rev > 0:
            if gp is not None: row["gross_margin"] = gp / rev
            if op is not None: row["op_margin"] = op / rev
            if ni is not None: row["net_margin"] = ni / rev
        if eq and eq > 0 and ni is not None:
            row["roe"] = ni / eq
        if ta and ta > 0 and ni is not None:
            row["roa"] = ni / ta

        # ROIC = NOPAT / Invested Capital
        if op is not None and eq is not None:
            tax_rate = 0.21
            pt = row.get("pretax_income")
            tx = row.get("tax_expense")
            if pt and pt > 0 and tx and tx > 0:
                tax_rate = min(tx / pt, 0.40)
            nopat = op * (1 - tax_rate)
            ic = eq + ltd + std - (row.get("cash") or 0)
            if ic > 0:
                row["roic"] = nopat / ic

        # D/E
        total_debt = ltd + std
        if eq and eq > 0:
            row["de_ratio"] = total_debt / eq

        # Current ratio
        if cl and cl > 0 and ca:
            row["current_ratio"] = ca / cl

        # FCF
        if cfo is not None:
            capex_val = abs(cx) if cx else 0
            row["fcf"] = cfo - capex_val

        # EBITDA
        if op is not None:
            da_v = abs(da_val) if da_val else 0
            row["ebitda"] = op + da_v

        # Revenue & earnings growth
        if prev_row and prev_row.get("revenue") and prev_row["revenue"] > 0 and rev:
            row["revenue_growth"] = (rev - prev_row["revenue"]) / abs(prev_row["revenue"])
        if prev_row and prev_row.get("net_income") and prev_row["net_income"] != 0 and ni:
            row["earnings_growth"] = (ni - prev_row["net_income"]) / abs(prev_row["net_income"])

        rows.append(row)
        prev_row = row

    return rows

def compute_aggregate_metrics(rows, ticker):
    """Compute CAGRs, scores, averages from annual rows."""
    if len(rows) < 2:
        return {}

    m = {"ticker": ticker}

    def _cagr(field, n_years):
        recent = [r for r in rows if r.get(field) and r[field] > 0]
        if len(recent) < 2: return None
        recent = recent[-min(len(recent), n_years+1):]
        if len(recent) < 2: return None
        first, last = recent[0][field], recent[-1][field]
        years = len(recent) - 1
        if first <= 0 or years <= 0: return None
        return (last / first) ** (1.0 / years) - 1

    m["revenue_cagr_3y"] = _cagr("revenue", 3)
    m["revenue_cagr_5y"] = _cagr("revenue", 5)
    m["earnings_cagr_3y"] = _cagr("net_income", 3)
    m["earnings_cagr_5y"] = _cagr("net_income", 5)

    def _avg(field, n):
        vals = [r[field] for r in rows[-n:] if r.get(field) is not None]
        return sum(vals) / len(vals) if vals else None

    m["avg_roic_5y"] = _avg("roic", 5)
    m["avg_roe_5y"] = _avg("roe", 5)
    m["avg_gross_margin_5y"] = _avg("gross_margin", 5)
    m["avg_op_margin_5y"] = _avg("op_margin", 5)

    # Latest values
    latest = rows[-1] if rows else {}
    m["latest_fcf"] = latest.get("fcf")
    m["latest_revenue"] = latest.get("revenue")
    m["latest_net_income"] = latest.get("net_income")
    m["latest_equity"] = latest.get("equity")
    m["latest_total_debt"] = (latest.get("lt_debt") or 0) + (latest.get("st_debt") or 0)

    # Piotroski F-Score
    if len(rows) >= 2:
        cy, py = rows[-1], rows[-2]
        f = 0
        if (cy.get("net_income") or 0) > 0: f += 1
        if (cy.get("cfo") or 0) > 0: f += 1
        if cy.get("roa") and py.get("roa") and cy["roa"] > py["roa"]: f += 1
        if (cy.get("cfo") or 0) > (cy.get("net_income") or 0): f += 1
        cy_lev = cy.get("de_ratio") or 0
        py_lev = py.get("de_ratio") or 0
        if cy_lev < py_lev: f += 1
        if cy.get("current_ratio") and py.get("current_ratio") and cy["current_ratio"] > py["current_ratio"]: f += 1
        if cy.get("gross_margin") and py.get("gross_margin") and cy["gross_margin"] > py["gross_margin"]: f += 1
        cy_at = (cy.get("revenue") or 0) / (cy.get("total_assets") or 1)
        py_at = (py.get("revenue") or 0) / (py.get("total_assets") or 1)
        if cy_at > py_at: f += 1
        # Share dilution check simplified
        f += 1
        m["piotroski_score"] = f

    # Altman Z-Score
    if latest.get("total_assets") and latest["total_assets"] > 0:
        ta = latest["total_assets"]
        ca = latest.get("current_assets") or 0
        cl = latest.get("current_liab") or 0
        re = latest.get("retained_earn") or 0
        ebit = latest.get("op_income") or 0
        rev = latest.get("revenue") or 0
        eq = latest.get("equity") or 0
        tl = latest.get("total_liab") or 1
        z = 1.2*((ca-cl)/ta) + 1.4*(re/ta) + 3.3*(ebit/ta) + 0.6*(eq/tl) + 1.0*(rev/ta)
        m["altman_z"] = z
        m["altman_zone"] = "Safe" if z > 2.99 else "Grey" if z > 1.81 else "Distress"

    # Moat score (simplified)
    moat = 0
    avg_gm = m.get("avg_gross_margin_5y")
    if avg_gm and avg_gm > 0.60: moat += 20
    elif avg_gm and avg_gm > 0.40: moat += 10
    avg_roic = m.get("avg_roic_5y")
    if avg_roic and avg_roic > 0.20: moat += 20
    elif avg_roic and avg_roic > 0.10: moat += 5
    rc = m.get("revenue_cagr_5y")
    if rc and rc > 0.08: moat += 10
    if latest.get("ppe") and latest.get("total_assets") and latest["total_assets"] > 0:
        if latest["ppe"] / latest["total_assets"] < 0.15: moat += 10
    rd = latest.get("rd_expense")
    rev = latest.get("revenue")
    if rd and rev and rev > 0 and rd / rev > 0.05: moat += 5
    m["moat_score"] = moat
    m["moat_width"] = "Wide" if moat >= 50 else "Narrow" if moat >= 25 else "None"

    # Quality composite
    q = 0
    if m.get("piotroski_score") and m["piotroski_score"] >= 6: q += 25
    if m.get("altman_zone") == "Safe": q += 25
    elif m.get("altman_zone") == "Grey": q += 12
    if avg_roic and avg_roic > 0.15: q += 25
    if latest.get("fcf") and latest["fcf"] > 0: q += 25
    m["quality_score"] = q

    # DCF valuation
    shares = latest.get("shares_out")
    if shares and shares > 0:
        dcf_fv, impl_g = compute_dcf(rows, shares)
        m["dcf_fair_value"] = dcf_fv
        m["implied_growth"] = impl_g

    m["last_computed"] = datetime.utcnow().isoformat()
    return m

# ══════════════════════════════════════════════════════════════════════════════
# Jina integration
# ══════════════════════════════════════════════════════════════════════════════

_jina_sem = Semaphore(5)

def jina_read(url, timeout=20):
    """Extract clean markdown content from a URL via Jina Reader."""
    if not JINA_KEY: return None
    with _jina_sem:
        try:
            req = Request(f"{JINA_READER}/{url}", headers={
                "Authorization": f"Bearer {JINA_KEY}",
                "Accept": "application/json",
                "X-Return-Format": "markdown",
                "X-Timeout": str(timeout),
            })
            resp = json.loads(urlopen(req, timeout=timeout+5).read().decode())
            data = resp.get("data", {})
            return {"title": data.get("title", ""), "content": data.get("content", ""),
                    "url": data.get("url", url)}
        except Exception as e:
            return None

def jina_search(query, num=5):
    """Search the web via Jina Search API."""
    if not JINA_KEY: return []
    with _jina_sem:
        try:
            body = json.dumps({"q": query, "num": num}).encode()
            req = Request(JINA_SEARCH, data=body, headers={
                "Authorization": f"Bearer {JINA_KEY}",
                "Content-Type": "application/json",
                "Accept": "application/json",
            })
            resp = json.loads(urlopen(req, timeout=30).read().decode())
            results = resp.get("data", [])
            return [{"title": r.get("title", ""), "url": r.get("url", ""),
                      "content": r.get("content", "")[:5000]} for r in results]
        except Exception:
            return []

def jina_embed(texts, task="retrieval.passage"):
    """Generate embeddings via Jina Embeddings API."""
    if not JINA_KEY: return []
    try:
        body = json.dumps({
            "model": EMBED_MODEL,
            "input": texts if isinstance(texts, list) else [texts],
            "task": task,
            "dimensions": EMBED_DIM,
            "normalized": True,
            "embedding_type": "float",
        }).encode()
        req = Request(JINA_EMBED, data=body, headers={
            "Authorization": f"Bearer {JINA_KEY}",
            "Content-Type": "application/json",
        })
        resp = json.loads(urlopen(req, timeout=30).read().decode())
        return [item["embedding"] for item in resp.get("data", [])]
    except Exception:
        return []

def pack_embedding(vec):
    """Pack float list to bytes."""
    return struct.pack(f"{len(vec)}f", *vec)

def unpack_embedding(blob, dim=EMBED_DIM):
    """Unpack bytes to float list."""
    return list(struct.unpack(f"{dim}f", blob))

def cosine_sim(a, b):
    """Cosine similarity between two vectors."""
    dot = sum(x * y for x, y in zip(a, b))
    na = sum(x * x for x in a) ** 0.5
    nb = sum(x * x for x in b) ** 0.5
    return dot / (na * nb) if na > 0 and nb > 0 else 0

# ══════════════════════════════════════════════════════════════════════════════
# Parallel.ai integration
# ══════════════════════════════════════════════════════════════════════════════

def parallel_search(query, num=5):
    """Search via Parallel.ai API."""
    if not PARALLEL_KEY: return []
    try:
        body = json.dumps({
            "query": query,
            "max_results": num,
        }).encode()
        req = Request(PARALLEL_SEARCH, data=body, headers={
            "x-api-key": PARALLEL_KEY,
            "Content-Type": "application/json",
        })
        resp = json.loads(urlopen(req, timeout=30).read().decode())
        results = resp.get("results", [])
        return [{"title": r.get("title", ""), "url": r.get("url", ""),
                  "content": r.get("snippet", r.get("content", ""))[:3000]} for r in results]
    except Exception:
        return []

def parallel_extract(url):
    """Extract content from URL via Parallel.ai."""
    if not PARALLEL_KEY: return None
    try:
        body = json.dumps({"url": url}).encode()
        req = Request(PARALLEL_EXTRACT, data=body, headers={
            "x-api-key": PARALLEL_KEY,
            "Content-Type": "application/json",
        })
        resp = json.loads(urlopen(req, timeout=30).read().decode())
        return resp.get("content", resp.get("text", ""))
    except Exception:
        return None

def parallel_deep_research(query, ticker=""):
    """Launch async deep research task via Parallel.ai Tasks API."""
    if not PARALLEL_KEY: return None
    try:
        body = json.dumps({
            "query": query,
            "model": "research",
        }).encode()
        req = Request(PARALLEL_TASKS, data=body, headers={
            "x-api-key": PARALLEL_KEY,
            "Content-Type": "application/json",
        })
        resp = json.loads(urlopen(req, timeout=60).read().decode())
        return {
            "job_id": resp.get("id", resp.get("run_id", "")),
            "status": resp.get("status", "submitted"),
            "result": resp.get("result", resp.get("output", "")),
        }
    except Exception as e:
        return {"job_id": "", "status": "failed", "result": str(e)}

def parallel_check_job(job_id):
    """Poll a Parallel.ai research job."""
    if not PARALLEL_KEY or not job_id: return None
    try:
        req = Request(f"{PARALLEL_TASKS}/{job_id}", headers={
            "x-api-key": PARALLEL_KEY,
        })
        resp = json.loads(urlopen(req, timeout=15).read().decode())
        return {
            "status": resp.get("status", "unknown"),
            "result": resp.get("result", resp.get("output", "")),
        }
    except Exception:
        return None

# ══════════════════════════════════════════════════════════════════════════════
# SEC EDGAR — Submissions, Insider Trades, 13F, SIC codes
# ══════════════════════════════════════════════════════════════════════════════

EDGAR_SUBMISSIONS_URL = "https://data.sec.gov/submissions"

def edgar_fetch_submissions(cik):
    """Fetch company submissions (filings list, SIC, addresses)."""
    padded = str(cik).zfill(10)
    url = f"{EDGAR_SUBMISSIONS_URL}/CIK{padded}.json"
    with _edgar_sem:
        try:
            req = Request(url, headers={"User-Agent": EDGAR_UA})
            return json.loads(urlopen(req, timeout=20).read().decode())
        except Exception:
            return None

def edgar_get_company_info(cik):
    """Extract SIC code, state, fiscal year end from submissions."""
    data = edgar_fetch_submissions(cik)
    if not data: return {}
    return {
        "sic_code": data.get("sic", ""),
        "sic_desc": data.get("sicDescription", ""),
        "state": data.get("stateOfIncorporation", ""),
        "fiscal_year_end": data.get("fiscalYearEnd", ""),
        "ein": data.get("ein", ""),
        "category": data.get("category", ""),
        "entity_type": data.get("entityType", ""),
        "phone": (data.get("addresses", {}).get("business", {}) or {}).get("phone", ""),
    }

def edgar_get_insider_trades(cik, limit=20):
    """Get recent insider trades (Form 4) from submissions."""
    data = edgar_fetch_submissions(cik)
    if not data: return []
    recent = data.get("filings", {}).get("recent", {})
    if not recent: return []

    forms = recent.get("form", [])
    dates = recent.get("filingDate", [])
    docs = recent.get("primaryDocument", [])
    accessions = recent.get("accessionNumber", [])

    trades = []
    for i, form in enumerate(forms):
        if form in ("4", "3", "5") and i < len(dates):
            trades.append({
                "form": form,
                "date": dates[i] if i < len(dates) else "",
                "accession": accessions[i] if i < len(accessions) else "",
                "document": docs[i] if i < len(docs) else "",
            })
            if len(trades) >= limit:
                break
    return trades

def edgar_get_13f_filings(cik, limit=5):
    """Get recent 13F filing accessions."""
    data = edgar_fetch_submissions(cik)
    if not data: return []
    recent = data.get("filings", {}).get("recent", {})
    if not recent: return []

    forms = recent.get("form", [])
    dates = recent.get("filingDate", [])
    accessions = recent.get("accessionNumber", [])

    filings = []
    for i, form in enumerate(forms):
        if form in ("13F-HR", "13F-HR/A"):
            filings.append({
                "form": form,
                "date": dates[i] if i < len(dates) else "",
                "accession": accessions[i] if i < len(accessions) else "",
            })
            if len(filings) >= limit:
                break
    return filings

# ══════════════════════════════════════════════════════════════════════════════
# FRED — Federal Reserve Economic Data (macro indicators)
# ══════════════════════════════════════════════════════════════════════════════

FRED_KEY = os.environ.get("FRED_API_KEY", "")
FRED_BASE = "https://api.stlouisfed.org/fred"

FRED_SERIES = {
    "gdp":            "GDP",
    "real_gdp":       "GDPC1",
    "cpi":            "CPIAUCSL",
    "unemployment":   "UNRATE",
    "fed_funds":      "FEDFUNDS",
    "treasury_10y":   "DGS10",
    "treasury_2y":    "DGS2",
    "yield_spread":   "T10Y2Y",
    "vix":            "VIXCLS",
    "sp500":          "SP500",
    "m2_money":       "M2SL",
    "mortgage_30y":   "MORTGAGE30US",
    "oil_wti":        "DCOILWTICO",
    "consumer_sent":  "UMCSENT",
    "housing_starts": "HOUST",
}

def fred_fetch(series_id, start_date=None, limit=60):
    """Fetch FRED time series data."""
    if not FRED_KEY: return []
    params = f"series_id={series_id}&api_key={FRED_KEY}&file_type=json&sort_order=desc&limit={limit}"
    if start_date:
        params += f"&observation_start={start_date}"
    url = f"{FRED_BASE}/series/observations?{params}"
    try:
        req = Request(url, headers={"User-Agent": EDGAR_UA})
        data = json.loads(urlopen(req, timeout=15).read().decode())
        return [{"date": o["date"], "value": float(o["value"])}
                for o in data.get("observations", [])
                if o.get("value") and o["value"] != "."]
    except Exception:
        return []

def fred_fetch_all_macro(conn):
    """Fetch all macro indicators and cache in av_cache table."""
    if not FRED_KEY:
        sprint(f"    {yellow('No FRED_API_KEY — skipping macro data')}")
        return {}
    results = {}
    for name, series_id in FRED_SERIES.items():
        # Check cache (1 day for daily, 7 for others)
        cache_days = 1 if series_id.startswith("D") else 7
        cutoff = (datetime.utcnow() - timedelta(days=cache_days)).isoformat()
        cached = db_one(conn, "SELECT data FROM av_cache WHERE ticker = ? AND function = ? AND fetched_at > ?",
                        ("_MACRO_", f"FRED_{series_id}", cutoff))
        if cached:
            try:
                results[name] = json.loads(cached["data"])
                continue
            except Exception:
                pass

        obs = fred_fetch(series_id, limit=30)
        if obs:
            results[name] = obs
            db_execute(conn, "INSERT OR REPLACE INTO av_cache (ticker, function, data, fetched_at) VALUES (?, ?, ?, ?)",
                       ("_MACRO_", f"FRED_{series_id}", json.dumps(obs), datetime.utcnow().isoformat()))
            time.sleep(0.1)

    return results

# ══════════════════════════════════════════════════════════════════════════════
# Yahoo Finance — free quotes, no API key needed
# ══════════════════════════════════════════════════════════════════════════════

YAHOO_QUOTE_URL = "https://query1.finance.yahoo.com/v7/finance/quote"
YAHOO_CHART_URL = "https://query2.finance.yahoo.com/v8/finance/chart"
_yahoo_sem = Semaphore(3)

def yahoo_batch_quote(tickers, conn=None):
    """Fetch prices via Yahoo chart endpoint + multi-source scraping fallbacks."""
    results = {}
    for sym in tickers:
        with _yahoo_sem:
            try:
                url = f"{YAHOO_CHART_URL}/{sym}?interval=1d&range=5d"
                req = Request(url, headers={
                    "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36",
                })
                data = json.loads(urlopen(req, timeout=10).read().decode())
                chart = data.get("chart", {}).get("result", [{}])[0]
                meta = chart.get("meta", {})
                price = meta.get("regularMarketPrice")
                if price:
                    q = {
                        "price": price,
                        "prev_close": meta.get("previousClose") or meta.get("chartPreviousClose"),
                        "52w_high": meta.get("fiftyTwoWeekHigh"),
                        "52w_low": meta.get("fiftyTwoWeekLow"),
                        "50dma": meta.get("fiftyDayAverage"),
                        "200dma": meta.get("twoHundredDayAverage"),
                        "exchange": meta.get("exchangeName", ""),
                        "currency": meta.get("currency", "USD"),
                    }
                    results[sym] = q
                    if conn:
                        db_execute(conn, "INSERT OR REPLACE INTO av_cache (ticker, function, data, fetched_at) VALUES (?, ?, ?, ?)",
                                   (sym, "YAHOO_QUOTE", json.dumps(q), datetime.utcnow().isoformat()))
            except Exception:
                pass
            time.sleep(0.05)
    # Fallback: try Stooq CSV for any that failed
    missing = [t for t in tickers if t not in results]
    for sym in missing[:20]:
        try:
            url = f"https://stooq.com/q/l/?s={sym}.us&f=sd2t2ohlcv&h&e=csv"
            raw = urlopen(Request(url, headers={"User-Agent": EDGAR_UA}), timeout=8).read().decode()
            lines = raw.strip().split("\n")
            if len(lines) >= 2:
                parts = lines[1].split(",")
                if len(parts) >= 7 and parts[5] != "N/D":
                    price = float(parts[5])
                    q = {"price": price, "source": "stooq"}
                    results[sym] = q
                    if conn:
                        db_execute(conn, "INSERT OR REPLACE INTO av_cache (ticker, function, data, fetched_at) VALUES (?, ?, ?, ?)",
                                   (sym, "STOOQ_QUOTE", json.dumps(q), datetime.utcnow().isoformat()))
        except Exception:
            pass
        time.sleep(0.1)
    return results

def yahoo_historical(ticker, period="1y", interval="1d"):
    """Fetch historical price data from Yahoo Finance."""
    import calendar
    from datetime import datetime as dt
    end = int(calendar.timegm(dt.utcnow().timetuple()))
    if period == "1y": start = end - 365 * 86400
    elif period == "5y": start = end - 5 * 365 * 86400
    else: start = end - 365 * 86400

    url = f"{YAHOO_CHART_URL}/{ticker}?period1={start}&period2={end}&interval={interval}"
    try:
        req = Request(url, headers={
            "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)",
        })
        data = json.loads(urlopen(req, timeout=15).read().decode())
        result = data.get("chart", {}).get("result", [{}])[0]
        timestamps = result.get("timestamp", [])
        ohlcv = result.get("indicators", {}).get("quote", [{}])[0]
        adj = result.get("indicators", {}).get("adjclose", [{}])[0]
        return [{
            "date": datetime.utcfromtimestamp(t).strftime("%Y-%m-%d"),
            "open": ohlcv.get("open", [None])[i],
            "high": ohlcv.get("high", [None])[i],
            "low": ohlcv.get("low", [None])[i],
            "close": ohlcv.get("close", [None])[i],
            "volume": ohlcv.get("volume", [None])[i],
            "adj_close": adj.get("adjclose", [None])[i] if adj else None,
        } for i, t in enumerate(timestamps)]
    except Exception:
        return []

# ══════════════════════════════════════════════════════════════════════════════
# Treasury.gov — fiscal data, debt, auction data
# ══════════════════════════════════════════════════════════════════════════════

TREASURY_BASE = "https://api.fiscaldata.treasury.gov/services/api/fiscal_service"

def treasury_fetch(endpoint, params="", limit=10):
    """Fetch from Treasury Fiscal Data API (no auth needed)."""
    url = f"{TREASURY_BASE}/{endpoint}?sort=-record_date&page[size]={limit}&format=json"
    if params:
        url += f"&{params}"
    try:
        req = Request(url, headers={"User-Agent": EDGAR_UA})
        data = json.loads(urlopen(req, timeout=15).read().decode())
        return data.get("data", [])
    except Exception:
        return []

def treasury_get_macro(conn):
    """Fetch key Treasury data points and cache."""
    results = {}

    # National debt
    cached = db_one(conn, "SELECT data FROM av_cache WHERE ticker = '_MACRO_' AND function = 'TREASURY_DEBT' AND fetched_at > ?",
                    ((datetime.utcnow() - timedelta(days=1)).isoformat(),))
    if cached:
        results["national_debt"] = json.loads(cached["data"])
    else:
        debt = treasury_fetch("v2/accounting/od/debt_to_penny", limit=5)
        if debt:
            results["national_debt"] = debt
            db_execute(conn, "INSERT OR REPLACE INTO av_cache (ticker, function, data, fetched_at) VALUES (?, ?, ?, ?)",
                       ("_MACRO_", "TREASURY_DEBT", json.dumps(debt), datetime.utcnow().isoformat()))

    # Average interest rates on debt
    cached = db_one(conn, "SELECT data FROM av_cache WHERE ticker = '_MACRO_' AND function = 'TREASURY_RATES' AND fetched_at > ?",
                    ((datetime.utcnow() - timedelta(days=1)).isoformat(),))
    if cached:
        results["avg_rates"] = json.loads(cached["data"])
    else:
        rates = treasury_fetch("v2/accounting/od/avg_interest_rates", limit=20)
        if rates:
            results["avg_rates"] = rates
            db_execute(conn, "INSERT OR REPLACE INTO av_cache (ticker, function, data, fetched_at) VALUES (?, ?, ?, ?)",
                       ("_MACRO_", "TREASURY_RATES", json.dumps(rates), datetime.utcnow().isoformat()))

    return results


# ══════════════════════════════════════════════════════════════════════════════
# Multi-source scraper — free financial data sites with adaptive fallback
# ══════════════════════════════════════════════════════════════════════════════

# These are all free, no-auth sites that serve financial data in parseable formats.
# The scraper tracks success/failure per source and auto-routes to working ones.

SCRAPE_SOURCES = [
    # CSV/JSON endpoints (most reliable)
    {"name": "stooq_csv",          "url": "https://stooq.com/q/l/?s={TICKER}.us&f=sd2t2ohlcvn&h&e=csv",
     "type": "quote",             "parser": "stooq_csv"},
    {"name": "yahoo_chart",        "url": "https://query2.finance.yahoo.com/v8/finance/chart/{TICKER}?interval=1d&range=5d",
     "type": "quote",             "parser": "yahoo_chart"},
    {"name": "google_finance",     "url": "https://www.google.com/finance/quote/{TICKER}:NYSE",
     "type": "quote",             "parser": "google_html"},
    {"name": "google_finance_nq",  "url": "https://www.google.com/finance/quote/{TICKER}:NASDAQ",
     "type": "quote",             "parser": "google_html"},
    # Fundamentals from free sites
    {"name": "wisesheets",         "url": "https://wisesheets.io/api/free/kpi?ticker={TICKER}",
     "type": "financials",        "parser": "json_generic"},
    {"name": "stockanalysis_fin",  "url": "https://stockanalysis.com/stocks/{ticker}/financials/",
     "type": "financials",        "parser": "html_table"},
    {"name": "macrotrends",        "url": "https://www.macrotrends.net/stocks/charts/{TICKER}/{ticker}/revenue",
     "type": "financials",        "parser": "html_table"},
    # Holders / Institutional
    {"name": "sec_13f_frames",     "url": "https://data.sec.gov/api/xbrl/frames/us-gaap/Assets/USD/CY2024Q4I.json",
     "type": "cross_company",     "parser": "edgar_frames"},
    # Short interest
    {"name": "shortvolume",        "url": "https://shortvolumes.com/?t={TICKER}",
     "type": "short_interest",    "parser": "html_text"},
    # Dividends
    {"name": "dividend_com",       "url": "https://www.dividend.com/stocks/{ticker}/",
     "type": "dividend",          "parser": "html_text"},
    # Earnings
    {"name": "earningswhispers",   "url": "https://www.earningswhispers.com/stocks/{ticker}",
     "type": "earnings",          "parser": "html_text"},
    # Options
    {"name": "yahoo_options",      "url": "https://query2.finance.yahoo.com/v7/finance/options/{TICKER}",
     "type": "options",           "parser": "yahoo_options"},
]

def init_scrape_sources(conn):
    """Populate scrape_sources table with known sources."""
    for src in SCRAPE_SOURCES:
        db_execute(conn, """
            INSERT OR IGNORE INTO scrape_sources (name, url_template, data_type, parser)
            VALUES (?, ?, ?, ?)
        """, (src["name"], src["url"], src["type"], src["parser"]))

def scrape_quote(ticker, conn):
    """Try multiple sources to get a stock quote, tracking success rates."""
    # Get enabled quote sources sorted by success rate
    sources = db_query(conn, """
        SELECT name, url_template, parser FROM scrape_sources
        WHERE data_type = 'quote' AND enabled = 1
        ORDER BY success_count DESC, fail_count ASC
    """)
    if not sources:
        sources = [{"name": s["name"], "url_template": s["url"], "parser": s["parser"]}
                   for s in SCRAPE_SOURCES if s["type"] == "quote"]

    for src in sources:
        name = src["name"]
        url = src["url_template"].replace("{TICKER}", ticker.upper()).replace("{ticker}", ticker.lower())
        t0 = time.time()
        try:
            req = Request(url, headers={
                "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36",
            })
            raw = urlopen(req, timeout=8).read().decode(errors="replace")
            latency = int((time.time() - t0) * 1000)
            result = _parse_quote(src["parser"], raw, ticker)

            if result and result.get("price"):
                # Record success
                db_execute(conn, """
                    UPDATE scrape_sources SET success_count = success_count + 1,
                    avg_latency_ms = (avg_latency_ms * success_count + ?) / (success_count + 1),
                    last_success = ? WHERE name = ?
                """, (latency, datetime.utcnow().isoformat(), name))

                # Cache the quote
                result["source"] = name
                db_execute(conn, """
                    INSERT OR REPLACE INTO av_cache (ticker, function, data, fetched_at)
                    VALUES (?, ?, ?, ?)
                """, (ticker, f"SCRAPE_{name}", json.dumps(result), datetime.utcnow().isoformat()))

                return result
            else:
                db_execute(conn, "UPDATE scrape_sources SET fail_count = fail_count + 1, last_failure = ? WHERE name = ?",
                           (datetime.utcnow().isoformat(), name))
        except Exception:
            db_execute(conn, "UPDATE scrape_sources SET fail_count = fail_count + 1, last_failure = ? WHERE name = ?",
                       (datetime.utcnow().isoformat(), name))
    return None

def _parse_quote(parser, raw, ticker):
    """Parse raw response based on parser type."""
    try:
        if parser == "stooq_csv":
            lines = raw.strip().split("\n")
            if len(lines) >= 2:
                parts = lines[1].split(",")
                if len(parts) >= 7 and parts[5] not in ("N/D", ""):
                    return {"price": float(parts[5]), "open": float(parts[3]) if parts[3] != "N/D" else None,
                            "high": float(parts[4]) if parts[4] != "N/D" else None,
                            "low": float(parts[5]) if parts[5] != "N/D" else None,
                            "volume": float(parts[7]) if len(parts) > 7 and parts[7] != "N/D" else None}

        elif parser == "yahoo_chart":
            data = json.loads(raw)
            meta = data.get("chart", {}).get("result", [{}])[0].get("meta", {})
            price = meta.get("regularMarketPrice")
            if price:
                return {"price": price, "prev_close": meta.get("previousClose"),
                        "52w_high": meta.get("fiftyTwoWeekHigh"), "52w_low": meta.get("fiftyTwoWeekLow"),
                        "50dma": meta.get("fiftyDayAverage"), "200dma": meta.get("twoHundredDayAverage")}

        elif parser == "google_html":
            # Extract price from Google Finance HTML
            import re
            # Look for data-last-price attribute
            m = re.search(r'data-last-price="([0-9.]+)"', raw)
            if m:
                return {"price": float(m.group(1))}
            # Fallback: look for price in structured data
            m = re.search(r'"price":"([0-9.]+)"', raw)
            if m:
                return {"price": float(m.group(1))}

        elif parser == "yahoo_options":
            data = json.loads(raw)
            chain = data.get("optionChain", {}).get("result", [{}])[0]
            quote = chain.get("quote", {})
            if quote.get("regularMarketPrice"):
                return {"price": quote["regularMarketPrice"],
                        "iv": quote.get("impliedVolatility"),
                        "market_cap": quote.get("marketCap")}

        elif parser == "json_generic":
            data = json.loads(raw)
            if isinstance(data, dict):
                return data

    except Exception:
        pass
    return None

def scrape_batch_quotes(tickers, conn, max_workers=6):
    """Scrape quotes for multiple tickers using adaptive multi-source routing."""
    results = {}
    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        futures = {pool.submit(scrape_quote, t, conn): t for t in tickers}
        for f in as_completed(futures):
            t = futures[f]
            try:
                r = f.result()
                if r:
                    results[t] = r
            except Exception:
                pass
    return results

def show_source_health(conn):
    """Display scrape source reliability stats."""
    sources = db_query(conn, """
        SELECT name, data_type, success_count, fail_count, avg_latency_ms, last_success, enabled
        FROM scrape_sources ORDER BY data_type, success_count DESC
    """)
    if not sources:
        sprint(f"  {dim('No scrape sources registered')}")
        return

    sprint(f"\n  {bold('Scrape Source Health:')}")
    sprint(f"    {'Source':<24s} {'Type':<14s} {'OK':>5s} {'Fail':>5s} {'Rate':>6s} {'Lat':>6s} {'Last OK':<12s}")
    sprint(f"    {'─'*24} {'─'*14} {'─'*5} {'─'*5} {'─'*6} {'─'*6} {'─'*12}")
    for s in sources:
        total = (s["success_count"] or 0) + (s["fail_count"] or 0)
        rate = f"{s['success_count']/total*100:.0f}%" if total > 0 else " —"
        lat = f"{s['avg_latency_ms']:.0f}ms" if s["avg_latency_ms"] else "  —"
        last = (s["last_success"] or "")[:10]
        ok_str = str(s["success_count"] or 0)
        fail_str = str(s["fail_count"] or 0)
        enabled = "" if s["enabled"] else " [OFF]"
        sprint(f"    {s['name']:<24s} {s['data_type']:<14s} {ok_str:>5s} {fail_str:>5s} {rate:>6s} {lat:>6s} {last:<12s}{enabled}")


# ══════════════════════════════════════════════════════════════════════════════
# Alpha Vantage with SQLite cache
# ══════════════════════════════════════════════════════════════════════════════

AV_BASE = "https://www.alphavantage.co/query"
_av_sem = Semaphore(4)  # premium: 75/min, but be conservative with parallel

def av_fetch_cached(conn, ticker, function, cache_days=7, **extra_params):
    """Fetch from AV with SQLite cache. Returns parsed JSON or None."""
    if not AV_KEY:
        return None

    # Check cache
    cutoff = (datetime.utcnow() - timedelta(days=cache_days)).isoformat()
    cached = db_one(conn, """
        SELECT data FROM av_cache
        WHERE ticker = ? AND function = ? AND fetched_at > ?
    """, (ticker, function, cutoff))
    if cached:
        try:
            return json.loads(cached["data"])
        except Exception:
            pass

    # Fetch from API
    with _av_sem:
        time.sleep(0.2)  # ~5/s = 300/min, well under 75/min burst with semaphore
        params = {"function": function, "symbol": ticker, "apikey": AV_KEY}
        params.update(extra_params)
        qs = "&".join(f"{k}={v}" for k, v in params.items())
        url = f"{AV_BASE}?{qs}"
        try:
            req = Request(url, headers={"User-Agent": "dsco-kb/2.0"})
            raw = urlopen(req, timeout=20).read().decode()
            data = json.loads(raw)
            if "Error Message" in data or "Note" in data:
                return None
            if "Information" in data and "rate" in data.get("Information", "").lower():
                time.sleep(3)
                return None

            # Cache it
            db_execute(conn, """
                INSERT OR REPLACE INTO av_cache (ticker, function, data, fetched_at)
                VALUES (?, ?, ?, ?)
            """, (ticker, function, raw, datetime.utcnow().isoformat()))
            return data
        except Exception:
            return None

def av_enrich_company(conn, ticker):
    """Pull AV overview + quote, update company record. Returns dict of enriched fields."""
    result = {}

    # Company Overview
    ov = av_fetch_cached(conn, ticker, "OVERVIEW", cache_days=14)
    if ov and ov.get("Symbol"):
        def sf(key):
            v = ov.get(key)
            if v in (None, "None", "-", ""): return None
            try: return float(v)
            except: return None

        result["sector"] = ov.get("Sector", "")
        result["industry"] = ov.get("Industry", "")
        result["description"] = ov.get("Description", "")
        result["market_cap"] = sf("MarketCapitalization")
        result["beta"] = sf("Beta")
        result["pe"] = sf("PERatio")
        result["fwd_pe"] = sf("ForwardPE")
        result["eps"] = sf("EPS")
        result["dividend_yield"] = sf("DividendYield")
        result["52w_high"] = sf("52WeekHigh")
        result["52w_low"] = sf("52WeekLow")
        result["50dma"] = sf("50DayMovingAverage")
        result["200dma"] = sf("200DayMovingAverage")
        result["analyst_target"] = sf("AnalystTargetPrice")
        result["shares_out"] = sf("SharesOutstanding")
        result["profit_margin"] = sf("ProfitMargin")
        result["op_margin"] = sf("OperatingMarginTTM")
        result["roe"] = sf("ReturnOnEquityTTM")
        result["roa"] = sf("ReturnOnAssetsTTM")
        result["revenue_ttm"] = sf("RevenueTTM")
        result["ebitda_ttm"] = sf("EBITDA")
        result["ev_ebitda"] = sf("EVToEBITDA")
        result["ps_ratio"] = sf("PriceToSalesRatioTTM")

        # Update companies table
        db_execute(conn, """
            UPDATE companies SET sector = ?, industry = ?, description = ?, market_cap = ?,
                                 last_updated = ?
            WHERE ticker = ?
        """, (result.get("sector"), result.get("industry"), result.get("description"),
              result.get("market_cap"), datetime.utcnow().isoformat(), ticker))

    # Global Quote for current price
    gq = av_fetch_cached(conn, ticker, "GLOBAL_QUOTE", cache_days=1, datatype="json")
    if gq:
        q = gq.get("Global Quote", {})
        try:
            result["price"] = float(q.get("05. price", 0))
            result["volume"] = float(q.get("06. volume", 0))
        except:
            pass

    # Income statement (for companies EDGAR missed)
    inc = av_fetch_cached(conn, ticker, "INCOME_STATEMENT", cache_days=30)
    if inc and inc.get("annualReports"):
        result["av_income"] = inc["annualReports"]

    # Balance sheet
    bs = av_fetch_cached(conn, ticker, "BALANCE_SHEET", cache_days=30)
    if bs and bs.get("annualReports"):
        result["av_balance"] = bs["annualReports"]

    # Cash flow
    cf = av_fetch_cached(conn, ticker, "CASH_FLOW", cache_days=30)
    if cf and cf.get("annualReports"):
        result["av_cashflow"] = cf["annualReports"]

    return result

def av_parse_financials(av_data):
    """Parse AV financial statements into rows compatible with our schema."""
    income = av_data.get("av_income", [])
    balance = av_data.get("av_balance", [])
    cashflow = av_data.get("av_cashflow", [])

    def sf(d, key):
        if not d: return None
        v = d.get(key, "None")
        if v in (None, "None", "-", ""): return None
        try: return float(v)
        except: return None

    # Index balance/cashflow by fiscal year
    bs_by_year = {}
    for r in balance:
        fy = r.get("fiscalDateEnding", "")[:4]
        if fy: bs_by_year[fy] = r
    cf_by_year = {}
    for r in cashflow:
        fy = r.get("fiscalDateEnding", "")[:4]
        if fy: cf_by_year[fy] = r

    rows = []
    prev_row = None
    for inc_r in sorted(income, key=lambda x: x.get("fiscalDateEnding", "")):
        fy = inc_r.get("fiscalDateEnding", "")[:4]
        if not fy: continue
        bs_r = bs_by_year.get(fy, {})
        cf_r = cf_by_year.get(fy, {})

        row = {"fiscal_year": fy}

        # Income
        row["revenue"] = sf(inc_r, "totalRevenue")
        row["gross_profit"] = sf(inc_r, "grossProfit")
        row["op_income"] = sf(inc_r, "operatingIncome")
        row["net_income"] = sf(inc_r, "netIncome")
        row["ebitda"] = sf(inc_r, "ebitda")
        row["rd_expense"] = sf(inc_r, "researchAndDevelopment")
        row["sga_expense"] = sf(inc_r, "sellingGeneralAndAdministrative")
        row["interest_exp"] = sf(inc_r, "interestExpense")
        row["tax_expense"] = sf(inc_r, "incomeTaxExpense")
        row["pretax_income"] = sf(inc_r, "incomeBeforeTax")
        row["da"] = sf(inc_r, "depreciationAndAmortization")

        # Balance
        row["total_assets"] = sf(bs_r, "totalAssets")
        row["current_assets"] = sf(bs_r, "totalCurrentAssets")
        row["cash"] = sf(bs_r, "cashAndCashEquivalentsAtCarryingValue") or sf(bs_r, "cashAndShortTermInvestments")
        row["receivables"] = sf(bs_r, "currentNetReceivables")
        row["inventory"] = sf(bs_r, "inventory")
        row["total_liab"] = sf(bs_r, "totalLiabilities")
        row["current_liab"] = sf(bs_r, "totalCurrentLiabilities")
        row["lt_debt"] = sf(bs_r, "longTermDebt")
        row["st_debt"] = sf(bs_r, "shortTermDebt") or sf(bs_r, "currentLongTermDebt")
        row["equity"] = sf(bs_r, "totalShareholderEquity")
        row["retained_earn"] = sf(bs_r, "retainedEarnings")
        row["goodwill"] = sf(bs_r, "goodwill")
        row["intangibles"] = sf(bs_r, "intangibleAssets")
        row["ppe"] = sf(bs_r, "propertyPlantEquipment")
        row["shares_out"] = sf(bs_r, "commonStockSharesOutstanding")

        # Cash flow
        row["cfo"] = sf(cf_r, "operatingCashflow")
        row["capex"] = sf(cf_r, "capitalExpenditures")
        row["dividends"] = sf(cf_r, "dividendPayout")
        row["buybacks"] = sf(cf_r, "commonStockRepurchased") or sf(cf_r, "paymentsForRepurchaseOfCommonStock")
        row["sbc"] = sf(cf_r, "stockBasedCompensation") or sf(cf_r, "shareBasedCompensation")

        # Derived metrics (same logic as EDGAR path)
        rev = row.get("revenue")
        ni = row.get("net_income")
        gp = row.get("gross_profit")
        op = row.get("op_income")
        ta = row.get("total_assets")
        eq = row.get("equity")
        ca = row.get("current_assets")
        cl = row.get("current_liab")
        cfo = row.get("cfo")
        cx = row.get("capex")
        ltd = row.get("lt_debt") or 0
        std = row.get("st_debt") or 0

        if rev and rev > 0:
            if gp is not None: row["gross_margin"] = gp / rev
            if op is not None: row["op_margin"] = op / rev
            if ni is not None: row["net_margin"] = ni / rev
        if eq and eq > 0 and ni is not None: row["roe"] = ni / eq
        if ta and ta > 0 and ni is not None: row["roa"] = ni / ta
        if op is not None and eq is not None:
            tax_rate = 0.21
            pt = row.get("pretax_income")
            tx = row.get("tax_expense")
            if pt and pt > 0 and tx and tx > 0: tax_rate = min(tx / pt, 0.40)
            nopat = op * (1 - tax_rate)
            ic = eq + ltd + std - (row.get("cash") or 0)
            if ic > 0: row["roic"] = nopat / ic
        if eq and eq > 0: row["de_ratio"] = (ltd + std) / eq
        if cl and cl > 0 and ca: row["current_ratio"] = ca / cl
        if cfo is not None:
            row["fcf"] = cfo - abs(cx or 0)
        if op is not None:
            row["ebitda"] = row.get("ebitda") or (op + abs(row.get("da") or 0))
        if prev_row and prev_row.get("revenue") and prev_row["revenue"] > 0 and rev:
            row["revenue_growth"] = (rev - prev_row["revenue"]) / abs(prev_row["revenue"])
        if prev_row and prev_row.get("net_income") and prev_row["net_income"] != 0 and ni:
            row["earnings_growth"] = (ni - prev_row["net_income"]) / abs(prev_row["net_income"])

        rows.append(row)
        prev_row = row

    return rows


# ══════════════════════════════════════════════════════════════════════════════
# BUILD command — populate KB from EDGAR + Alpha Vantage
# ══════════════════════════════════════════════════════════════════════════════

def cmd_build(args):
    conn = get_db()
    sprint(f"\n  {bold('Building Equity Knowledge Base')}")
    sprint(f"  {dim('Database:')} {DB_PATH}\n")

    # Phase 1: Load ticker universe from NASDAQ FTP
    sprint(f"  {bold('Phase 1: Loading ticker universe')}")
    sprint(f"  {dim('⚡')} NASDAQ FTP (nasdaqlisted.txt + otherlisted.txt)...", end=" ", flush=True)
    nasdaq_tickers = load_nasdaq_universe()
    sprint(green(f"✓ {len(nasdaq_tickers):,} common stocks"))

    # Phase 2: Load SEC EDGAR CIK mapping
    sprint(f"  {dim('⚡')} SEC EDGAR CIK mapping...", end=" ", flush=True)
    edgar_tickers = edgar_load_tickers()
    sprint(green(f"✓ {len(edgar_tickers):,} CIK entries"))

    # Merge: NASDAQ universe + EDGAR CIK
    tickers = {}
    matched = 0
    for sym, info in nasdaq_tickers.items():
        edgar_info = edgar_tickers.get(sym, {})
        cik = edgar_info.get("cik", "")
        tickers[sym] = {
            "name": info["name"],
            "exchange": info["exchange"],
            "cik": cik,
        }
        if cik:
            matched += 1

    sprint(f"  {dim('⚡')} Merged: {green(f'{len(tickers):,}')} stocks, {green(f'{matched:,}')} with SEC CIK")

    # Also include EDGAR-only tickers not in NASDAQ lists
    edgar_only = 0
    for sym, info in edgar_tickers.items():
        if sym not in tickers:
            tickers[sym] = {"name": info.get("name", ""), "exchange": "EDGAR", "cik": info["cik"]}
            edgar_only += 1
    if edgar_only > 0:
        sprint(f"  {dim('⚡')} Added {edgar_only:,} EDGAR-only tickers (OTC/delisted/foreign)")
    sprint(f"  {dim('⚡')} {bold(f'Total universe: {len(tickers):,} equities')}")

    # Filter
    if args.exchange:
        tickers = {t: v for t, v in tickers.items() if v.get("exchange", "").upper().startswith(args.exchange.upper())}
        sprint(f"  {dim(f'Filtered to exchange {args.exchange}:')} {len(tickers):,}")

    if args.ticker:
        requested = [t.strip().upper() for t in args.ticker.split(",")]
        subset = {t: tickers[t] for t in requested if t in tickers}
        if not subset:
            sprint(red(f"  No matching tickers found"))
            return
        tickers = subset

    if args.limit and args.limit < len(tickers):
        keys = sorted(tickers.keys())[:args.limit]
        tickers = {k: tickers[k] for k in keys}

    # Skip tickers without CIK (can't fetch EDGAR data)
    with_cik = {t: v for t, v in tickers.items() if v.get("cik")}
    no_cik = len(tickers) - len(with_cik)
    if no_cik > 0:
        sprint(f"  {dim(f'Skipping {no_cik:,} tickers without SEC CIK')}")
    tickers = with_cik

    sprint(f"\n  {bold('Phase 2: Fetching EDGAR XBRL data')}")
    sprint(f"  {dim('Processing:')} {len(tickers):,} companies with {args.workers} workers\n")

    # Insert companies
    now = datetime.utcnow().isoformat()
    for t, info in tickers.items():
        db_execute(conn, """
            INSERT OR IGNORE INTO companies (ticker, cik, name, exchange, last_updated)
            VALUES (?, ?, ?, ?, ?)
        """, (t, info.get("cik", ""), info["name"], info.get("exchange", ""), now))

    # Fetch EDGAR data in parallel
    processed = [0]
    errors = [0]
    total = len(tickers)
    t0 = time.time()

    def process_ticker(ticker, info):
        try:
            facts = edgar_fetch_facts(info["cik"])
            if not facts:
                errors[0] += 1
                return

            rows = edgar_extract_financials(facts)
            if not rows:
                errors[0] += 1
                return

            # Entity name from EDGAR
            entity_name = facts.get("entityName", info.get("name", ""))

            # Store annual financials
            for row in rows:
                fields = list(row.keys())
                placeholders = ",".join(["?"] * (len(fields) + 1))
                field_names = ",".join(["ticker"] + fields)
                values = [ticker] + [row.get(f) for f in fields]
                db_execute(conn, f"""
                    INSERT OR REPLACE INTO annual_financials ({field_names})
                    VALUES ({placeholders})
                """, values)

            # Compute aggregate metrics
            metrics = compute_aggregate_metrics(rows, ticker)
            if metrics:
                fields = list(metrics.keys())
                placeholders = ",".join(["?"] * len(fields))
                field_names = ",".join(fields)
                values = [metrics.get(f) for f in fields]
                db_execute(conn, f"""
                    INSERT OR REPLACE INTO computed_metrics ({field_names})
                    VALUES ({placeholders})
                """, values)

            # Update company record
            db_execute(conn, """
                UPDATE companies SET name = ?, edgar_fetched = ? WHERE ticker = ?
            """, (entity_name, datetime.utcnow().isoformat(), ticker))

            processed[0] += 1
            if processed[0] % 50 == 0:
                elapsed = time.time() - t0
                rate = processed[0] / elapsed
                eta = (total - processed[0]) / rate if rate > 0 else 0
                sprint(f"    {green(f'{processed[0]:>5,}')} / {total:,}  "
                       f"({errors[0]} errors)  "
                       f"{dim(f'{rate:.0f}/s  ETA {eta/60:.0f}m')}")

        except Exception as e:
            errors[0] += 1

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(process_ticker, t, info): t for t, info in tickers.items()}
        for f in as_completed(futures):
            pass  # results handled in process_ticker

    elapsed = time.time() - t0
    sprint(f"\n  {bold('EDGAR phase complete:')}")
    sprint(f"    {green(f'{processed[0]:,}')} companies processed, {red(f'{errors[0]:,}')} errors")
    sprint(f"    {dim(f'{elapsed:.0f}s elapsed ({processed[0]/elapsed:.0f}/s)')}")

    # ── Phase 3: Alpha Vantage enrichment ──
    if AV_KEY and not args.no_av:
        sprint(f"\n  {bold('Phase 3: Alpha Vantage enrichment')} (sector, price, market data)")

        # Find companies needing enrichment: no sector, no market_cap, or EDGAR failed
        needs_av = db_query(conn, """
            SELECT c.ticker FROM companies c
            WHERE c.ticker IN ({})
            AND (c.sector IS NULL OR c.sector = '' OR c.market_cap IS NULL
                 OR c.edgar_fetched IS NULL)
            ORDER BY c.ticker
        """.format(",".join("?" * len(tickers))), list(tickers.keys()))

        av_tickers = [r["ticker"] for r in needs_av]
        if not av_tickers:
            sprint(f"    {green('All companies already have market data')}")
        else:
            sprint(f"    {dim(f'{len(av_tickers)} companies need enrichment')}\n")
            av_done = [0]
            av_filled = [0]

            def av_enrich_one(t):
                try:
                    result = av_enrich_company(conn, t)
                    av_done[0] += 1

                    # If EDGAR failed, try to fill financials from AV
                    existing = db_one(conn, "SELECT COUNT(*) as n FROM annual_financials WHERE ticker = ?", (t,))
                    if (existing is None or existing["n"] == 0) and result.get("av_income"):
                        av_rows = av_parse_financials(result)
                        if av_rows:
                            for row in av_rows:
                                fields = list(row.keys())
                                placeholders = ",".join(["?"] * (len(fields) + 1))
                                field_names = ",".join(["ticker"] + fields)
                                values = [t] + [row.get(f) for f in fields]
                                db_execute(conn, f"INSERT OR REPLACE INTO annual_financials ({field_names}) VALUES ({placeholders})", values)
                            metrics = compute_aggregate_metrics(av_rows, t)
                            if metrics:
                                fields = list(metrics.keys())
                                placeholders = ",".join(["?"] * len(fields))
                                field_names = ",".join(fields)
                                values = [metrics.get(f) for f in fields]
                                db_execute(conn, f"INSERT OR REPLACE INTO computed_metrics ({field_names}) VALUES ({placeholders})", values)
                            av_filled[0] += 1

                    if av_done[0] % 20 == 0:
                        sprint(f"    {green(f'{av_done[0]:>4}')} / {len(av_tickers)}  "
                               f"({av_filled[0]} filled from AV)")
                except Exception:
                    pass

            with ThreadPoolExecutor(max_workers=4) as pool:
                futures = [pool.submit(av_enrich_one, t) for t in av_tickers]
                for f in as_completed(futures):
                    pass

            sprint(f"    {bold('AV enrichment complete:')} {av_done[0]} enriched, {av_filled[0]} filled with AV financials")

    # ── Phase 4: Multi-source price scraping (adaptive, no key needed) ──
    sprint(f"\n  {bold('Phase 4: Multi-source price scraping')} (adaptive fallback)")
    init_scrape_sources(conn)
    all_built = db_query(conn, "SELECT ticker FROM companies WHERE edgar_fetched IS NOT NULL OR sector IS NOT NULL")
    all_tickers = [r["ticker"] for r in all_built]

    # Only scrape tickers that don't have a recent quote cached
    cutoff = (datetime.utcnow() - timedelta(hours=12)).isoformat()
    already_quoted = set()
    for r in db_query(conn, "SELECT DISTINCT ticker FROM av_cache WHERE function LIKE 'SCRAPE_%' AND fetched_at > ?", (cutoff,)):
        already_quoted.add(r["ticker"])
    # Also count AV quotes
    for r in db_query(conn, "SELECT DISTINCT ticker FROM av_cache WHERE function = 'GLOBAL_QUOTE' AND fetched_at > ?", (cutoff,)):
        already_quoted.add(r["ticker"])

    need_quotes = [t for t in all_tickers if t not in already_quoted]
    if need_quotes:
        sprint(f"    {len(need_quotes)} tickers need price quotes ({len(already_quoted)} already cached)")
        # Process in batches
        batch_size = 30
        total_scraped = 0
        for i in range(0, len(need_quotes), batch_size):
            batch = need_quotes[i:i+batch_size]
            results = scrape_batch_quotes(batch, conn, max_workers=6)
            total_scraped += len(results)
            if (i // batch_size + 1) % 3 == 0:
                sprint(f"    {green(f'{total_scraped:>5}')} quotes scraped ({i+batch_size}/{len(need_quotes)})")
        sprint(f"    {bold(f'{total_scraped} new quotes')} scraped from multiple sources")
    else:
        sprint(f"    {green(f'All {len(already_quoted)} tickers have recent quotes cached')}")

    show_source_health(conn)

    # ── Phase 5: EDGAR Submissions (SIC codes, entity info) ──
    sprint(f"\n  {bold('Phase 5: SEC EDGAR submissions')} (SIC codes, entity details)")
    needs_sic = db_query(conn, """
        SELECT c.ticker, c.cik FROM companies c
        WHERE c.cik IS NOT NULL AND c.cik != ''
        AND (c.sic_code IS NULL OR c.sic_code = '')
        LIMIT ?
    """, (args.limit or 500,))
    if needs_sic:
        sprint(f"    {len(needs_sic)} companies need SIC codes")
        sic_done = [0]

        def fetch_sic(ticker_cik):
            t, cik = ticker_cik
            try:
                info = edgar_get_company_info(cik)
                if info.get("sic_code"):
                    db_execute(conn, "UPDATE companies SET sic_code = ? WHERE ticker = ?",
                               (info["sic_code"], t))
                    sic_done[0] += 1
            except Exception:
                pass

        with ThreadPoolExecutor(max_workers=8) as pool:
            futures = [pool.submit(fetch_sic, (r["ticker"], r["cik"])) for r in needs_sic]
            for f in as_completed(futures):
                pass

        sprint(f"    {bold(f'{sic_done[0]} SIC codes')} added")
    else:
        sprint(f"    {green('All companies have SIC codes')}")

    # ── Phase 6: FRED macro data ──
    sprint(f"\n  {bold('Phase 6: FRED macro indicators')}")
    macro = fred_fetch_all_macro(conn)
    if macro:
        sprint(f"    {bold(f'{len(macro)} macro series')} cached: {', '.join(macro.keys())}")
    else:
        sprint(f"    {dim('Skipped (no FRED_API_KEY)')}")

    # ── Phase 7: Treasury fiscal data ──
    sprint(f"\n  {bold('Phase 7: Treasury.gov fiscal data')}")
    treasury = treasury_get_macro(conn)
    if treasury:
        sprint(f"    {bold(f'{len(treasury)} datasets')} cached")
    else:
        sprint(f"    {dim('No data retrieved')}")

    # Final stats
    total_companies = db_one(conn, "SELECT COUNT(*) as n FROM companies")["n"]
    total_fins = db_one(conn, "SELECT COUNT(*) as n FROM annual_financials")["n"]
    total_metrics = db_one(conn, "SELECT COUNT(*) as n FROM computed_metrics")["n"]
    total_cache = db_one(conn, "SELECT COUNT(*) as n FROM av_cache")["n"]
    db_size = os.path.getsize(DB_PATH) / 1e6

    sprint(f"\n  {'═' * 60}")
    sprint(f"  {bold('BUILD COMPLETE')}")
    sprint(f"    Companies:        {green(f'{total_companies:,}')}")
    sprint(f"    Financial rows:   {green(f'{total_fins:,}')}")
    sprint(f"    Computed metrics: {green(f'{total_metrics:,}')}")
    sprint(f"    Cache entries:    {green(f'{total_cache:,}')}")
    sprint(f"    Database size:    {db_size:.1f} MB")
    sprint(f"  {'═' * 60}\n")

# ══════════════════════════════════════════════════════════════════════════════
# UPDATE command — refresh stale data + enrich with Jina/Parallel
# ══════════════════════════════════════════════════════════════════════════════

def cmd_update(args):
    conn = get_db()
    sprint(f"\n  {bold('Updating Knowledge Base')}\n")

    if args.ticker:
        tickers = args.ticker.upper().split(",")
    else:
        # Find stale companies
        cutoff = (datetime.utcnow() - timedelta(days=args.stale_days)).isoformat()
        rows = db_query(conn, """
            SELECT ticker, cik FROM companies
            WHERE edgar_fetched IS NULL OR edgar_fetched < ?
            ORDER BY ticker LIMIT ?
        """, (cutoff, args.limit or 500))
        tickers = [r["ticker"] for r in rows]

    if not tickers:
        sprint(f"  {green('Everything is up to date!')}")
        return

    sprint(f"  {dim('Updating')} {len(tickers)} companies\n")

    for i, ticker in enumerate(tickers):
        sprint(f"  [{i+1}/{len(tickers)}] {cyan(ticker)}", end=" ", flush=True)

        company = db_one(conn, "SELECT cik FROM companies WHERE ticker = ?", (ticker,))
        if not company:
            sprint(red("not in DB"))
            continue

        # Refresh EDGAR
        facts = edgar_fetch_facts(company["cik"])
        if facts:
            rows = edgar_extract_financials(facts)
            for row in rows:
                fields = list(row.keys())
                placeholders = ",".join(["?"] * (len(fields) + 1))
                field_names = ",".join(["ticker"] + fields)
                values = [ticker] + [row.get(f) for f in fields]
                db_execute(conn, f"""
                    INSERT OR REPLACE INTO annual_financials ({field_names})
                    VALUES ({placeholders})
                """, values)

            metrics = compute_aggregate_metrics(rows, ticker)
            if metrics:
                fields = list(metrics.keys())
                placeholders = ",".join(["?"] * len(fields))
                field_names = ",".join(fields)
                values = [metrics.get(f) for f in fields]
                db_execute(conn, f"""
                    INSERT OR REPLACE INTO computed_metrics ({field_names})
                    VALUES ({placeholders})
                """, values)

            db_execute(conn, "UPDATE companies SET edgar_fetched = ? WHERE ticker = ?",
                       (datetime.utcnow().isoformat(), ticker))
            sprint(green("EDGAR✓"), end=" ", flush=True)

        # Enrich with Jina research
        if args.enrich and JINA_KEY:
            company_name = db_one(conn, "SELECT name FROM companies WHERE ticker = ?", (ticker,))
            name = company_name["name"] if company_name else ticker

            # Search for recent analysis
            results = jina_search(f"{name} {ticker} stock analysis competitive position 2025 2026", num=3)
            for r in results:
                content = r.get("content", "")
                if not content: continue
                h = hashlib.md5(content[:500].encode()).hexdigest()
                existing = db_one(conn, "SELECT id FROM research_notes WHERE content_hash = ?", (h,))
                if not existing:
                    db_execute(conn, """
                        INSERT INTO research_notes (ticker, source, source_url, title, content, content_hash, created_at, tokens_approx)
                        VALUES (?, 'jina_search', ?, ?, ?, ?, ?, ?)
                    """, (ticker, r.get("url", ""), r.get("title", ""), content, h,
                          datetime.utcnow().isoformat(), len(content) // 4))
            sprint(green("Jina✓"), end=" ", flush=True)

            # Generate embeddings for description + latest research
            embed_texts = []
            if facts:
                entity = facts.get("entityName", "")
                if entity:
                    embed_texts.append(f"{entity} ({ticker}): financial data available")
            for r in results[:2]:
                if r.get("content"):
                    embed_texts.append(r["content"][:2000])

            if embed_texts:
                embeddings = jina_embed(embed_texts)
                for j, emb in enumerate(embeddings):
                    db_execute(conn, """
                        INSERT INTO embeddings (ticker, source, text_preview, embedding, dim, model, created_at)
                        VALUES (?, ?, ?, ?, ?, ?, ?)
                    """, (ticker, "research", embed_texts[j][:200], pack_embedding(emb),
                          EMBED_DIM, EMBED_MODEL, datetime.utcnow().isoformat()))
                sprint(green("Embed✓"), end=" ", flush=True)

        # Deep research via Parallel.ai
        if args.deep_research and PARALLEL_KEY:
            company_name = db_one(conn, "SELECT name FROM companies WHERE ticker = ?", (ticker,))
            name = company_name["name"] if company_name else ticker
            query = f"Comprehensive investment analysis of {name} ({ticker}): competitive position, product portfolio, market dynamics, growth drivers, risks, and valuation"
            result = parallel_deep_research(query, ticker)
            if result:
                db_execute(conn, """
                    INSERT INTO research_jobs (ticker, provider, job_id, status, query, result, created_at)
                    VALUES (?, 'parallel', ?, ?, ?, ?, ?)
                """, (ticker, result.get("job_id", ""), result.get("status", ""),
                      query, result.get("result", ""), datetime.utcnow().isoformat()))
                sprint(green("Research✓"), end=" ", flush=True)

        sprint()

    sprint(f"\n  {bold('Update complete.')}\n")

# ══════════════════════════════════════════════════════════════════════════════
# SEARCH command — semantic search across KB
# ══════════════════════════════════════════════════════════════════════════════

def cmd_search(args):
    conn = get_db()
    query = args.query
    sprint(f"\n  {bold('Searching:')} {cyan(query)}\n")

    # Strategy 1: Semantic search via embeddings
    if JINA_KEY:
        sprint(f"  {dim('⚡')} Generating query embedding...", end=" ", flush=True)
        q_embs = jina_embed([query], task="retrieval.query")
        if q_embs:
            q_vec = q_embs[0]
            sprint(green("✓"))

            # Load all embeddings and compute similarity
            all_embs = db_query(conn, "SELECT ticker, source, text_preview, embedding, dim FROM embeddings")
            scored = []
            for row in all_embs:
                doc_vec = unpack_embedding(row["embedding"], row["dim"])
                sim = cosine_sim(q_vec, doc_vec)
                scored.append((sim, row["ticker"], row["source"], row["text_preview"]))

            scored.sort(reverse=True)
            sprint(f"\n  {bold('Semantic Results')} ({len(scored)} embeddings searched):\n")
            for sim, ticker, source, preview in scored[:15]:
                bar_w = int(sim * 30)
                sprint(f"    {sim:.3f} {'█' * bar_w} {cyan(ticker):>6s} [{dim(source)}] {dim(preview[:60])}")
        else:
            sprint(red("✗ (no API key or error)"))

    # Strategy 2: SQL-based keyword search across research notes
    sprint(f"\n  {bold('Full-text Results:')}\n")
    keywords = query.split()
    like_clauses = " AND ".join(f"(content LIKE ? OR title LIKE ?)" for _ in keywords)
    params = []
    for kw in keywords:
        params.extend([f"%{kw}%", f"%{kw}%"])

    notes = db_query(conn, f"""
        SELECT ticker, source, title, substr(content, 1, 200) as preview, created_at
        FROM research_notes WHERE {like_clauses}
        ORDER BY created_at DESC LIMIT 20
    """, params)

    if notes:
        for n in notes:
            sprint(f"    {cyan(n['ticker']):>6s} [{dim(n['source'])}] {n['title'][:50]}")
            sprint(f"           {dim(n['preview'][:100])}")
    else:
        sprint(f"    {dim('No text matches found')}")

    # Strategy 3: Screen by metrics matching the query
    sprint(f"\n  {bold('Matching Companies by Metrics:')}\n")
    matches = db_query(conn, """
        SELECT c.ticker, c.name, m.revenue_cagr_5y, m.avg_roic_5y, m.moat_width,
               m.quality_score, m.piotroski_score, m.latest_revenue
        FROM companies c JOIN computed_metrics m ON c.ticker = m.ticker
        WHERE m.quality_score >= 50
        ORDER BY m.quality_score DESC, m.avg_roic_5y DESC
        LIMIT 20
    """)
    if matches:
        sprint(f"    {'Ticker':<8s} {'Name':<30s} {'Rev CAGR':>10s} {'ROIC':>8s} {'Moat':>8s} {'Quality':>8s}")
        sprint(f"    {'─'*8} {'─'*30} {'─'*10} {'─'*8} {'─'*8} {'─'*8}")
        for r in matches:
            sprint(f"    {cyan(r['ticker']):<8s} {(r['name'] or '')[:30]:<30s} "
                   f"{pct(r['revenue_cagr_5y']):>10s} {pct(r['avg_roic_5y']):>8s} "
                   f"{(r['moat_width'] or ''):>8s} {r['quality_score'] or 0:>7.0f}")

    sprint()

# ══════════════════════════════════════════════════════════════════════════════
# REPORT command — generate full research report
# ══════════════════════════════════════════════════════════════════════════════

def cmd_report(args):
    conn = get_db()
    ticker = args.ticker.upper()
    sprint(f"\n  {bold('Generating Research Report:')} {cyan(ticker)}\n")

    company = db_one(conn, "SELECT * FROM companies WHERE ticker = ?", (ticker,))
    if not company:
        sprint(red(f"  {ticker} not in knowledge base. Run: equity_kb.py build --ticker {ticker}"))
        return

    metrics = db_one(conn, "SELECT * FROM computed_metrics WHERE ticker = ?", (ticker,))
    financials = db_query(conn, """
        SELECT * FROM annual_financials WHERE ticker = ? ORDER BY fiscal_year
    """, (ticker,))
    notes = db_query(conn, """
        SELECT * FROM research_notes WHERE ticker = ? ORDER BY created_at DESC LIMIT 10
    """, (ticker,))

    # Header
    sprint(f"  {'═' * 80}")
    sprint(f"  {bold(company['name'] or ticker)} ({cyan(ticker)})")
    if company["sector"]:
        sprint(f"  {company['sector']} · {company['industry'] or ''}")
    sprint(f"  {'═' * 80}")

    # Financial summary table
    if financials:
        recent = financials[-6:]
        years = [r["fiscal_year"] for r in recent]
        sprint(f"\n  {bold('Financial History:')}")
        sprint(f"    {'Metric':<16s}" + "".join(f"{y:>12s}" for y in years))
        sprint(f"    {'─'*16}" + "─"*12*len(years))

        for label, field, fmt in [
            ("Revenue", "revenue", usd), ("Gross Profit", "gross_profit", usd),
            ("Op Income", "op_income", usd), ("Net Income", "net_income", usd),
            ("FCF", "fcf", usd), ("ROIC", "roic", pct),
            ("Gross Margin", "gross_margin", pct), ("Op Margin", "op_margin", pct),
            ("D/E Ratio", "de_ratio", lambda v: f"{v:.2f}" if v else dim("—")),
        ]:
            row = f"    {label:<16s}"
            for r in recent:
                v = r[field] if r[field] is not None else None
                row += f"{fmt(v):>12s}"
            sprint(row)

    # Aggregate metrics
    if metrics:
        sprint(f"\n  {bold('Aggregate Metrics:')}")
        sprint(f"    Revenue CAGR 3Y/5Y:  {pct(metrics['revenue_cagr_3y'])} / {pct(metrics['revenue_cagr_5y'])}")
        sprint(f"    Earnings CAGR 3Y/5Y: {pct(metrics['earnings_cagr_3y'])} / {pct(metrics['earnings_cagr_5y'])}")
        sprint(f"    Avg ROIC (5Y):       {pct(metrics['avg_roic_5y'])}")
        sprint(f"    Avg ROE (5Y):        {pct(metrics['avg_roe_5y'])}")
        sprint(f"    Avg Gross Margin:    {pct(metrics['avg_gross_margin_5y'])}")
        sprint(f"    Avg Op Margin:       {pct(metrics['avg_op_margin_5y'])}")

        ps = metrics["piotroski_score"]
        if ps is not None:
            color = green if ps >= 7 else yellow if ps >= 4 else red
            sprint(f"    Piotroski F-Score:   {color(f'{ps}/9')}")

        az = metrics["altman_z"]
        if az is not None:
            zone = metrics["altman_zone"] or ""
            color = green if zone == "Safe" else yellow if zone == "Grey" else red
            sprint(f"    Altman Z-Score:      {color(f'{az:.2f} ({zone})')}")

        moat_w = metrics["moat_width"] or "N/A"
        moat_s = metrics["moat_score"] or 0
        qual = metrics["quality_score"] or 0
        sprint(f"    Moat:                {moat_w} (score: {moat_s})")
        sprint(f"    Quality Score:       {qual:.0f}/100")

        dcf_fv = metrics["dcf_fair_value"]
        if dcf_fv and dcf_fv > 0:
            sprint(f"\n  {bold('DCF Valuation:')}")
            sprint(f"    Fair Value/Share:    ${dcf_fv:,.2f}")
            ig = metrics["implied_growth"]
            if ig is not None:
                sprint(f"    Growth Assumption:   {pct(ig)}")

    # Research notes
    if notes:
        sprint(f"\n  {bold('Research Notes:')} ({len(notes)} entries)")
        for n in notes[:5]:
            sprint(f"    [{dim(n['source'])}] {n['title'][:60]}")
            preview = (n["content"] or "")[:200].replace("\n", " ")
            sprint(f"      {dim(preview)}")

    # Enrich on demand
    if args.depth in ("standard", "deep") and JINA_KEY:
        sprint(f"\n  {bold('Enriching with web research...')}")
        name = company["name"] or ticker
        queries = [
            f"{name} {ticker} competitive analysis market position 2025",
            f"{name} {ticker} product portfolio growth strategy",
            f"{name} {ticker} risks challenges headwinds",
        ]
        for q in queries:
            sprint(f"    {dim('⚡')} Searching: {q[:60]}...", end=" ", flush=True)
            results = jina_search(q, num=3)
            for r in results:
                content = r.get("content", "")
                if not content: continue
                h = hashlib.md5(content[:500].encode()).hexdigest()
                existing = db_one(conn, "SELECT id FROM research_notes WHERE content_hash = ?", (h,))
                if not existing:
                    db_execute(conn, """
                        INSERT INTO research_notes (ticker, source, source_url, title, content, content_hash, created_at, tokens_approx)
                        VALUES (?, 'jina_search', ?, ?, ?, ?, ?, ?)
                    """, (ticker, r.get("url", ""), r.get("title", ""), content, h,
                          datetime.utcnow().isoformat(), len(content) // 4))
            sprint(green(f"✓ {len(results)} results"))

    # Deep research via Parallel.ai
    if args.depth == "deep" and PARALLEL_KEY:
        sprint(f"\n  {bold('Launching deep research via Parallel.ai...')}")
        name = company["name"] or ticker
        result = parallel_deep_research(
            f"Comprehensive institutional-quality investment analysis of {name} ({ticker}). "
            f"Cover: business model, competitive moat, product portfolio, TAM, growth drivers, "
            f"management quality, capital allocation, key risks, regulatory exposure, "
            f"and forward outlook. Cite specific data points.",
            ticker
        )
        if result:
            if result.get("result"):
                sprint(f"\n  {bold('Deep Research Report:')}")
                sprint(textwrap.indent(textwrap.fill(result["result"][:3000], 78), "    "))
                db_execute(conn, """
                    INSERT INTO research_jobs (ticker, provider, job_id, status, query, result, created_at, completed_at)
                    VALUES (?, 'parallel', ?, ?, ?, ?, ?, ?)
                """, (ticker, result.get("job_id", ""), "completed", "deep research",
                      result.get("result", ""), datetime.utcnow().isoformat(),
                      datetime.utcnow().isoformat()))
            elif result.get("job_id"):
                sprint(f"    Job submitted: {result['job_id']} (poll with: equity_kb.py check-job {result['job_id']})")
                db_execute(conn, """
                    INSERT INTO research_jobs (ticker, provider, job_id, status, query, created_at)
                    VALUES (?, 'parallel', ?, 'pending', ?, ?)
                """, (ticker, result["job_id"], "deep research", datetime.utcnow().isoformat()))

    sprint()

# ══════════════════════════════════════════════════════════════════════════════
# SCREEN command — filter equities by quantitative criteria
# ══════════════════════════════════════════════════════════════════════════════

def cmd_screen(args):
    conn = get_db()
    sprint(f"\n  {bold('Equity Screener')}\n")

    conditions = []
    params = []

    if args.min_roic is not None:
        conditions.append("m.avg_roic_5y >= ?")
        params.append(args.min_roic / 100)
    if args.min_revenue_cagr is not None:
        conditions.append("m.revenue_cagr_5y >= ?")
        params.append(args.min_revenue_cagr / 100)
    if args.max_de is not None:
        conditions.append("(af.de_ratio IS NOT NULL AND af.de_ratio <= ?)")
        params.append(args.max_de)
    if args.min_piotroski is not None:
        conditions.append("m.piotroski_score >= ?")
        params.append(args.min_piotroski)
    if args.min_quality is not None:
        conditions.append("m.quality_score >= ?")
        params.append(args.min_quality)
    if args.moat:
        conditions.append("m.moat_width = ?")
        params.append(args.moat)
    if args.min_margin is not None:
        conditions.append("m.avg_op_margin_5y >= ?")
        params.append(args.min_margin / 100)

    where = " AND ".join(conditions) if conditions else "1=1"

    query = f"""
        SELECT c.ticker, c.name, m.revenue_cagr_5y, m.earnings_cagr_5y,
               m.avg_roic_5y, m.avg_gross_margin_5y, m.avg_op_margin_5y,
               m.piotroski_score, m.altman_zone, m.moat_width, m.quality_score,
               m.latest_revenue, m.latest_net_income, m.latest_fcf
        FROM companies c
        JOIN computed_metrics m ON c.ticker = m.ticker
        LEFT JOIN (
            SELECT ticker, de_ratio FROM annual_financials
            WHERE fiscal_year = (SELECT MAX(fiscal_year) FROM annual_financials af2 WHERE af2.ticker = annual_financials.ticker)
        ) af ON c.ticker = af.ticker
        WHERE {where}
        ORDER BY m.quality_score DESC, m.avg_roic_5y DESC
        LIMIT ?
    """
    params.append(args.limit or 50)

    results = db_query(conn, query, params)

    if not results:
        sprint(f"  {yellow('No companies match your criteria.')}")
        sprint(f"  {dim('Try relaxing filters or running: equity_kb.py build')}")
        return

    sprint(f"  {green(f'{len(results)} companies')} match your criteria:\n")
    sprint(f"    {'#':>3s} {'Ticker':<8s} {'Name':<25s} {'Rev CAGR':>9s} {'ROIC':>7s} {'OpMar':>7s} {'Moat':>6s} {'Qual':>5s} {'Pitr':>5s} {'Revenue':>10s}")
    sprint(f"    {'─'*3} {'─'*8} {'─'*25} {'─'*9} {'─'*7} {'─'*7} {'─'*6} {'─'*5} {'─'*5} {'─'*10}")

    for i, r in enumerate(results):
        moat_s = (r["moat_width"] or "—")[:4]
        sprint(f"    {i+1:>3d} {cyan(r['ticker']):<8s} {(r['name'] or '')[:25]:<25s} "
               f"{pct(r['revenue_cagr_5y']):>9s} {pct(r['avg_roic_5y']):>7s} "
               f"{pct(r['avg_op_margin_5y']):>7s} {moat_s:>6s} "
               f"{r['quality_score'] or 0:>5.0f} {r['piotroski_score'] or 0:>5d} "
               f"{usd(r['latest_revenue']):>10s}")

    sprint()

# ══════════════════════════════════════════════════════════════════════════════
# EXPORT command
# ══════════════════════════════════════════════════════════════════════════════

def cmd_export(args):
    conn = get_db()

    query = """
        SELECT c.ticker, c.name, c.sector, c.industry,
               m.revenue_cagr_3y, m.revenue_cagr_5y,
               m.earnings_cagr_3y, m.earnings_cagr_5y,
               m.avg_roic_5y, m.avg_roe_5y,
               m.avg_gross_margin_5y, m.avg_op_margin_5y,
               m.piotroski_score, m.altman_z, m.altman_zone,
               m.moat_score, m.moat_width, m.quality_score,
               m.latest_revenue, m.latest_net_income, m.latest_fcf,
               m.latest_equity, m.latest_total_debt
        FROM companies c
        JOIN computed_metrics m ON c.ticker = m.ticker
        ORDER BY c.ticker
    """
    rows = db_query(conn, query)

    if not rows:
        sprint(red("No data to export. Run: equity_kb.py build"))
        return

    if args.format == "json":
        data = [dict(r) for r in rows]
        output = json.dumps(data, indent=2, default=str)
    else:
        buf = io.StringIO()
        writer = csv.writer(buf)
        writer.writerow(rows[0].keys())
        for r in rows:
            writer.writerow(r)
        output = buf.getvalue()

    if args.output:
        with open(args.output, "w") as f:
            f.write(output)
        sprint(f"  Exported {len(rows)} companies to {args.output}")
    else:
        print(output)

# ══════════════════════════════════════════════════════════════════════════════
# STATS command
# ══════════════════════════════════════════════════════════════════════════════

def cmd_stats(args):
    conn = get_db()
    sprint(f"\n  {bold('Equity Knowledge Base Statistics')}")
    sprint(f"  {dim('Database:')} {DB_PATH}\n")

    companies = db_one(conn, "SELECT COUNT(*) as n FROM companies")
    with_edgar = db_one(conn, "SELECT COUNT(*) as n FROM companies WHERE edgar_fetched IS NOT NULL")
    financials = db_one(conn, "SELECT COUNT(*) as n FROM annual_financials")
    metrics = db_one(conn, "SELECT COUNT(*) as n FROM computed_metrics")
    notes = db_one(conn, "SELECT COUNT(*) as n FROM research_notes")
    embeddings = db_one(conn, "SELECT COUNT(*) as n FROM embeddings")
    jobs = db_one(conn, "SELECT COUNT(*) as n FROM research_jobs")

    n_companies = companies["n"]
    n_edgar = with_edgar["n"]
    n_fin = financials["n"]
    n_met = metrics["n"]
    n_notes = notes["n"]
    n_emb = embeddings["n"]
    n_jobs = jobs["n"]
    sprint(f"    Companies:           {green(f'{n_companies:,}')}")
    sprint(f"    With EDGAR data:     {green(f'{n_edgar:,}')}")
    sprint(f"    Annual financials:   {green(f'{n_fin:,}')} rows")
    sprint(f"    Computed metrics:    {green(f'{n_met:,}')}")
    sprint(f"    Research notes:      {green(f'{n_notes:,}')}")
    sprint(f"    Embeddings:          {green(f'{n_emb:,}')}")
    sprint(f"    Research jobs:       {green(f'{n_jobs:,}')}")

    # Quality distribution
    sprint(f"\n  {bold('Quality Distribution:')}")
    for label, lo, hi in [("Elite (80+)", 80, 200), ("Strong (60-79)", 60, 80),
                           ("Average (40-59)", 40, 60), ("Weak (<40)", 0, 40)]:
        cnt = db_one(conn, "SELECT COUNT(*) as n FROM computed_metrics WHERE quality_score >= ? AND quality_score < ?", (lo, hi))
        sprint(f"    {label:<20s} {cnt['n']:>6,}")

    # Moat distribution
    sprint(f"\n  {bold('Moat Distribution:')}")
    for width in ["Wide", "Narrow", "None"]:
        cnt = db_one(conn, "SELECT COUNT(*) as n FROM computed_metrics WHERE moat_width = ?", (width,))
        sprint(f"    {width:<10s} {cnt['n']:>6,}")

    # Top by quality
    sprint(f"\n  {bold('Top 10 by Quality Score:')}")
    top = db_query(conn, """
        SELECT c.ticker, c.name, m.quality_score, m.avg_roic_5y, m.moat_width, m.piotroski_score
        FROM companies c JOIN computed_metrics m ON c.ticker = m.ticker
        ORDER BY m.quality_score DESC LIMIT 10
    """)
    for r in top:
        sprint(f"    {cyan(r['ticker']):>6s} {(r['name'] or '')[:30]:<30s} "
               f"Q:{r['quality_score'] or 0:.0f}  ROIC:{pct(r['avg_roic_5y'])}  "
               f"Moat:{r['moat_width'] or '—'}  F:{r['piotroski_score'] or 0}")

    # DB file size
    db_size = os.path.getsize(DB_PATH) if os.path.exists(DB_PATH) else 0
    sprint(f"\n  {dim(f'Database size: {db_size/1e6:.1f} MB')}\n")

# ══════════════════════════════════════════════════════════════════════════════
# CLI
# ══════════════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="Equity Knowledge Base — Full-Universe Research Engine",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            Examples:
              equity_kb.py build --limit 100 --workers 8       # Build KB for first 100 tickers
              equity_kb.py build --ticker AAPL,MSFT,NVDA       # Build for specific tickers
              equity_kb.py update --enrich --ticker AAPL        # Update + Jina research
              equity_kb.py update --deep-research --ticker NVDA # + Parallel.ai deep research
              equity_kb.py search "high margin cloud SaaS"      # Semantic search
              equity_kb.py report AAPL --depth deep             # Full research report
              equity_kb.py screen --min-roic 20 --moat Wide     # Screen for quality
              equity_kb.py export --format csv -o universe.csv  # Export data
              equity_kb.py stats                                # KB statistics
        """))

    subs = parser.add_subparsers(dest="command")

    # build
    p = subs.add_parser("build", help="Build KB from SEC EDGAR")
    p.add_argument("--limit", type=int, help="Max tickers to process")
    p.add_argument("--ticker", help="Specific tickers (comma-separated)")
    p.add_argument("--sector", help="Filter by sector")
    p.add_argument("--exchange", help="Filter by exchange (NASDAQ, NYSE, etc.)")
    p.add_argument("--no-av", action="store_true", help="Skip Alpha Vantage enrichment")
    p.add_argument("--workers", type=int, default=8, help="Parallel workers (default: 8)")

    # update
    p = subs.add_parser("update", help="Update stale data & enrich")
    p.add_argument("--ticker", help="Specific tickers")
    p.add_argument("--stale-days", type=int, default=30, help="Days before data is stale")
    p.add_argument("--limit", type=int, default=500, help="Max tickers to update")
    p.add_argument("--enrich", action="store_true", help="Enrich with Jina web research")
    p.add_argument("--deep-research", action="store_true", help="Launch Parallel.ai deep research")

    # search
    p = subs.add_parser("search", help="Semantic search across KB")
    p.add_argument("query", help="Search query")

    # report
    p = subs.add_parser("report", help="Generate research report")
    p.add_argument("ticker", help="Stock ticker")
    p.add_argument("--depth", choices=["quick", "standard", "deep"], default="standard")

    # screen
    p = subs.add_parser("screen", help="Screen equities by criteria")
    p.add_argument("--min-roic", type=float, help="Min avg ROIC (%%)")
    p.add_argument("--min-revenue-cagr", type=float, help="Min revenue CAGR (%%)")
    p.add_argument("--max-de", type=float, help="Max D/E ratio")
    p.add_argument("--min-piotroski", type=int, help="Min Piotroski score")
    p.add_argument("--min-quality", type=float, help="Min quality score")
    p.add_argument("--moat", choices=["Wide", "Narrow", "None"], help="Moat width")
    p.add_argument("--min-margin", type=float, help="Min operating margin (%%)")
    p.add_argument("--limit", type=int, default=50, help="Max results")

    # export
    p = subs.add_parser("export", help="Export KB data")
    p.add_argument("--format", choices=["csv", "json"], default="csv")
    p.add_argument("-o", "--output", help="Output file (default: stdout)")

    # stats
    subs.add_parser("stats", help="Show KB statistics")

    # sources
    subs.add_parser("sources", help="Show scrape source health & reliability")

    args = parser.parse_args()
    if not args.command:
        parser.print_help()
        return

    def cmd_sources(args):
        conn = get_db()
        init_scrape_sources(conn)
        show_source_health(conn)
        sprint()

    {"build": cmd_build, "update": cmd_update, "search": cmd_search,
     "report": cmd_report, "screen": cmd_screen, "export": cmd_export,
     "stats": cmd_stats, "sources": cmd_sources}[args.command](args)

if __name__ == "__main__":
    main()
