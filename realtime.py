#!/usr/bin/env python3
"""
Real-time NWP + METAR + Kalshi Trading Dashboard
==================================================
Fuses: latest METAR obs, hourly temp history, multi-model NWP forecasts,
NWS hourly forecast, and live Kalshi bucket pricing into a single view.

Usage:
  python3 realtime.py                     # 20-city overview
  python3 realtime.py --city nyc chi phx  # focused cities
  python3 realtime.py --deep nyc          # single-city deep dive with buckets
  python3 realtime.py --loop 5            # auto-refresh every 5 min
"""

import json
import math
import re
import time
import urllib.request
import urllib.error
from datetime import datetime, timedelta, timezone
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Optional

from nwp_pipeline import KALSHI_CITIES, fetch_bufkit, NWPEnsemble

try:
    from calibration import calibrate_city, CalibratedDistribution, EdgeCalculator
    HAS_CALIB = True
except ImportError:
    HAS_CALIB = False

try:
    from rich.console import Console
    from rich.table import Table
    from rich.panel import Panel
    from rich.text import Text
    from rich.layout import Layout
    from rich import box
    HAS_RICH = True
except ImportError:
    HAS_RICH = False

# ── API endpoints ─────────────────────────────────────────────────────

METAR_URL = "https://aviationweather.gov/api/data/metar?ids={ids}&format=raw&hours={hours}"
NWS_OBS_URL = "https://api.weather.gov/stations/{icao}/observations?limit={limit}"
NWS_HOURLY_URL = "https://api.weather.gov/gridpoints/{wfo}/{gx},{gy}/forecastHourly"
NWS_POINT_URL = "https://api.weather.gov/points/{lat},{lon}"
KALSHI_BASE = "https://api.elections.kalshi.com/trade-api/v2"
HEADERS = {"User-Agent": "dsco-rt/2.0", "Accept": "application/geo+json"}
KALSHI_HEADERS = {"Accept": "application/json"}


def _fetch_json(url, headers=None, timeout=12):
    try:
        req = urllib.request.Request(url, headers=headers or HEADERS)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read())
    except:
        return None


def _fetch_text(url, timeout=10):
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "dsco-rt/2.0"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read().decode("utf-8", errors="replace")
    except:
        return ""


# ══════════════════════════════════════════════════════════════════════
#  METAR + Observations
# ══════════════════════════════════════════════════════════════════════

def fetch_all_metars(icaos: list[str], hours: int = 24) -> dict[str, list[dict]]:
    """Fetch METARs for all stations, parse into structured dicts."""
    text = _fetch_text(METAR_URL.format(ids=",".join(icaos), hours=hours))
    result = {}
    for line in text.strip().split("\n"):
        line = line.strip()
        if not line or "No METAR" in line:
            continue
        parts = line.split()
        if parts[0] in ("METAR", "SPECI"):
            parts = parts[1:]
        if len(parts) < 3:
            continue
        stn = parts[0]

        # Parse time
        obs_time = None
        for p in parts:
            if re.match(r'\d{6}Z$', p):
                now = datetime.now(timezone.utc)
                try:
                    obs_time = now.replace(day=int(p[:2]), hour=int(p[2:4]),
                                           minute=int(p[4:6]), second=0, microsecond=0)
                    if obs_time > now + timedelta(hours=1):
                        obs_time -= timedelta(days=28)
                except:
                    pass

        # Parse temp/dewpoint
        temp_f = dp_f = None
        m = re.search(r'\s(M?\d{2})/(M?\d{2})\s', line)
        if m:
            def _c(s): return -int(s[1:]) if s[0] == 'M' else int(s)
            temp_f = round(_c(m.group(1)) * 9/5 + 32, 1)
            dp_f = round(_c(m.group(2)) * 9/5 + 32, 1)

        # Parse wind
        wind_dir = wind_spd = wind_gust = 0
        wm = re.search(r'(\d{3}|VRB)(\d{2,3})(?:G(\d{2,3}))?KT', line)
        if wm:
            wind_dir = 0 if wm.group(1) == "VRB" else int(wm.group(1))
            wind_spd = int(wm.group(2))
            wind_gust = int(wm.group(3)) if wm.group(3) else 0

        # Parse altimeter
        altim = None
        am = re.search(r'A(\d{4})', line)
        if am:
            altim = int(am.group(1)) / 100.0

        # Parse visibility
        vis = None
        vm = re.search(r'(\d+)SM', line)
        if vm:
            vis = int(vm.group(1))

        # Parse sky condition
        sky = ""
        for cond in ["SKC", "CLR", "FEW", "SCT", "BKN", "OVC", "VV"]:
            if cond in line:
                sky = cond
                break

        # Wx phenomena
        wx = ""
        for phen in ["-RA", "+RA", "RA", "-SN", "+SN", "SN", "TS", "FG", "BR", "HZ", "DZ", "-DZ", "FZRA"]:
            if f" {phen} " in f" {line} ":
                wx = phen
                break

        # Remarks — look for precise temp (T01330067 = 13.3C/6.7C)
        rm = re.search(r'T(\d)(\d{3})(\d)(\d{3})', line)
        if rm:
            t_sign = -1 if rm.group(1) == '1' else 1
            d_sign = -1 if rm.group(3) == '1' else 1
            temp_f = round((t_sign * int(rm.group(2)) / 10) * 9/5 + 32, 1)
            dp_f = round((d_sign * int(rm.group(4)) / 10) * 9/5 + 32, 1)

        entry = {
            "time": obs_time, "temp_f": temp_f, "dp_f": dp_f,
            "wind_dir": wind_dir, "wind_spd": wind_spd, "wind_gust": wind_gust,
            "altim": altim, "vis": vis, "sky": sky, "wx": wx, "raw": line,
        }

        if stn not in result:
            result[stn] = []
        result[stn].append(entry)

    return result


def compute_obs_stats(obs_list: list[dict], today: datetime) -> dict:
    """From 24h of METARs, compute today's running max, min, trend."""
    today_obs = [o for o in obs_list if o["time"] and o["time"].date() == today.date() and o["temp_f"] is not None]
    if not today_obs:
        return {}

    temps = [o["temp_f"] for o in today_obs]
    times = [o["time"] for o in today_obs]

    max_t = max(temps)
    min_t = min(temps)
    max_time = times[temps.index(max_t)]
    latest = today_obs[0]  # most recent first from aviationweather

    # Trend: compare last 3 hours
    recent = [o["temp_f"] for o in today_obs[:3]]
    trend = 0
    if len(recent) >= 2:
        trend = recent[0] - recent[-1]  # positive = warming

    return {
        "obs_max": max_t, "obs_min": min_t,
        "obs_max_time": max_time.strftime("%H:%MZ"),
        "obs_count": len(today_obs),
        "current": latest["temp_f"],
        "current_dp": latest.get("dp_f"),
        "current_time": latest["time"],
        "trend_3h": round(trend, 1),
        "wind": (latest.get("wind_dir", 0), latest.get("wind_spd", 0), latest.get("wind_gust", 0)),
        "sky": latest.get("sky", ""),
        "wx": latest.get("wx", ""),
        "altim": latest.get("altim"),
    }


# ══════════════════════════════════════════════════════════════════════
#  NWP Forecast High (multi-model)
# ══════════════════════════════════════════════════════════════════════

def fetch_model_highs(city_key: str, target_date: datetime) -> dict[str, float]:
    """Fetch forecast high from HRRR, NAM, GFS for a city. Returns {model: high_f}."""
    now = datetime.now(timezone.utc)
    highs = {}
    city = KALSHI_CITIES[city_key]
    lat, lon = city[3], city[4]

    for model in ["hrrr", "nam", "gfs"]:
        from nwp_pipeline import MODELS
        cfg = MODELS[model]

        # Build candidate cycles: walk backwards from current hour
        # For hourly models (HRRR/RAP), try current hour down to -5
        # For 6-hourly models (NAM/GFS), try last 4 synoptic cycles
        candidates = []
        for offset in range(24):
            c = (now.hour - offset) % 24
            if c in cfg["cycles"]:
                candidates.append(c)
            if len(candidates) >= 4:
                break

        for cycle in candidates:
            # Compute init time for this cycle
            init = now.replace(hour=cycle, minute=0, second=0, microsecond=0)
            if init > now:
                init -= timedelta(days=1)

            # Check if this init can reach the target date
            # For today: we need forecasts valid today, so hours_ahead to end of target day
            hours_to_end = (target_date.replace(hour=23, minute=59) - init).total_seconds() / 3600
            hours_to_start = (target_date - init).total_seconds() / 3600

            # Skip if target day is entirely beyond model range
            if hours_to_start > cfg["max_fhr"]:
                continue
            # Skip if init is after target day
            if hours_to_end < 0:
                continue

            forecasts = fetch_bufkit(model, city_key, cycle, init)
            if not forecasts:
                continue

            day_fc = [f for f in forecasts
                      if f["valid_utc"].date() == target_date.date() and f["t2m_f"] > -900]
            if day_fc:
                highs[model] = max(f["t2m_f"] for f in day_fc)
                break

    # Supplemental independent members used by Kukulkan core.
    try:
        from kukulkan import fetch_additional_model_highs
        highs.update(fetch_additional_model_highs(lat, lon, target_date))
    except Exception:
        pass

    return highs


# ══════════════════════════════════════════════════════════════════════
#  Kalshi Live Bucket Prices
# ══════════════════════════════════════════════════════════════════════

def fetch_kalshi_buckets(series_ticker: str, target_date: datetime) -> list[dict]:
    """Fetch live Kalshi bucket markets for a series/date."""
    date_str = target_date.strftime("%y%b%d").upper()  # e.g. "26APR10"

    # Try to find the event
    url = f"{KALSHI_BASE}/markets?series_ticker={series_ticker}&status=open&limit=100"
    data = _fetch_json(url, headers=KALSHI_HEADERS)
    if not data:
        return []

    markets = data.get("markets", [])

    # Filter to today's date
    today_markets = []
    for m in markets:
        ticker = m.get("ticker", "")
        # Kalshi tickers contain date like KXHIGHNY-26APR10-T70
        if date_str in ticker:
            # Parse bucket from subtitle
            sub = m.get("subtitle", m.get("title", ""))
            lo = hi = None

            match = re.search(r'(\d+)\s*(?:to|–|-|through)\s*(\d+)', sub)
            if match:
                lo, hi = float(match.group(1)), float(match.group(2))
            elif re.search(r'(\d+).*(?:or|°)\s*(?:above|higher|more)', sub, re.I):
                val = re.search(r'(\d+)', sub)
                if val:
                    lo, hi = float(val.group(1)), float(val.group(1)) + 30
            elif re.search(r'(\d+).*(?:or|°)\s*(?:below|lower|less)', sub, re.I):
                val = re.search(r'(\d+)', sub)
                if val:
                    lo, hi = float(val.group(1)) - 30, float(val.group(1))

            # Also try parsing from ticker: -T70 means >=70, -B65 means <65
            if lo is None:
                tm = re.search(r'-T(\d+)', ticker)
                bm = re.search(r'-B(\d+\.?\d*)', ticker)
                if tm:
                    lo = float(tm.group(1))
                    hi = lo + 20
                elif bm:
                    hi = float(bm.group(1))
                    lo = hi - 20

            yes_price = m.get("yes_ask", m.get("last_price"))
            no_price = m.get("no_ask")
            volume = m.get("volume", 0)
            open_interest = m.get("open_interest", 0)

            if lo is not None:
                today_markets.append({
                    "ticker": ticker,
                    "lo": lo, "hi": hi,
                    "yes_price": yes_price,
                    "no_price": no_price,
                    "volume": volume,
                    "open_interest": open_interest,
                    "subtitle": sub,
                })

    return sorted(today_markets, key=lambda x: x["lo"])


# ══════════════════════════════════════════════════════════════════════
#  Edge Computation
# ══════════════════════════════════════════════════════════════════════

def bucket_prob(lo, hi, mu, sigma):
    """P(lo <= X <= hi) for X ~ N(mu, sigma)."""
    if sigma <= 0:
        return 1.0 if lo <= mu <= hi else 0.0
    from math import erf, sqrt
    def phi(x):
        return 0.5 * (1 + erf((x - mu) / (sigma * sqrt(2))))
    return phi(hi) - phi(lo)


def compute_edges(buckets: list[dict], est_high: float, sigma: float) -> list[dict]:
    """Compare NWP-derived probabilities against Kalshi prices."""
    if not buckets or est_high is None:
        return []

    # Compute model probs
    total_p = 0
    for b in buckets:
        b["model_prob"] = bucket_prob(b["lo"], b["hi"], est_high, sigma)
        total_p += b["model_prob"]

    # Normalize
    for b in buckets:
        b["model_prob"] = b["model_prob"] / total_p if total_p > 0 else 0

        market_p = (b["yes_price"] or 50) / 100.0
        b["market_prob"] = market_p
        b["edge"] = b["model_prob"] - market_p

        # Signal
        if b["edge"] > 0.06:
            b["signal"] = "BUY"
        elif b["edge"] < -0.06:
            b["signal"] = "SELL"
        else:
            b["signal"] = "—"

    return buckets


# ══════════════════════════════════════════════════════════════════════
#  Dashboard
# ══════════════════════════════════════════════════════════════════════

def run_dashboard(city_keys: list[str] | None = None, deep: str | None = None):
    now = datetime.now(timezone.utc)
    today = now.replace(hour=0, minute=0, second=0, microsecond=0)
    et = now - timedelta(hours=4)

    if city_keys is None:
        city_keys = sorted(KALSHI_CITIES.keys())

    console = Console() if HAS_RICH else None

    # ── Header
    if console:
        console.print(Panel(
            f"[bold white]KUKULKAN TRADING DASHBOARD[/]  —  "
            f"[cyan]{et.strftime('%b %d %Y')}[/]  "
            f"[yellow]{et.strftime('%I:%M %p')} ET[/]  "
            f"[dim]({now.strftime('%H:%M')} UTC)[/]",
            border_style="bright_blue", width=100,
        ))
    else:
        print(f"\n  KUKULKAN TRADING DASHBOARD — {et.strftime('%b %d %Y %I:%M %p')} ET ({now.strftime('%H:%M')} UTC)")

    # ── Fetch METARs (all cities, 24h, one batch)
    all_icaos = [KALSHI_CITIES[ck][1] for ck in city_keys]
    icao_map = {KALSHI_CITIES[ck][1]: ck for ck in city_keys}
    all_metars = fetch_all_metars(all_icaos, hours=24)

    # ── Parallel: NWP model highs for each city
    model_results = {}
    with ThreadPoolExecutor(max_workers=10) as pool:
        futures = {pool.submit(fetch_model_highs, ck, today): ck for ck in city_keys}
        for f in as_completed(futures):
            ck = futures[f]
            try:
                model_results[ck] = f.result()
            except:
                model_results[ck] = {}

    # ── Build rows
    rows = []
    for ck in city_keys:
        c = KALSHI_CITIES[ck]
        icao = c[1]
        obs = all_metars.get(icao, [])
        stats = compute_obs_stats(obs, now)
        models = model_results.get(ck, {})

        # Best estimate high — use calibration engine if available
        model_temps = list(models.values())
        calib_result = None

        if HAS_CALIB and models:
            calib_result = calibrate_city(ck, models, today)
            est_high = calib_result.get("calib_mean")
            sigma = calib_result.get("calib_std", 5.0)
            model_mean = calib_result.get("weighted_mean")
        else:
            candidates = []
            if stats.get("obs_max"):
                candidates.append(stats["obs_max"])
            for m, v in models.items():
                candidates.append(v)
            est_high = max(candidates) if candidates else None
            model_mean = sum(model_temps) / len(model_temps) if model_temps else None
            sigma = max(2.5, (max(model_temps) - min(model_temps)) / 2 + 1.5) if model_temps else 5.0

        model_spread = (max(model_temps) - min(model_temps)) if len(model_temps) > 1 else 0

        # If obs already exceeds calibrated mean, bump est_high
        if stats.get("obs_max") and est_high and stats["obs_max"] > est_high:
            est_high = stats["obs_max"]

        rows.append({
            "ck": ck, "city": c[0], "icao": icao,
            "stats": stats, "models": models,
            "est_high": est_high, "model_mean": model_mean,
            "model_spread": model_spread, "sigma": sigma,
            "calib": calib_result,
        })

    # ── Print overview table
    if console:
        t = Table(box=box.SIMPLE_HEAVY, show_edge=False, pad_edge=False,
                  title="[bold]20-CITY OVERVIEW[/]", title_style="bright_white")
        t.add_column("City", style="bold white", width=15)
        t.add_column("Now", justify="right", width=6)
        t.add_column("Td", justify="right", width=5, style="dim")
        t.add_column("Sky", width=4, style="dim")
        t.add_column("Wind", width=10)
        t.add_column("ObsHi", justify="right", width=6)
        t.add_column("HRRR", justify="right", width=5, style="cyan")
        t.add_column("NAM", justify="right", width=5, style="green")
        t.add_column("GFS", justify="right", width=5, style="yellow")
        t.add_column("Est↑", justify="right", width=6, style="bold bright_white")
        t.add_column("σ", justify="right", width=4)
        t.add_column("3h", justify="right", width=5)
        t.add_column("Wx", width=4)

        for r in rows:
            s = r["stats"]
            m = r["models"]

            cur = f"{s['current']:.0f}" if s.get("current") else "—"
            dp = f"{s['current_dp']:.0f}" if s.get("current_dp") else ""
            sky = s.get("sky", "")
            wx = s.get("wx", "")

            wind_str = ""
            if s.get("wind"):
                d, sp, g = s["wind"]
                dirs = ["N","NNE","NE","ENE","E","ESE","SE","SSE","S","SSW","SW","WSW","W","WNW","NW","NNW"]
                wd = dirs[int((d + 11.25) / 22.5) % 16] if sp > 0 else ""
                wind_str = f"{wd}{sp}" + (f"G{g}" if g else "")

            obs_hi = f"{s['obs_max']:.0f}" if s.get("obs_max") else "—"
            hrrr = f"{m['hrrr']:.0f}" if "hrrr" in m else "—"
            nam = f"{m['nam']:.0f}" if "nam" in m else "—"
            gfs = f"{m['gfs']:.0f}" if "gfs" in m else "—"
            est = f"{r['est_high']:.0f}" if r["est_high"] else "—"
            sigma = f"{r['sigma']:.1f}" if r["sigma"] else ""

            trend = ""
            if s.get("trend_3h"):
                tv = s["trend_3h"]
                if tv > 1:
                    trend = f"[red]↑{tv:.0f}[/]"
                elif tv < -1:
                    trend = f"[blue]↓{abs(tv):.0f}[/]"
                else:
                    trend = f"[dim]→[/]"

            t.add_row(r["city"], cur, dp, sky, wind_str, obs_hi, hrrr, nam, gfs, est, sigma, trend, wx)

        console.print(t)

        # Legend
        console.print("[dim]  Now=METAR temp  Td=dewpoint  ObsHi=today's observed max  Est↑=best high estimate  σ=uncertainty  3h=temp trend[/]")
        console.print(f"[dim]  Updated {now.strftime('%H:%M:%S')} UTC  •  HRRR/NAM/GFS = forecast daily high from latest available cycle[/]\n")

    else:
        # Fallback plain text
        print(f"\n  {'City':<15} {'Now':>5} {'ObsHi':>6} {'HRRR':>5} {'NAM':>5} {'GFS':>5} {'Est↑':>6} {'σ':>4} {'3h':>5}")
        print(f"  {'─'*65}")
        for r in rows:
            s = r["stats"]
            m = r["models"]
            cur = f"{s.get('current', 0):.0f}" if s.get("current") else "—"
            obs_hi = f"{s['obs_max']:.0f}" if s.get("obs_max") else "—"
            hrrr = f"{m['hrrr']:.0f}" if "hrrr" in m else "—"
            nam = f"{m['nam']:.0f}" if "nam" in m else "—"
            gfs = f"{m['gfs']:.0f}" if "gfs" in m else "—"
            est = f"{r['est_high']:.0f}" if r["est_high"] else "—"
            sig = f"{r['sigma']:.1f}" if r["sigma"] else ""
            trend = f"{s.get('trend_3h', 0):+.0f}" if s.get("trend_3h") else ""
            print(f"  {r['city']:<15} {cur:>5} {obs_hi:>6} {hrrr:>5} {nam:>5} {gfs:>5} {est:>6} {sig:>4} {trend:>5}")

    # ── Deep dive for single city
    if deep and deep in KALSHI_CITIES:
        deep_dive(deep, rows, now, today, console)

    return rows


def deep_dive(ck: str, rows: list[dict], now: datetime, today: datetime, console):
    """Single-city deep dive with hourly obs, bucket pricing, and edge."""
    c = KALSHI_CITIES[ck]
    row = next((r for r in rows if r["ck"] == ck), None)
    if not row:
        return

    s = row["stats"]
    m = row["models"]

    if console:
        console.print(f"\n[bold bright_white]{'═'*80}[/]")
        console.print(f"[bold bright_white]  {c[0].upper()} ({c[1]}) — CLI: {c[7]} — WFO: {c[8]}[/]")
        console.print(f"[bold bright_white]{'═'*80}[/]")

    # Model detail
    if console and m:
        mt = Table(box=box.ROUNDED, title="[bold]Model Forecast Highs[/]", show_edge=False)
        mt.add_column("Model", width=8)
        mt.add_column("Fcst High", justify="right", width=10)
        mt.add_column("vs Est", justify="right", width=8)
        est = row["est_high"] or 0
        for model in ["hrrr", "nam", "gfs"]:
            if model in m:
                diff = m[model] - est
                color = "red" if diff > 2 else ("blue" if diff < -2 else "white")
                mt.add_row(model.upper(), f"{m[model]:.1f}°F", f"[{color}]{diff:+.1f}[/]")
        console.print(mt)

    # Fetch Kalshi bucket prices
    series = c[5]  # high series ticker
    buckets = fetch_kalshi_buckets(series, today)

    if buckets and row["est_high"]:
        # Use calibrated edge calculation if available
        calib = row.get("calib")
        if HAS_CALIB and calib and "distribution" in calib:
            calc = EdgeCalculator()
            edges = calc.compute_edges(calib["distribution"], buckets)
            calib_mode = True
        else:
            edges = compute_edges(buckets, row["est_high"], row["sigma"])
            calib_mode = False

        if console:
            title = f"[bold]Kalshi Buckets — {series}[/]"
            if calib_mode:
                title += "  [green](CALIBRATED)[/]"
            bt = Table(box=box.ROUNDED, title=title, show_edge=False)
            bt.add_column("Bucket", width=14)
            bt.add_column("Kalshi¢", justify="right", width=8)
            bt.add_column("Calib%", justify="right", width=8)
            bt.add_column("Edge", justify="right", width=8)
            bt.add_column("Signal", justify="center", width=10)
            bt.add_column("Kelly%", justify="right", width=7)
            bt.add_column("Vol", justify="right", width=6)
            bt.add_column("Bar", width=20)

            for b in edges:
                if calib_mode:
                    price_str = f"{b.get('market_price', 50)}¢"
                    model_str = f"{b['calib_prob_pct']:.1f}%"
                    edge_val = b["edge_pct"]
                    kelly_str = f"{b['kelly_pct']:.1f}%" if b.get("kelly_pct") else ""
                    sig = b["signal"]
                    bar_len = int(b["calib_prob"] * 40)
                else:
                    price_str = f"{b['yes_price']}¢" if b.get("yes_price") else "—"
                    model_str = f"{b['model_prob']*100:.1f}%"
                    edge_val = b["edge"] * 100
                    kelly_str = ""
                    sig = b["signal"]
                    bar_len = int(b.get("model_prob", 0) * 40)

                edge_color = "green" if edge_val > 5 else ("red" if edge_val < -5 else "dim")
                edge_str = f"[{edge_color}]{edge_val:+.1f}%[/]"

                sig_colors = {"BUY": "bold green", "SELL": "bold red",
                              "LEAN_BUY": "green", "LEAN_SELL": "red"}
                sig_style = sig_colors.get(sig, "dim")

                vol_str = str(b.get("volume", ""))
                bar = "█" * bar_len

                bt.add_row(
                    f"{b['lo']:.0f}-{b['hi']:.0f}°F",
                    price_str, model_str, edge_str,
                    f"[{sig_style}]{sig}[/]",
                    kelly_str, vol_str, f"[cyan]{bar}[/]"
                )

            console.print(bt)

            if calib_mode and calib:
                console.print(f"  [dim]Calibrated: μ={calib['calib_mean']}°F  σ={calib['calib_std']}°F  skew={calib.get('skew', 0):.2f}  |  P10={calib['p10']}  P50={calib['p50']}  P90={calib['p90']}[/]")
                corr = calib.get("corrected_models", {})
                if corr:
                    parts = [f"{k.upper()}={v:.0f}" for k, v in corr.items()]
                    console.print(f"  [dim]Bias-corrected: {', '.join(parts)}[/]")
            else:
                console.print(f"  [dim]Est High: {row['est_high']:.1f}°F  σ: {row['sigma']:.1f}°F  Models: {', '.join(m.keys())}[/]")

        else:
            print(f"\n  Kalshi Buckets — {series}")
            print(f"  {'Bucket':<14} {'Price':>6} {'Model':>7} {'Edge':>7} {'Signal':>7}")
            print(f"  {'─'*45}")
            for b in edges:
                print(f"  {b['lo']:.0f}-{b['hi']:.0f}°F    {b.get('yes_price','—'):>5}¢ {b['model_prob']*100:>6.1f}% {b['edge']*100:>+6.1f}% {b['signal']:>7}")

    elif not buckets:
        msg = f"  No Kalshi buckets found for {series} on {today.strftime('%Y-%m-%d')}"
        if console:
            console.print(f"[dim]{msg}[/]")
        else:
            print(msg)

    # Obs history
    if console and s.get("obs_count", 0) > 0:
        console.print(f"\n  [dim]Today: {s['obs_count']} obs  |  Max {s['obs_max']:.0f}°F @ {s.get('obs_max_time','')}  |  Min {s['obs_min']:.0f}°F  |  3h trend: {s.get('trend_3h', 0):+.1f}°F[/]")


# ══════════════════════════════════════════════════════════════════════
#  CLI
# ══════════════════════════════════════════════════════════════════════

def main():
    import argparse
    p = argparse.ArgumentParser(description="Real-time Kalshi temperature trading dashboard")
    p.add_argument("--city", nargs="+", help="City keys (e.g. nyc chi)")
    p.add_argument("--deep", help="Single city deep dive with buckets (e.g. nyc)")
    p.add_argument("--loop", type=int, help="Auto-refresh interval in minutes")
    args = p.parse_args()

    cities = args.city or None
    deep = args.deep

    if args.loop:
        while True:
            print("\033[2J\033[H", end="")
            run_dashboard(cities, deep)
            if HAS_RICH:
                Console().print(f"[dim]  Refreshing in {args.loop}m... Ctrl+C to stop[/]")
            else:
                print(f"  Refreshing in {args.loop}m...")
            time.sleep(args.loop * 60)
    else:
        run_dashboard(cities, deep)


if __name__ == "__main__":
    main()
