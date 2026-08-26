#!/usr/bin/env python3
"""
Unified ingester for three prediction-market venues:
  - Kalshi      (https://api.elections.kalshi.com)
  - Polymarket  (https://gamma-api.polymarket.com)
  - PredictIt   (https://www.predictit.org/api)

Normalizes to one schema, writes CSV + Parquet, prints cross-venue stats.
"""

from __future__ import annotations

import json
import sys
import time
import urllib.request
from datetime import datetime, timezone

import pandas as pd

UA = {"User-Agent": "dsco/1.0 (+research)"}
TIMEOUT = 30
OUT_CSV = "/Users/arthurcolle/venues_all.csv"
OUT_PARQUET = "/Users/arthurcolle/venues_all.parquet"


def _get(url: str) -> dict | list:
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return json.loads(r.read().decode("utf-8"))


# ─────────────────────────────────────────────────────────────────────────────
# Kalshi
# ─────────────────────────────────────────────────────────────────────────────
def ingest_kalshi(max_pages: int = 30) -> pd.DataFrame:
    """Fetch Kalshi open markets. Real schema uses *_dollars (0-1) and *_fp suffixes."""
    print("  ↓ Kalshi", file=sys.stderr)
    base = "https://api.elections.kalshi.com/trade-api/v2/markets?status=open&limit=1000"
    rows, cursor = [], None
    for page in range(max_pages):
        url = base + (f"&cursor={cursor}" if cursor else "")
        try:
            d = _get(url)
        except Exception as e:
            print(f"    ✗ {e}", file=sys.stderr); break
        markets = d.get("markets", []) if isinstance(d, dict) else []
        for m in markets:
            try:
                # Skip multi-leg provisional parlays that don't trade
                if m.get("is_provisional"):
                    continue
                ya = float(m.get("yes_ask_dollars") or 0) * 100
                yb = float(m.get("yes_bid_dollars") or 0) * 100
                na = float(m.get("no_ask_dollars") or 0) * 100
                nb = float(m.get("no_bid_dollars") or 0) * 100
                last = float(m.get("last_price_dollars") or 0) * 100
                vol = float(m.get("volume_fp") or 0)
                vol_24 = float(m.get("volume_24h_fp") or 0)
                oi = float(m.get("open_interest_fp") or 0)
                liq = float(m.get("liquidity_dollars") or 0)
                rows.append({
                    "venue": "Kalshi",
                    "market_id": m.get("ticker"),
                    "event_id": m.get("event_ticker"),
                    "question": (m.get("title") or "") + (
                        f" — {m['yes_sub_title']}" if m.get("yes_sub_title") else ""),
                    "contract": "YES",
                    "yes_ask_c": ya,
                    "yes_bid_c": yb,
                    "no_ask_c":  na,
                    "no_bid_c":  nb,
                    "last_c":    last,
                    "volume_24h_$":  vol_24,    # contracts, not $, but uniform-enough
                    "open_interest_$": oi,
                    "liquidity_$": liq,
                    "close_time": m.get("close_time"),
                    "url": f"https://kalshi.com/markets/{m.get('event_ticker','')}/{m.get('ticker','')}",
                })
            except Exception:
                pass
        cursor = (d.get("cursor") if isinstance(d, dict) else None) or None
        if not cursor:
            break
    print(f"    ✓ Kalshi: {len(rows)} markets (after filtering provisional)", file=sys.stderr)
    return pd.DataFrame(rows)


# ─────────────────────────────────────────────────────────────────────────────
# Polymarket
# ─────────────────────────────────────────────────────────────────────────────
def ingest_polymarket(max_pages: int = 6, page_size: int = 500) -> pd.DataFrame:
    print("  ↓ Polymarket", file=sys.stderr)
    rows = []
    for page in range(max_pages):
        url = (f"https://gamma-api.polymarket.com/markets?"
               f"closed=false&active=true&order=volume24hr&ascending=false"
               f"&limit={page_size}&offset={page*page_size}")
        try:
            data = _get(url)
        except Exception as e:
            print(f"    ✗ {e}", file=sys.stderr); break
        if not data:
            break
        for m in data:
            try:
                outs = json.loads(m.get("outcomes") or "[]")
                prices = json.loads(m.get("outcomePrices") or "[]")
                for name, p in zip(outs, prices):
                    p_f = float(p or 0)
                    rows.append({
                        "venue": "Polymarket",
                        "market_id": m.get("id"),
                        "event_id": m.get("conditionId"),
                        "question": m.get("question"),
                        "contract": name,
                        "yes_ask_c": round(p_f * 100, 1),
                        "yes_bid_c": round(p_f * 100, 1),  # gamma doesn't expose bid/ask separately
                        "no_ask_c":  round((1 - p_f) * 100, 1),
                        "no_bid_c":  round((1 - p_f) * 100, 1),
                        "last_c":    round(p_f * 100, 1),
                        "volume_24h_$":  float(m.get("volume24hr") or 0),
                        "open_interest_$": float(m.get("openInterest") or 0),
                        "liquidity_$": float(m.get("liquidityNum") or 0),
                        "close_time": m.get("endDate"),
                        "url": f"https://polymarket.com/market/{m.get('slug','')}",
                    })
            except Exception:
                pass
        time.sleep(0.2)
    print(f"    ✓ Polymarket: {len(rows)} contracts", file=sys.stderr)
    return pd.DataFrame(rows)


# ─────────────────────────────────────────────────────────────────────────────
# PredictIt
# ─────────────────────────────────────────────────────────────────────────────
def ingest_predictit() -> pd.DataFrame:
    print("  ↓ PredictIt", file=sys.stderr)
    try:
        d = _get("https://www.predictit.org/api/marketdata/all/")
    except Exception as e:
        print(f"    ✗ {e}", file=sys.stderr)
        return pd.DataFrame()
    rows = []
    for m in d.get("markets", []):
        for c in m.get("contracts", []):
            try:
                rows.append({
                    "venue": "PredictIt",
                    "market_id": f"{m['id']}",
                    "event_id":  f"{m['id']}",
                    "question":  m.get("name"),
                    "contract":  c.get("name") or c.get("shortName"),
                    "yes_ask_c": round(float(c.get("bestBuyYesCost") or 0) * 100, 1),
                    "yes_bid_c": round(float(c.get("bestSellYesCost") or 0) * 100, 1),
                    "no_ask_c":  round(float(c.get("bestBuyNoCost") or 0) * 100, 1),
                    "no_bid_c":  round(float(c.get("bestSellNoCost") or 0) * 100, 1),
                    "last_c":    round(float(c.get("lastTradePrice") or 0) * 100, 1),
                    "volume_24h_$":  0,   # PredictIt doesn't expose volume here
                    "open_interest_$": 0,
                    "liquidity_$": 0,
                    "close_time": c.get("dateEnd") if c.get("dateEnd") not in (None, "NA") else None,
                    "url": m.get("url"),
                })
            except Exception:
                pass
    print(f"    ✓ PredictIt: {len(rows)} contracts", file=sys.stderr)
    return pd.DataFrame(rows)


# ─────────────────────────────────────────────────────────────────────────────
# Driver
# ─────────────────────────────────────────────────────────────────────────────
def main() -> None:
    t0 = time.time()
    print(f"=== Venue ingest @ {datetime.now(timezone.utc).isoformat()} ===", file=sys.stderr)

    frames = [ingest_kalshi(), ingest_polymarket(), ingest_predictit()]
    df = pd.concat(frames, ignore_index=True)
    df["spread_c"] = (df["no_ask_c"] + df["yes_ask_c"]) - 100  # implied vig if 2-sided
    df["mid_c"] = (df["yes_ask_c"] + df["yes_bid_c"]) / 2

    df.to_csv(OUT_CSV, index=False)
    try:
        df.to_parquet(OUT_PARQUET, index=False)
        parquet_msg = f" + parquet ({OUT_PARQUET})"
    except Exception:
        parquet_msg = " (parquet skipped — install pyarrow)"

    dt = time.time() - t0
    print(f"\n  wrote {len(df):,} rows to {OUT_CSV}{parquet_msg}  in {dt:.1f}s\n", file=sys.stderr)

    # ── Summary ─────────────────────────────────────────────────────────────
    # "Tradeable" = both sides quoted with a real two-way market (not orderbook-empty defaults)
    tr = df[(df["yes_ask_c"] > 0) & (df["yes_ask_c"] < 100) &
            (df["no_ask_c"] > 0) & (df["no_ask_c"] < 100)].copy()

    print("══ Per-venue summary (all markets) ══")
    summary = df.groupby("venue").agg(
        contracts=("market_id", "count"),
        unique_markets=("event_id", "nunique"),
        vol_24h_total=("volume_24h_$", "sum"),
        liq_total=("liquidity_$", "sum"),
    ).round(1)
    print(summary.to_string())

    print("\n══ Per-venue summary (TRADEABLE: 0 < yes_ask < 100 both sides) ══")
    tr_summary = tr.groupby("venue").agg(
        contracts=("market_id", "count"),
        med_yes_ask=("yes_ask_c", "median"),
        med_spread=("spread_c", "median"),
        med_vol24=("volume_24h_$", "median"),
        max_vol24=("volume_24h_$", "max"),
    ).round(2)
    print(tr_summary.to_string())

    # Top by 24h volume — global
    top = (tr.sort_values("volume_24h_$", ascending=False)
             .head(15)[["venue", "question", "contract", "yes_ask_c", "volume_24h_$"]])
    if len(top):
        print("\n══ Top 15 tradeable contracts by 24h volume (cross-venue) ══")
        print(top.to_string(index=False, max_colwidth=60))

    # Top by 24h volume — per venue
    for v in ["Kalshi", "Polymarket", "PredictIt"]:
        sub = (tr[tr["venue"] == v]
               .sort_values("volume_24h_$", ascending=False)
               .head(5)[["question", "contract", "yes_ask_c", "no_ask_c", "volume_24h_$"]])
        if len(sub):
            print(f"\n── {v} top 5 by 24h volume ──")
            print(sub.to_string(index=False, max_colwidth=55))

    # Arthur's rule: YES ≤40c with real volume
    cheap = (tr[(tr["yes_ask_c"] <= 40) & (tr["volume_24h_$"] > 1000)]
             .sort_values("volume_24h_$", ascending=False)
             .head(15)[["venue", "question", "contract", "yes_ask_c", "no_ask_c",
                        "spread_c", "volume_24h_$"]])
    if len(cheap):
        print("\n══ ≤40c YES contracts with >$1k 24h volume — top 15 ══")
        print(cheap.to_string(index=False, max_colwidth=55))


if __name__ == "__main__":
    main()
