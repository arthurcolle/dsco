#!/usr/bin/env python3
"""
Stock Valuation Script
======================
Multi-model valuation tool for any publicly traded ticker.
Uses Yahoo Finance data to compute intrinsic value estimates.

Usage: python3 valuation.py <TICKER>
Example: python3 valuation.py AAPL
"""

import sys
import json
import urllib.request
import urllib.error
from datetime import datetime

# ─── Colors ───────────────────────────────────────────────────────────────────
R  = "\033[0m"
B  = "\033[1m"
G  = "\033[32m"
RD = "\033[31m"
Y  = "\033[33m"
C  = "\033[36m"
DIM = "\033[2m"

def color_val(label, val, fmt=",.2f"):
    return f"  {DIM}{label:<30}{R} {B}${val:{fmt}}{R}"

def pct(val):
    sign = "+" if val >= 0 else ""
    color = G if val >= 0 else RD
    return f"{color}{sign}{val:.1f}%{R}"

# ─── Data Fetching ────────────────────────────────────────────────────────────
HEADERS = {"User-Agent": "Mozilla/5.0 (valuation-script)"}

def fetch_json(url):
    req = urllib.request.Request(url, headers=HEADERS)
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read().decode())

def get_yahoo_data(ticker):
    """Fetch quote + financials from Yahoo Finance v8 API."""
    modules = "financialData,defaultKeyStatistics,summaryDetail,incomeStatementHistory,balanceSheetHistory,cashflowStatementHistory,earningsTrend,price,summaryProfile"
    url = f"https://query1.finance.yahoo.com/v10/finance/quoteSummary/{ticker}?modules={modules}"
    data = fetch_json(url)
    return data["quoteSummary"]["result"][0]

def safe_get(d, *keys, default=None):
    """Safely navigate nested dicts with .get chaining."""
    curr = d
    for k in keys:
        if isinstance(curr, dict):
            curr = curr.get(k, {})
        else:
            return default
    if isinstance(curr, dict):
        return curr.get("raw", curr.get("fmt", default))
    return curr if curr else default

# ─── Valuation Models ────────────────────────────────────────────────────────

def dcf_valuation(fcf, growth_rate, terminal_growth=0.03, discount_rate=0.10, years=10, shares_out=1):
    """Discounted Cash Flow (DCF) model with 2-stage growth."""
    if not fcf or fcf <= 0 or not shares_out:
        return None
    
    # Stage 1: High growth (years 1-5), Stage 2: Fade to terminal (years 6-10)
    projected_fcfs = []
    for yr in range(1, years + 1):
        if yr <= 5:
            g = growth_rate
        else:
            # Linear fade from growth_rate to terminal_growth
            fade = (yr - 5) / 5
            g = growth_rate * (1 - fade) + terminal_growth * fade
        fcf = fcf * (1 + g)
        pv = fcf / (1 + discount_rate) ** yr
        projected_fcfs.append((yr, g, fcf, pv))
    
    # Terminal value (Gordon Growth)
    terminal_fcf = projected_fcfs[-1][2] * (1 + terminal_growth)
    terminal_value = terminal_fcf / (discount_rate - terminal_growth)
    pv_terminal = terminal_value / (1 + discount_rate) ** years
    
    total_pv = sum(pv for _, _, _, pv in projected_fcfs) + pv_terminal
    fair_value = total_pv / shares_out
    
    return {
        "fair_value": fair_value,
        "total_pv_fcfs": sum(pv for _, _, _, pv in projected_fcfs),
        "pv_terminal": pv_terminal,
        "projected_fcfs": projected_fcfs,
        "terminal_growth": terminal_growth,
        "discount_rate": discount_rate,
    }

def graham_valuation(eps, growth_rate, aaa_yield=0.0475):
    """Benjamin Graham intrinsic value formula (revised)."""
    if not eps or eps <= 0 or not growth_rate:
        return None
    # V = EPS × (8.5 + 2g) × 4.4 / Y
    g = growth_rate * 100  # Convert to percentage
    value = eps * (8.5 + 2 * g) * 4.4 / (aaa_yield * 100)
    return value

def peter_lynch_valuation(eps, growth_rate, dividend_yield=0):
    """Peter Lynch PEG-based fair value."""
    if not eps or eps <= 0 or not growth_rate or growth_rate <= 0:
        return None
    # Fair P/E = growth rate (%) + dividend yield (%)
    fair_pe = (growth_rate * 100) + (dividend_yield * 100)
    fair_pe = min(fair_pe, 50)  # Cap at 50x
    return eps * fair_pe

def earnings_power_value(ebit, tax_rate, wacc, shares_out):
    """Earnings Power Value (EPV) — Greenwald method."""
    if not ebit or ebit <= 0 or not wacc or not shares_out:
        return None
    nopat = ebit * (1 - tax_rate)
    epv = nopat / wacc
    return epv / shares_out

def relative_valuation(eps, sector_pe=20):
    """Simple relative valuation using sector average P/E."""
    if not eps or eps <= 0:
        return None
    return eps * sector_pe

def dividend_discount_model(dividend, growth_rate, discount_rate=0.10):
    """Gordon Growth / DDM for dividend-paying stocks."""
    if not dividend or dividend <= 0 or growth_rate >= discount_rate:
        return None
    return dividend / (discount_rate - growth_rate)

def asset_based_valuation(total_assets, total_liabilities, shares_out):
    """Net Asset Value (Book Value) per share."""
    if not total_assets or not shares_out:
        return None
    nav = (total_assets - (total_liabilities or 0))
    return nav / shares_out

# ─── Main ─────────────────────────────────────────────────────────────────────

def run_valuation(ticker):
    ticker = ticker.upper()
    print(f"\n{B}{'═' * 60}{R}")
    print(f"{B}  📊 STOCK VALUATION REPORT — {C}{ticker}{R}")
    print(f"{B}{'═' * 60}{R}")
    print(f"  {DIM}Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}{R}\n")

    # ── Fetch Data ────────────────────────────────────────────────────────────
    try:
        data = get_yahoo_data(ticker)
    except Exception as e:
        print(f"  {RD}✗ Failed to fetch data for {ticker}: {e}{R}")
        sys.exit(1)

    fin   = data.get("financialData", {})
    stats = data.get("defaultKeyStatistics", {})
    summ  = data.get("summaryDetail", {})
    price_data = data.get("price", {})
    profile = data.get("summaryProfile", {})

    # Extract key metrics
    current_price   = safe_get(fin, "currentPrice", default=safe_get(price_data, "regularMarketPrice"))
    market_cap      = safe_get(price_data, "marketCap") or safe_get(summ, "marketCap")
    shares_out      = safe_get(stats, "sharesOutstanding") or (market_cap / current_price if market_cap and current_price else None)
    
    eps_trailing    = safe_get(stats, "trailingEps") or safe_get(summ, "trailingEps")
    eps_forward     = safe_get(stats, "forwardEps")
    pe_trailing     = safe_get(summ, "trailingPE")
    pe_forward      = safe_get(summ, "forwardPE") or safe_get(stats, "forwardPE")
    peg_ratio       = safe_get(stats, "pegRatio")
    pb_ratio        = safe_get(stats, "priceToBook")
    
    revenue         = safe_get(fin, "totalRevenue")
    ebitda          = safe_get(fin, "ebitda")
    free_cashflow   = safe_get(fin, "freeCashflow")
    op_cashflow     = safe_get(fin, "operatingCashflow")
    profit_margin   = safe_get(fin, "profitMargins")
    revenue_growth  = safe_get(fin, "revenueGrowth")
    earnings_growth = safe_get(fin, "earningsGrowth")
    roe             = safe_get(fin, "returnOnEquity")
    debt_to_equity  = safe_get(fin, "debtToEquity")
    
    dividend_rate   = safe_get(summ, "dividendRate")
    dividend_yield  = safe_get(summ, "dividendYield")
    beta            = safe_get(stats, "beta") or safe_get(summ, "beta")
    
    book_value      = safe_get(stats, "bookValue")
    enterprise_val  = safe_get(stats, "enterpriseValue") or safe_get(summ, "enterpriseValue")
    
    # Estimate growth rate
    growth_5y = safe_get(stats, "earningsQuarterlyGrowth")
    analyst_growth = None
    trend = data.get("earningsTrend", {}).get("trend", [])
    for t in trend:
        if t.get("period") == "+5y":
            analyst_growth = safe_get(t, "growth")
            break
    
    est_growth = analyst_growth or earnings_growth or revenue_growth or 0.08
    est_growth = max(min(est_growth, 0.40), 0.01)  # Clamp 1%-40%

    # Balance sheet
    bs_list = data.get("balanceSheetHistory", {}).get("balanceSheetStatements", [])
    total_assets = total_liabilities = None
    if bs_list:
        latest_bs = bs_list[0]
        total_assets = safe_get(latest_bs, "totalAssets")
        total_liabilities = safe_get(latest_bs, "totalLiab")

    # Income statement for EBIT
    is_list = data.get("incomeStatementHistory", {}).get("incomeStatementHistory", [])
    ebit = None
    tax_rate = 0.21
    if is_list:
        latest_is = is_list[0]
        ebit = safe_get(latest_is, "ebit")
        tax_provision = safe_get(latest_is, "incomeTaxExpense")
        pretax_income = safe_get(latest_is, "incomeBeforeTax")
        if tax_provision and pretax_income and pretax_income > 0:
            tax_rate = max(0.05, min(tax_provision / pretax_income, 0.40))

    # Company info
    name = safe_get(price_data, "longName") or safe_get(price_data, "shortName") or ticker
    sector = safe_get(profile, "sector") or "N/A"
    industry = safe_get(profile, "industry") or "N/A"

    # ── Print Company Overview ────────────────────────────────────────────────
    print(f"  {B}Company:{R}  {name}")
    print(f"  {B}Sector:{R}   {sector}  •  {industry}")
    if current_price:
        print(f"  {B}Price:{R}    ${current_price:,.2f}", end="")
        if market_cap:
            mc_b = market_cap / 1e9
            print(f"  •  Mkt Cap: ${mc_b:,.1f}B", end="")
        print()
    print()

    # ── Print Key Metrics ─────────────────────────────────────────────────────
    print(f"  {B}{C}── KEY METRICS ──{R}")
    metrics = []
    if eps_trailing:   metrics.append(f"EPS (TTM): ${eps_trailing:.2f}")
    if eps_forward:    metrics.append(f"EPS (Fwd): ${eps_forward:.2f}")
    if pe_trailing:    metrics.append(f"P/E (TTM): {pe_trailing:.1f}x")
    if pe_forward:     metrics.append(f"P/E (Fwd): {pe_forward:.1f}x")
    if peg_ratio:      metrics.append(f"PEG: {peg_ratio:.2f}")
    if pb_ratio:       metrics.append(f"P/B: {pb_ratio:.2f}x")
    if beta:           metrics.append(f"Beta: {beta:.2f}")
    if roe:            metrics.append(f"ROE: {roe*100:.1f}%")
    if profit_margin:  metrics.append(f"Margin: {profit_margin*100:.1f}%")
    if debt_to_equity: metrics.append(f"D/E: {debt_to_equity:.0f}%")
    if revenue_growth: metrics.append(f"Rev Growth: {revenue_growth*100:.1f}%")
    if dividend_yield: metrics.append(f"Div Yield: {dividend_yield*100:.2f}%")
    if free_cashflow:  metrics.append(f"FCF: ${free_cashflow/1e9:.2f}B")
    
    for i in range(0, len(metrics), 3):
        row = metrics[i:i+3]
        print(f"  {DIM}{'   •   '.join(row)}{R}")
    
    print(f"\n  {DIM}Estimated Growth Rate: {est_growth*100:.1f}% (5yr analyst / earnings / revenue){R}")

    # ── WACC Estimate ─────────────────────────────────────────────────────────
    risk_free = 0.043  # ~10yr Treasury
    market_premium = 0.055
    beta_val = beta if beta else 1.0
    cost_of_equity = risk_free + beta_val * market_premium
    wacc = cost_of_equity  # Simplified; full WACC needs debt cost
    if debt_to_equity and debt_to_equity > 0:
        de = debt_to_equity / 100
        cost_of_debt = 0.055  # Approximation
        equity_weight = 1 / (1 + de)
        debt_weight = de / (1 + de)
        wacc = equity_weight * cost_of_equity + debt_weight * cost_of_debt * (1 - tax_rate)

    # ══════════════════════════════════════════════════════════════════════════
    # ── VALUATION MODELS ─────────────────────────────────────────────────────
    # ══════════════════════════════════════════════════════════════════════════
    print(f"\n{B}{'─' * 60}{R}")
    print(f"  {B}{C}── VALUATION MODELS ──{R}\n")

    results = []

    # 1) DCF
    dcf = dcf_valuation(
        fcf=free_cashflow or (op_cashflow * 0.8 if op_cashflow else None),
        growth_rate=est_growth,
        terminal_growth=0.03,
        discount_rate=max(wacc, 0.08),
        years=10,
        shares_out=shares_out
    )
    if dcf:
        fv = dcf["fair_value"]
        results.append(("DCF (2-Stage)", fv))
        upside = ((fv / current_price) - 1) * 100 if current_price else 0
        print(f"  {B}1. Discounted Cash Flow (DCF){R}")
        print(f"     Discount Rate (WACC): {wacc*100:.1f}%  •  Terminal Growth: 3.0%")
        print(f"     PV of FCFs: ${dcf['total_pv_fcfs']/1e9:.2f}B  •  PV Terminal: ${dcf['pv_terminal']/1e9:.2f}B")
        print(f"     {G}▸ Fair Value: ${fv:,.2f}{R}  ({pct(upside)} vs current)\n")
    else:
        print(f"  {DIM}1. DCF — Skipped (no FCF data){R}\n")

    # 2) Graham
    graham = graham_valuation(eps_trailing, est_growth)
    if graham:
        results.append(("Graham Formula", graham))
        upside = ((graham / current_price) - 1) * 100 if current_price else 0
        print(f"  {B}2. Benjamin Graham Formula{R}")
        print(f"     V = EPS × (8.5 + 2g) × 4.4 / Y")
        print(f"     {G}▸ Fair Value: ${graham:,.2f}{R}  ({pct(upside)} vs current)\n")
    else:
        print(f"  {DIM}2. Graham — Skipped (no positive EPS){R}\n")

    # 3) Peter Lynch PEG
    lynch = peter_lynch_valuation(eps_trailing, est_growth, dividend_yield or 0)
    if lynch:
        results.append(("Lynch PEG", lynch))
        upside = ((lynch / current_price) - 1) * 100 if current_price else 0
        print(f"  {B}3. Peter Lynch (PEG-Based){R}")
        fair_pe = (est_growth * 100) + ((dividend_yield or 0) * 100)
        print(f"     Fair P/E = Growth% + DivYield% = {fair_pe:.1f}x")
        print(f"     {G}▸ Fair Value: ${lynch:,.2f}{R}  ({pct(upside)} vs current)\n")
    else:
        print(f"  {DIM}3. Lynch PEG — Skipped (no positive EPS/growth){R}\n")

    # 4) Earnings Power Value
    epv = earnings_power_value(ebit, tax_rate, max(wacc, 0.08), shares_out)
    if epv:
        results.append(("EPV (Greenwald)", epv))
        upside = ((epv / current_price) - 1) * 100 if current_price else 0
        print(f"  {B}4. Earnings Power Value (EPV){R}")
        print(f"     EBIT: ${ebit/1e9:.2f}B  •  Tax: {tax_rate*100:.0f}%  •  WACC: {wacc*100:.1f}%")
        print(f"     {G}▸ Fair Value: ${epv:,.2f}{R}  ({pct(upside)} vs current)\n")
    else:
        print(f"  {DIM}4. EPV — Skipped (no EBIT data){R}\n")

    # 5) DDM
    ddm = dividend_discount_model(dividend_rate, min(est_growth, wacc - 0.01), max(wacc, 0.08))
    if ddm:
        results.append(("DDM (Gordon)", ddm))
        upside = ((ddm / current_price) - 1) * 100 if current_price else 0
        print(f"  {B}5. Dividend Discount Model (DDM){R}")
        print(f"     Annual Dividend: ${dividend_rate:.2f}  •  Div Growth: {min(est_growth, wacc-0.01)*100:.1f}%")
        print(f"     {G}▸ Fair Value: ${ddm:,.2f}{R}  ({pct(upside)} vs current)\n")
    else:
        print(f"  {DIM}5. DDM — Skipped (no dividend or growth ≥ discount rate){R}\n")

    # 6) Book Value / NAV
    nav = asset_based_valuation(total_assets, total_liabilities, shares_out)
    if nav:
        results.append(("Net Asset Value", nav))
        upside = ((nav / current_price) - 1) * 100 if current_price else 0
        print(f"  {B}6. Net Asset Value (Book Value){R}")
        if total_assets and total_liabilities:
            print(f"     Assets: ${total_assets/1e9:.1f}B  •  Liabilities: ${total_liabilities/1e9:.1f}B")
        print(f"     {G}▸ Fair Value: ${nav:,.2f}{R}  ({pct(upside)} vs current)\n")
    else:
        print(f"  {DIM}6. NAV — Skipped (no balance sheet data){R}\n")

    # 7) Relative Valuation (Forward P/E)
    if eps_forward and pe_forward:
        # Use sector median P/E if available; else 20x
        sector_pe = 20
        rel_val = eps_forward * sector_pe
        results.append(("Relative (20x Fwd P/E)", rel_val))
        upside = ((rel_val / current_price) - 1) * 100 if current_price else 0
        print(f"  {B}7. Relative Valuation{R}")
        print(f"     Forward EPS: ${eps_forward:.2f}  ×  Sector Avg P/E: {sector_pe}x")
        print(f"     {G}▸ Fair Value: ${rel_val:,.2f}{R}  ({pct(upside)} vs current)\n")

    # ══════════════════════════════════════════════════════════════════════════
    # ── COMPOSITE FAIR VALUE ─────────────────────────────────────────────────
    # ══════════════════════════════════════════════════════════════════════════
    print(f"{B}{'─' * 60}{R}")
    print(f"  {B}{C}── COMPOSITE FAIR VALUE ──{R}\n")

    if results:
        # Weighted average (DCF gets 2x weight)
        weights = {"DCF (2-Stage)": 2.5, "Graham Formula": 1.5, "Lynch PEG": 1.5,
                    "EPV (Greenwald)": 1.5, "DDM (Gordon)": 1.0, "Net Asset Value": 0.5,
                    "Relative (20x Fwd P/E)": 1.0}
        
        total_w = sum(weights.get(name, 1.0) for name, _ in results)
        weighted_sum = sum(val * weights.get(name, 1.0) for name, val in results)
        composite = weighted_sum / total_w

        simple_avg = sum(v for _, v in results) / len(results)
        median_val = sorted(v for _, v in results)[len(results) // 2]

        # Print individual model summary
        print(f"  {'Model':<28} {'Fair Value':>12}  {'vs Price':>10}")
        print(f"  {'─' * 54}")
        for name, val in results:
            upside = ((val / current_price) - 1) * 100 if current_price else 0
            w = weights.get(name, 1.0)
            bar = "█" * max(1, int(w * 4))
            print(f"  {name:<28} {f'${val:>10,.2f}':>12}  {pct(upside):>18}  {DIM}{bar} {w}x{R}")

        print(f"  {'─' * 54}")
        if current_price:
            composite_upside = ((composite / current_price) - 1) * 100
            print(f"\n  {B}Weighted Average:  ${composite:>10,.2f}   {pct(composite_upside)} vs ${current_price:,.2f}{R}")
            print(f"  {DIM}Simple Average:    ${simple_avg:>10,.2f}   {pct(((simple_avg/current_price)-1)*100)}{R}")
            print(f"  {DIM}Median:            ${median_val:>10,.2f}   {pct(((median_val/current_price)-1)*100)}{R}")

            # Margin of Safety
            mos_price = composite * 0.75
            print(f"\n  {Y}🛡  Margin of Safety (25%): ${mos_price:,.2f}{R}")

            # Final Verdict
            print(f"\n  {B}{'─' * 40}{R}")
            if composite_upside > 20:
                verdict = f"{G}★ UNDERVALUED — Potential Buy ★{R}"
            elif composite_upside > 0:
                verdict = f"{Y}◉ FAIRLY VALUED — Hold / Watch{R}"
            elif composite_upside > -15:
                verdict = f"{Y}◉ SLIGHTLY OVERVALUED — Caution{R}"
            else:
                verdict = f"{RD}✗ OVERVALUED — Consider Avoiding{R}"
            print(f"  {B}Verdict:{R}  {verdict}")
            print(f"  {B}{'─' * 40}{R}")
    else:
        print(f"  {RD}No valuation models could be computed (insufficient data).{R}")

    # ── Disclaimer ────────────────────────────────────────────────────────────
    print(f"\n  {DIM}⚠  This is an automated estimate, NOT financial advice.")
    print(f"  Models use simplified assumptions. Always do your own research.{R}")
    print(f"{B}{'═' * 60}{R}\n")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: python3 {sys.argv[0]} <TICKER>")
        print(f"Example: python3 {sys.argv[0]} AAPL")
        sys.exit(1)
    run_valuation(sys.argv[1])
