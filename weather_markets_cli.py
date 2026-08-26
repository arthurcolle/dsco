#!/usr/bin/env python3
"""
╔══════════════════════════════════════════════════════════════════════╗
║   NOAA · NWS · KALSHI  —  Weather Market Intelligence CLI  v1.1    ║
║   Real-time NWS forecasts  ×  Kalshi prediction market structure   ║
╚══════════════════════════════════════════════════════════════════════╝

Commands
────────
  nws forecast   --lat --lon [--days] [--hourly]
  nws alerts     --area
  nws observe    --station | --lat --lon
  nws stations   --lat --lon

  kalshi weather [--days-ahead] [--cities] [--analyze]
  kalshi series  [--filter] [--limit]
  kalshi markets [--event] [--series] [--status] [--analyze]
  kalshi detail  <TICKER>  [--book]
  kalshi orderbook <TICKER>
  kalshi structure [--series] [--weather-only] [--limit]

  combo  --lat --lon [--area] [--days]
  dash   [--lat] [--lon] [--area]
"""

import sys
import json
import math
import time
import datetime
import textwrap
from typing import Optional

import click
import requests
from rich.console import Console
from rich.table import Table
from rich.panel import Panel
from rich.text import Text
from rich.rule import Rule
from rich.progress import Progress, SpinnerColumn, TextColumn
from rich import box

console = Console()

# ──────────────────────────── ENDPOINTS ────────────────────────────
NWS_BASE    = "https://api.weather.gov"
KALSHI_BASE = "https://api.elections.kalshi.com/trade-api/v2"   # public, no auth

HEADERS_NWS    = {"User-Agent": "WeatherMarketsCLI/1.1", "Accept": "application/geo+json"}
HEADERS_KALSHI = {"Accept": "application/json"}

# Weather series tickers on Kalshi (curated list)
WEATHER_SERIES = {
    "KXRAINNYC":   "Rain NYC (daily)",
    "KXRAINSEA":   "Rain Seattle (daily)",
    "KXRAINHOU":   "Rain Houston (daily)",
    "KXRAINNO":    "Rain New Orleans (daily)",
    "KXHIGHNY":    "High Temp NYC (daily)",
    "KXHIGHNY0":   "High Temp NYC v2 (daily)",
    "KXHIGHCHI":   "High Temp Chicago (daily)",
    "KXHIGHMIA":   "High Temp Miami (daily)",
    "KXHIGHLAX":   "High Temp LA (daily)",
    "KXHIGHAUS":   "High Temp Austin (daily)",
    "KXHIGHDEN":   "High Temp Denver (daily)",
    "KXHIGHHOU":   "High Temp Houston (daily)",
    "KXHIGHPHIL":  "High Temp Philadelphia (daily)",
    "KXLOWNYC":    "Low Temp NYC (daily)",
    "KXLOWCHI":    "Low Temp Chicago (daily)",
    "KXLOWMIA":    "Low Temp Miami (daily)",
    "KXLOWLAX":    "Low Temp LA (daily)",
    "KXLOWDEN":    "Low Temp Denver (daily)",
    "KXLOWAUS":    "Low Temp Austin (daily)",
    "KXLOWPHIL":   "Low Temp Philadelphia (daily)",
    "KXTEMPNYCH":  "NYC Hourly Temp Directional",
    "KXHIGHTHOU":  "High Temp Houston (daily)",
    "KXHIGHTSEA":  "High Temp Seattle (daily)",
    "KXHIGHTPHX":  "High Temp Phoenix (daily)",
    "KXHIGHTLV":   "High Temp Las Vegas (daily)",
    "KXHIGHTBOS":  "High Temp Boston (daily)",
    "KXHIGHTDAL":  "High Temp Dallas (daily)",
    "KXHIGHTMIN":  "High Temp Minneapolis (daily)",
    "KXHIGHTDC":   "High Temp DC (daily)",
    "KXHIGHTOKC":  "High Temp OKC (daily)",
    "KXHIGHTATL":  "High Temp Atlanta (daily)",
    "KXHIGHTLV":   "High Temp Las Vegas (daily)",
    "KXHIGHTNOLA": "High Temp New Orleans (daily)",
    "KXHIGHTSATX": "High Temp San Antonio (daily)",
    "KXHIGHTSFO":  "High Temp San Francisco (daily)",
    "KXDVHIGH":    "Death Valley Temp (daily)",
    "KXNYCSNOWM":  "NYC Snow Monthly",
    "KXBOSSNOWM":  "Boston Snow Monthly",
    "KXCHISNOWM":  "Chicago Snow Monthly",
    "KXDENSNOWM":  "Denver Snow Monthly",
    "KXPHILSNOWM": "Philadelphia Snow Monthly",
    "KXDCSNOWM":   "DC Snow Monthly",
    "KXSEASNOWM":  "Seattle Snow Monthly",
    "KXSNOWNYC":   "NYC Snow (custom)",
    "KXSNOWNY":    "NYC Snow v2",
    "KXHURCTOT":   "# Hurricanes (annual)",
    "KXHURCTOTMAJ":"# Major Hurricanes (annual)",
    "KXTROPSTORM": "# Tropical Storms (annual)",
    "KXHURNYC":    "Hurricane hits NYC",
    "KXHURNJ":     "Hurricane hits NJ",
    "KXHURMIA":    "Hurricane hits Miami",
    "KXHURTB":     "Hurricane hits Tampa",
    "KXHURNO":     "Hurricane hits New Orleans",
    "KXHURCATH":   "Hurricane Henri (custom)",
    "KXGSTORM":    "Geomagnetic Storm",
    "KXHEATWARNING":"Heat Warning",
    "KXGTEMP":     "Hottest Year (annual)",
    "KXTEMPMON":   "Global Monthly Temp",
    "KXAVGTEMP":   "US Average Temp (monthly)",
    "RAINNYCM":    "Monthly Rain NYC",
    "KXRAINNYCM":  "Monthly Rain NYC v2",
    "HIGHCHI":     "High Temp Chicago",
    "HIGHNY":      "High Temp NYC",
    "HIGHNY0":     "High Temp NYC v2",
    "HIGHMIA":     "High Temp Miami",
    "RAINNY":      "Rain NYC",
    "RAINNYC":     "Rain NYC v2",
    "RAINNO":      "Rain New Orleans",
    "RAINHOU":     "Rain Houston",
    "RAINSEA":     "Rain Seattle",
    "SNOWNY":      "Snow NYC",
    "SNOWNYC":     "Snow NYC v2",
}

# ═══════════════════════════════════════════════════════════════════
#  HTTP UTILS
# ═══════════════════════════════════════════════════════════════════

def _get(url, params=None, headers=None, timeout=15) -> dict:
    try:
        r = requests.get(url, params=params,
                         headers=headers or HEADERS_NWS, timeout=timeout)
        r.raise_for_status()
        return r.json()
    except requests.HTTPError as e:
        console.print(f"[red]HTTP {e.response.status_code}[/] — {url}")
        return {}
    except Exception as e:
        console.print(f"[red]Request error:[/] {e}")
        return {}


def _spinner(msg: str):
    return Progress(SpinnerColumn(), TextColumn(f"[cyan]{msg}[/]"), transient=True)


# ═══════════════════════════════════════════════════════════════════
#  DISPLAY HELPERS
# ═══════════════════════════════════════════════════════════════════

def _prob_bar(p: float, width: int = 22) -> Text:
    filled = max(0, min(width, int(p * width)))
    bar    = "█" * filled + "░" * (width - filled)
    color  = "green" if p >= 0.6 else ("yellow" if p >= 0.35 else "red")
    t = Text()
    t.append(f"{bar} ", style=color)
    t.append(f"{p*100:5.1f}%", style=f"bold {color}")
    return t


def _spread_color(spread: int) -> str:
    if spread <= 2:  return "green"
    if spread <= 5:  return "yellow"
    return "red"


def _liq_score(vol, spread) -> str:
    if not vol or spread is None:
        return "—"
    s = math.log(vol + 1) / max(spread + 1, 1)
    c = "green" if s > 3 else ("yellow" if s > 1 else "red")
    return f"[{c}]{s:.2f}[/]"


def wind_dir(deg: float) -> str:
    dirs = ["N","NNE","NE","ENE","E","ESE","SE","SSE",
            "S","SSW","SW","WSW","W","WNW","NW","NNW"]
    return dirs[int((deg + 11.25) / 22.5) % 16]


def _fmt_time(iso: str, fmt="%m/%d %H:%M") -> str:
    try:
        return datetime.datetime.fromisoformat(
            iso.replace("Z", "+00:00")).strftime(fmt)
    except Exception:
        return iso[:16]


def _cents(dollars_str) -> Optional[int]:
    """Convert '$0.72' or '0.72' → 72 (cents)."""
    if dollars_str is None:
        return None
    try:
        return round(float(str(dollars_str).replace("$","")) * 100)
    except Exception:
        return None


# ═══════════════════════════════════════════════════════════════════
#  NWS API
# ═══════════════════════════════════════════════════════════════════

def _nws_point(lat, lon):
    return _get(f"{NWS_BASE}/points/{lat:.4f},{lon:.4f}")

def _nws_forecast_raw(office, gx, gy, hourly=False):
    path = "forecastHourly" if hourly else "forecast"
    return _get(f"{NWS_BASE}/gridpoints/{office}/{gx},{gy}/{path}")

def _nws_forecast(lat, lon, hourly=False):
    meta = _nws_point(lat, lon)
    if not meta:
        return {}, {}
    p = meta.get("properties", {})
    fc = _nws_forecast_raw(p["gridId"], p["gridX"], p["gridY"], hourly)
    return meta, fc

def _nws_alerts(area="US"):
    return _get(f"{NWS_BASE}/alerts/active", params={"area": area})

def _nws_stations(lat, lon):
    return _get(f"{NWS_BASE}/points/{lat:.4f},{lon:.4f}/stations")

def _nws_obs(station_id):
    return _get(f"{NWS_BASE}/stations/{station_id}/observations/latest")


# ═══════════════════════════════════════════════════════════════════
#  KALSHI API  (public, no auth)
# ═══════════════════════════════════════════════════════════════════

def _kalshi_events(series_ticker="", limit=50, status="open"):
    params = {"limit": limit, "status": status}
    if series_ticker:
        params["series_ticker"] = series_ticker
    return _get(f"{KALSHI_BASE}/events", params=params, headers=HEADERS_KALSHI)

def _kalshi_markets(event_ticker="", series_ticker="",
                    limit=100, status="open"):
    params = {"limit": limit, "status": status}
    if event_ticker:  params["event_ticker"]  = event_ticker
    if series_ticker: params["series_ticker"] = series_ticker
    return _get(f"{KALSHI_BASE}/markets", params=params, headers=HEADERS_KALSHI)

def _kalshi_market(ticker):
    return _get(f"{KALSHI_BASE}/markets/{ticker}", headers=HEADERS_KALSHI)

def _kalshi_orderbook(ticker):
    return _get(f"{KALSHI_BASE}/markets/{ticker}/orderbook",
                headers=HEADERS_KALSHI)

def _kalshi_series_list(limit=200):
    return _get(f"{KALSHI_BASE}/series", params={"limit": limit},
                headers=HEADERS_KALSHI)

def _enrich(m: dict) -> dict:
    """Add integer cent fields to a raw market dict."""
    m["_ya"] = _cents(m.get("yes_ask_dollars"))
    m["_yb"] = _cents(m.get("yes_bid_dollars"))
    m["_na"] = _cents(m.get("no_ask_dollars"))
    m["_nb"] = _cents(m.get("no_bid_dollars"))
    m["_vol"] = int(float(m.get("volume_fp") or 0))
    m["_oi"]  = int(float(m.get("open_interest_fp") or 0))
    if m["_ya"] is not None and m["_na"] is not None:
        m["_spread"] = m["_ya"] + m["_na"] - 100
        m["_skew"]   = m["_ya"] - 50
    else:
        m["_spread"] = None; m["_skew"] = None
    return m


def _weather_markets_live(series_list=None, days_ahead=3, limit=50) -> list[dict]:
    """Fetch live open weather markets from curated series."""
    series_list = series_list or list(WEATHER_SERIES.keys())[:20]
    results = []
    seen = set()
    for s in series_list:
        data = _kalshi_markets(series_ticker=s, limit=limit)
        for m in data.get("markets", []):
            if m["ticker"] not in seen:
                seen.add(m["ticker"])
                results.append(_enrich(m))
    return results


# ═══════════════════════════════════════════════════════════════════
#  RENDER  —  NWS
# ═══════════════════════════════════════════════════════════════════

def render_forecast(fc: dict, location: str, n=14, hourly=False):
    periods = (fc.get("properties") or {}).get("periods", [])[:n]
    if not periods:
        console.print("[red]No forecast periods.[/]"); return

    title = f"[bold cyan]{'Hourly' if hourly else '7-Day'} NWS Forecast — {location}[/]"
    t = Table(title=title, box=box.ROUNDED, show_lines=True,
              expand=True, header_style="bold magenta")

    if hourly:
        t.add_column("Time",     width=18)
        t.add_column("Temp",     justify="right", width=7)
        t.add_column("Wind",     width=18)
        t.add_column("Dew Pt",   justify="right", width=8)
        t.add_column("RH %",     justify="right", width=6)
        t.add_column("Forecast", width=26)
        t.add_column("Precip%",  justify="right", width=8)
        for p in periods:
            temp  = p.get("temperature", "?")
            unit  = p.get("temperatureUnit","F")
            tc    = "red" if unit=="F" and isinstance(temp,int) and temp>=90 else (
                    "cyan" if unit=="F" and isinstance(temp,int) and temp<=32 else "white")
            dp    = (p.get("dewpoint") or {}).get("value")
            rh    = (p.get("relativeHumidity") or {}).get("value")
            prcp  = (p.get("probabilityOfPrecipitation") or {}).get("value")
            t.add_row(
                _fmt_time(p.get("startTime","")),
                f"[{tc}]{temp}°{unit}[/]",
                f"{p.get('windSpeed','')} {p.get('windDirection','')}",
                f"{dp:.1f}°C" if dp is not None else "—",
                f"{rh}%" if rh is not None else "—",
                p.get("shortForecast","")[:26],
                f"{prcp}%" if prcp is not None else "—",
            )
    else:
        t.add_column("Period",   width=20)
        t.add_column("Temp",     justify="right", width=7)
        t.add_column("Wind",     width=18)
        t.add_column("Forecast", width=34)
        t.add_column("Precip%",  justify="right", width=8)
        for p in periods:
            temp = p.get("temperature","?")
            unit = p.get("temperatureUnit","F")
            tc   = "red" if unit=="F" and isinstance(temp,int) and temp>=90 else (
                   "cyan" if unit=="F" and isinstance(temp,int) and temp<=32 else "white")
            prcp = (p.get("probabilityOfPrecipitation") or {}).get("value")
            t.add_row(
                p.get("name",""),
                f"[{tc}]{temp}°{unit}[/]",
                f"{p.get('windSpeed','')} {p.get('windDirection','')}",
                p.get("shortForecast","")[:34],
                f"{prcp}%" if prcp is not None else "—",
            )
    console.print(t)


def render_alerts(data: dict, area: str):
    features = (data or {}).get("features", [])
    if not features:
        console.print(Panel(f"[green]✓ No active NWS alerts for {area}[/]",
                            border_style="green")); return
    console.print(Rule(f"[bold red]⚠  {len(features)} Active NWS Alert(s) — {area}[/]",
                       style="red"))
    for f in features:
        p    = f.get("properties", {})
        sev  = p.get("severity","Unknown")
        col  = {"Extreme":"red","Severe":"orange1","Moderate":"yellow","Minor":"cyan"}.get(sev,"white")
        console.print(Panel(
            textwrap.fill((p.get("description") or "")[:600], 80),
            title=f"[bold {col}]{p.get('event','Alert')}[/] — [{col}]{sev}[/]",
            subtitle=f"[grey50]{(p.get('areaDesc') or '')[:80]}[/]",
            border_style=col,
        ))


def render_observation(obs: dict, station_id: str):
    p = (obs.get("properties") or {})
    if not p:
        console.print(f"[red]No observation for {station_id}[/]"); return

    def v(key, unit="", dec=1):
        val = (p.get(key) or {}).get("value")
        return f"{val:.{dec}f}{unit}" if val is not None else "—"

    ts = p.get("timestamp","")
    try:
        ts = datetime.datetime.fromisoformat(ts).strftime("%Y-%m-%d %H:%M UTC")
    except Exception:
        pass

    rows = [("Station",station_id),("Time",ts),
            ("Temp",v("temperature","°C")),("Dew Point",v("dewpoint","°C")),
            ("Wind Dir",v("windDirection","°",0)),
            ("Wind Speed",v("windSpeed"," km/h")),
            ("Wind Gust",v("windGust"," km/h")),
            ("Pressure",v("barometricPressure"," Pa",0)),
            ("Sea Lvl P",v("seaLevelPressure"," Pa",0)),
            ("Visibility",v("visibility"," m",0)),
            ("Humidity",v("relativeHumidity","%")),
            ("Wind Chill",v("windChill","°C")),
            ("Heat Index",v("heatIndex","°C")),]

    t = Table(title=f"[bold]Current Obs — {station_id}[/]",
              box=box.ROUNDED, show_header=False)
    t.add_column("Field", style="bold yellow", width=14)
    t.add_column("Value", width=20)
    for r in rows:
        t.add_row(*r)
    console.print(t)


def render_stations(data: dict, limit=8):
    feats = (data.get("features") or [])[:limit]
    if not feats:
        console.print("[red]No stations found.[/]"); return
    t = Table(title="[bold]Nearby NWS Observation Stations[/]",
              box=box.ROUNDED, header_style="bold cyan")
    t.add_column("Station ID", width=12)
    t.add_column("Name",       width=36)
    t.add_column("Elev (m)",   justify="right", width=10)
    t.add_column("Lat",        justify="right", width=9)
    t.add_column("Lon",        justify="right", width=10)
    for f in feats:
        p2    = f.get("properties", {})
        coords= (f.get("geometry") or {}).get("coordinates",[None,None])
        elev  = (p2.get("elevation") or {}).get("value")
        t.add_row(
            p2.get("stationIdentifier","?"),
            (p2.get("name") or "?")[:36],
            f"{elev:.0f}" if elev is not None else "—",
            f"{coords[1]:.4f}" if coords[1] else "—",
            f"{coords[0]:.4f}" if coords[0] else "—",
        )
    console.print(t)


# ═══════════════════════════════════════════════════════════════════
#  RENDER  —  KALSHI
# ═══════════════════════════════════════════════════════════════════

def render_kalshi_markets(markets: list[dict], title="Kalshi Markets"):
    if not markets:
        console.print("[yellow]No markets.[/]"); return
    t = Table(title=f"[bold magenta]{title}[/]",
              box=box.ROUNDED, show_lines=True, expand=True,
              header_style="bold cyan")
    t.add_column("Ticker",   style="bold", width=30)
    t.add_column("Title",                  width=38)
    t.add_column("Yes Ask",  justify="right", width=9)
    t.add_column("No Ask",   justify="right", width=9)
    t.add_column("Prob Bar",               width=32)
    t.add_column("Volume",   justify="right", width=10)
    t.add_column("OI",       justify="right", width=8)
    t.add_column("Closes",                 width=14)

    for m in markets:
        ya  = m.get("_ya"); na  = m.get("_na")
        vol = m.get("_vol", 0); oi = m.get("_oi", 0)
        prob = ya / 100 if ya is not None else None
        bar  = _prob_bar(prob) if prob is not None else Text("—")
        t.add_row(
            m.get("ticker","?")[:30],
            (m.get("title") or "")[:38],
            f"{ya}¢" if ya is not None else "—",
            f"{na}¢" if na is not None else "—",
            bar,
            f"{vol:,}",
            f"{oi:,}",
            _fmt_time(m.get("close_time",""), "%m/%d %H:%M"),
        )
    console.print(t)


def render_structure(markets: list[dict]):
    if not markets:
        console.print("[yellow]No markets to analyse.[/]"); return
    console.print(Rule("[bold]Market Structure Analysis[/]", style="magenta"))

    t = Table(box=box.MINIMAL_DOUBLE_HEAD, expand=True,
              header_style="bold yellow")
    t.add_column("Ticker",    width=30, style="bold")
    t.add_column("Spread ¢",  justify="right", width=9)
    t.add_column("Yes Ask",   justify="right", width=9)
    t.add_column("No Ask",    justify="right", width=9)
    t.add_column("Total",     justify="right", width=7)
    t.add_column("Skew ¢",    justify="right", width=9)
    t.add_column("Vol",       justify="right", width=9)
    t.add_column("OI",        justify="right", width=8)
    t.add_column("Vol/OI",    justify="right", width=8)
    t.add_column("Liq",       justify="right", width=7)

    for m in markets:
        ya = m.get("_ya"); na = m.get("_na")
        vol = m.get("_vol",0); oi = m.get("_oi",0)
        sp  = m.get("_spread"); sk = m.get("_skew")

        if sp is not None:
            sc = _spread_color(sp)
            sp_s  = f"[{sc}]{sp:+d}[/]"
            tot_s = f"{ya+na}"
            sk_s  = f"[{'orange1' if abs(sk)>10 else 'white'}]{sk:+d}[/]"
        else:
            sp_s=tot_s=sk_s="—"

        vol_oi = f"{vol/oi:.2f}" if oi else "—"
        liq    = _liq_score(vol, sp)

        t.add_row(
            m.get("ticker","?")[:30],
            sp_s,
            f"{ya}¢" if ya is not None else "—",
            f"{na}¢" if na is not None else "—",
            tot_s,
            sk_s,
            f"{vol:,}",
            f"{oi:,}",
            vol_oi,
            liq,
        )
    console.print(t)
    console.print(Panel(
        "[bold]Legend[/]\n"
        "  [cyan]Spread[/]  = (yes_ask + no_ask) − 100¢   →  exchange rake / friction\n"
        "  [cyan]Skew[/]    = yes_ask − 50¢               →  directional lean of market maker\n"
        "  [cyan]Liq[/]     = log(vol) / (spread+1)       →  higher = more liquid / efficient\n"
        "  [cyan]Vol/OI[/]  = turnover ratio               →  >1 means recycled full book",
        title="Structure Guide", border_style="grey50",
    ))


def render_orderbook(ob_data: dict, ticker: str):
    ob = (ob_data.get("orderbook") or {})
    yes_bids = ob.get("yes", [])  # [[price_cents, size], ...]
    no_bids  = ob.get("no",  [])

    t = Table(title=f"[bold]Orderbook — {ticker}[/]",
              box=box.SIMPLE_HEAD, expand=False, header_style="bold")
    t.add_column("YES (¢)",  justify="center", style="green", width=10)
    t.add_column("Size",     justify="right", width=8)
    t.add_column("│",        width=2, style="grey50")
    t.add_column("NO (¢)",   justify="center", style="red", width=10)
    t.add_column("Size",     justify="right", width=8)

    rows = max(len(yes_bids), len(no_bids))
    for i in range(min(rows, 12)):
        y = yes_bids[i] if i < len(yes_bids) else None
        n = no_bids[i]  if i < len(no_bids)  else None
        t.add_row(
            f"{y[0]}¢" if y else "", f"{y[1]:,}" if y else "",
            "│",
            f"{n[0]}¢" if n else "", f"{n[1]:,}" if n else "",
        )
    console.print(t)


def render_market_detail(m: dict):
    if not m:
        console.print("[red]Market not found.[/]"); return
    m = _enrich(m)
    ya=m["_ya"]; na=m["_na"]; yb=m["_yb"]; nb=m["_nb"]
    vol=m["_vol"]; oi=m["_oi"]; sp=m["_spread"]; sk=m["_skew"]

    lines = [
        f"[bold]{m.get('title','')}[/]",
        f"[grey50]{m.get('subtitle','')}[/]",
        "",
        f"  Ticker       : [bold cyan]{m.get('ticker','')}[/]",
        f"  Series       : {m.get('series_ticker','')}   Event: {m.get('event_ticker','')}",
        f"  Yes Ask/Bid  : [green]{ya}¢ / {yb}¢[/]" if ya else "  Yes  : —",
        f"  No  Ask/Bid  : [red]{na}¢ / {nb}¢[/]"   if na else "  No   : —",
    ]
    if ya is not None:
        lines.append(f"  Implied Prob : {_prob_bar(ya/100)}")
    if sp is not None:
        sc = _spread_color(sp)
        lines += [
            f"  Spread       : [{sc}]{sp:+d}¢[/] (rake above fair = 100¢)",
            f"  Skew         : [{'orange1' if sk and abs(sk)>10 else 'white'}]{sk:+d}¢[/]",
            f"  Liq Score    : {_liq_score(vol,sp)}",
        ]
    lines += [
        "",
        f"  Volume       : [yellow]{vol:,}[/]",
        f"  Open Int     : [yellow]{oi:,}[/]",
        f"  Vol/OI       : {vol/oi:.2f}" if oi else "  Vol/OI       : —",
        f"  Status       : {m.get('status','')}",
        f"  Opens        : {_fmt_time(m.get('open_time',''), '%Y-%m-%d %H:%M')}",
        f"  Closes       : {_fmt_time(m.get('close_time',''), '%Y-%m-%d %H:%M')}",
        "",
        f"  [grey50]Rules: {(m.get('rules_primary') or '')[:180]}[/]",
    ]
    console.print(Panel("\n".join(lines), title="[bold magenta]Market Detail[/]",
                        border_style="cyan"))


def render_series_table(series_list: list[dict]):
    t = Table(title="[bold]Kalshi Weather Series[/]",
              box=box.ROUNDED, header_style="bold cyan")
    t.add_column("Ticker",    width=24, style="bold")
    t.add_column("Title",     width=40)
    t.add_column("Frequency", width=12)
    for s in series_list:
        t.add_row(s.get("ticker",""), (s.get("title") or "")[:40],
                  s.get("frequency",""))
    console.print(t)


# ═══════════════════════════════════════════════════════════════════
#  CLI
# ═══════════════════════════════════════════════════════════════════

@click.group()
@click.version_option("1.1.0", prog_name="weather-markets")
def cli():
    """
    \b
    ╔══════════════════════════════════════════════════════════╗
    ║  NOAA · NWS · KALSHI  —  Weather Market Intelligence    ║
    ╚══════════════════════════════════════════════════════════╝
    Combine live NWS/NOAA forecasts with Kalshi prediction-market
    structure analysis to find context, edge, and opportunity.
    """


# ─────────────────────────── NWS ───────────────────────────────────

@cli.group()
def nws():
    """NOAA National Weather Service commands."""

@nws.command("forecast")
@click.option("--lat",    type=float, required=True)
@click.option("--lon",    type=float, required=True)
@click.option("--days",   default=7, show_default=True, help="# days (2 periods/day)")
@click.option("--hourly", is_flag=True, help="Show hourly forecast")
def nws_forecast_cmd(lat, lon, days, hourly):
    """NWS gridded forecast for a lat/lon."""
    with _spinner("Fetching NWS forecast …") as p:
        p.add_task(""); time.sleep(0.05)
    meta, fc = _nws_forecast(lat, lon, hourly)
    if not fc:
        return
    props = (meta.get("properties") or {})
    city  = (props.get("relativeLocation") or {}).get("properties", {})
    loc   = f"{city.get('city','?')}, {city.get('state','?')} ({lat},{lon})"
    console.print(f"[grey50]Timezone: {props.get('timeZone','?')}[/]")
    n = (days * 24 if hourly else days * 2)
    render_forecast(fc, loc, n=min(n, 96), hourly=hourly)


@nws.command("alerts")
@click.option("--area", default="US", show_default=True,
              help="2-letter state code or US")
def nws_alerts_cmd(area):
    """Active NWS weather alerts."""
    with _spinner(f"Fetching NWS alerts ({area}) …") as p:
        p.add_task(""); time.sleep(0.05)
    render_alerts(_nws_alerts(area.upper()), area.upper())


@nws.command("observe")
@click.option("--lat",     type=float, default=None)
@click.option("--lon",     type=float, default=None)
@click.option("--station", default="", help="NWS station ID e.g. KNYC")
def nws_observe_cmd(lat, lon, station):
    """Latest surface observation from a NWS station."""
    if not station:
        if lat is None or lon is None:
            console.print("[red]Provide --station or --lat + --lon.[/]"); return
        with _spinner("Finding nearest station …") as p:
            p.add_task(""); time.sleep(0.05)
        stns = _nws_stations(lat, lon)
        feats = (stns.get("features") or [])
        if not feats:
            console.print("[red]No stations found.[/]"); return
        station = feats[0]["properties"]["stationIdentifier"]
        console.print(f"[grey50]Auto-selected: {station}[/]")
    with _spinner(f"Fetching obs for {station} …") as p:
        p.add_task(""); time.sleep(0.05)
    render_observation(_nws_obs(station), station)


@nws.command("stations")
@click.option("--lat",   type=float, required=True)
@click.option("--lon",   type=float, required=True)
@click.option("--limit", default=8, show_default=True)
def nws_stations_cmd(lat, lon, limit):
    """List nearby NWS observation stations."""
    with _spinner("Finding stations …") as p:
        p.add_task(""); time.sleep(0.05)
    render_stations(_nws_stations(lat, lon), limit)


# ─────────────────────────── KALSHI ────────────────────────────────

@cli.group()
def kalshi():
    """Kalshi prediction market commands."""


@kalshi.command("weather")
@click.option("--cities",    default="nyc,chi,mia,lax,sea,hou,den,phi,bos",
              show_default=True, help="City codes for rain/temp markets")
@click.option("--days-ahead",default=2, show_default=True,
              help="Include events up to N days ahead")
@click.option("--analyze",   is_flag=True, help="Show structure analysis")
@click.option("--limit",     default=10, show_default=True,
              help="Max markets per series")
def kalshi_weather_cmd(cities, days_ahead, analyze, limit):
    """
    Show live open Kalshi weather markets (rain, temp, snow, hurricane).
    Pulls from curated daily/weekly weather series.
    """
    city_map = {
        "nyc": ["KXRAINNYC","KXHIGHNY","KXLOWNYC","KXTEMPNYCH"],
        "chi": ["KXHIGHCHI","KXLOWCHI","KXRAINCHIM"],
        "mia": ["KXHIGHMIA","KXLOWMIA"],
        "lax": ["KXHIGHLAX","KXLOWLAX"],
        "sea": ["KXRAINSEA","KXHIGHTSEA"],
        "hou": ["KXRAINHOU","KXHIGHTHOU","KXHIGHHOU"],
        "den": ["KXHIGHDEN","KXLOWDEN","KXDENSNOWM"],
        "phi": ["KXHIGHPHIL","KXLOWPHIL","KXPHILSNOWM"],
        "bos": ["KXHIGHTBOS","KXBOSSNOWM"],
        "atl": ["KXHIGHTATL"],
        "okc": ["KXHIGHTOKC"],
        "dal": ["KXHIGHTDAL"],
        "sat": ["KXHIGHTSATX"],
        "nola":["KXHIGHTNOU","KXRAINNO"],
        "min": ["KXHIGHTMIN"],
        "sfo": ["KXHIGHTSFO","KXRAINSFOM"],
        "phx": ["KXHIGHTPHX"],
        "lv":  ["KXHIGHTLV"],
        "dc":  ["KXHIGHTDC","KXDCSNOWM"],
        "aus": ["KXHIGHAUS","KXLOWAUS"],
    }

    series_to_fetch = []
    for c in cities.lower().split(","):
        c = c.strip()
        if c in city_map:
            series_to_fetch.extend(city_map[c])
    # Add season-wide series
    series_to_fetch.extend([
        "KXHURCTOT","KXHURCTOTMAJ","KXTROPSTORM",
        "KXNYCSNOWM","KXCHISNOWM","KXGTEMP","KXTEMPMON",
    ])
    series_to_fetch = list(dict.fromkeys(series_to_fetch))  # dedupe

    with _spinner(f"Fetching weather markets for {cities} …") as prog:
        prog.add_task(""); time.sleep(0.05)

    all_markets = _weather_markets_live(series_to_fetch, limit=limit)
    console.print(f"[grey50]Found {len(all_markets)} open weather markets.[/]")
    render_kalshi_markets(all_markets, "Kalshi — Live Weather Markets")
    if analyze:
        render_structure(all_markets)


@kalshi.command("series")
@click.option("--filter", "filt", default="weather",
              help="Keyword filter (default: weather)")
@click.option("--limit",  default=50, show_default=True)
def kalshi_series_cmd(filt, limit):
    """Browse Kalshi series, filtered by keyword."""
    with _spinner("Fetching Kalshi series list …") as p:
        p.add_task(""); time.sleep(0.05)

    # Show our curated list, or fetch from API
    if filt.lower() in ("weather","all",""):
        curated = [{"ticker": k, "title": v, "frequency": "—"}
                   for k, v in WEATHER_SERIES.items()][:limit]
        render_series_table(curated)
        return

    data = _kalshi_series_list(200)
    all_s = data.get("series", [])
    filt_lower = filt.lower()
    filtered = [s for s in all_s
                if filt_lower in (s.get("title","") + s.get("ticker","")).lower()]
    render_series_table(filtered[:limit])


@kalshi.command("markets")
@click.option("--event",   default="", help="Event ticker")
@click.option("--series",  default="", help="Series ticker e.g. KXRAINNYC")
@click.option("--status",  default="open", show_default=True,
              type=click.Choice(["open","closed","settled"]))
@click.option("--limit",   default=50, show_default=True)
@click.option("--analyze", is_flag=True)
def kalshi_markets_cmd(event, series, status, limit, analyze):
    """Browse Kalshi markets with optional filters."""
    with _spinner("Fetching markets …") as p:
        p.add_task(""); time.sleep(0.05)
    data = _kalshi_markets(event_ticker=event, series_ticker=series,
                            limit=limit, status=status)
    markets = [_enrich(m) for m in data.get("markets", [])]
    render_kalshi_markets(markets, f"Kalshi Markets [{status}]")
    if analyze:
        render_structure(markets)


@kalshi.command("detail")
@click.argument("ticker")
@click.option("--book", is_flag=True, help="Show live orderbook")
def kalshi_detail_cmd(ticker, book):
    """Show detail + structure for a specific TICKER."""
    with _spinner(f"Fetching {ticker} …") as p:
        p.add_task(""); time.sleep(0.05)
    data = _kalshi_market(ticker)
    m = data.get("market", data)
    render_market_detail(m)
    if book:
        ob = _kalshi_orderbook(ticker)
        render_orderbook(ob, ticker)


@kalshi.command("orderbook")
@click.argument("ticker")
def kalshi_orderbook_cmd(ticker):
    """Live orderbook for a Kalshi market TICKER."""
    with _spinner(f"Fetching orderbook for {ticker} …") as p:
        p.add_task(""); time.sleep(0.05)
    render_orderbook(_kalshi_orderbook(ticker), ticker)


@kalshi.command("structure")
@click.option("--series",       default="", help="Series ticker to drill into")
@click.option("--weather-only", is_flag=True,
              help="Auto-load all curated weather series")
@click.option("--limit",        default=50, show_default=True)
def kalshi_structure_cmd(series, weather_only, limit):
    """Deep market-structure analysis: spread, skew, liquidity score."""
    with _spinner("Loading markets …") as p:
        p.add_task(""); time.sleep(0.05)
    if weather_only:
        markets = _weather_markets_live(limit=limit)
    elif series:
        data = _kalshi_markets(series_ticker=series, limit=limit)
        markets = [_enrich(m) for m in data.get("markets", [])]
    else:
        data = _kalshi_markets(limit=limit)
        markets = [_enrich(m) for m in data.get("markets", [])]
    render_structure(markets)


# ─────────────────────────── COMBO / DASH ──────────────────────────

@cli.command("combo")
@click.option("--lat",   type=float, required=True)
@click.option("--lon",   type=float, required=True)
@click.option("--area",  default="", help="State code for alerts e.g. NY")
@click.option("--days",  default=5, show_default=True)
@click.option("--cities",default="nyc", show_default=True)
def combo_cmd(lat, lon, area, days, cities):
    """
    \b
    COMBO — NWS forecast + alerts + Kalshi weather markets, one view.
    Perfect for a pre-session trading brief.
    """
    console.print(Rule(
        "[bold cyan]NOAA/NWS  ×  KALSHI — Combined Weather Market Brief[/]",
        style="cyan"))

    # NWS forecast
    with _spinner("Fetching NWS forecast …") as p:
        p.add_task(""); time.sleep(0.05)
    meta, fc = _nws_forecast(lat, lon)
    if fc:
        props = (meta.get("properties") or {})
        city  = (props.get("relativeLocation") or {}).get("properties", {})
        loc   = f"{city.get('city','?')}, {city.get('state','?')}"
        render_forecast(fc, loc, n=days*2)
    console.print()

    # Alerts
    al_area = area.upper() if area else "US"
    with _spinner(f"Fetching alerts ({al_area}) …") as p:
        p.add_task(""); time.sleep(0.05)
    render_alerts(_nws_alerts(al_area), al_area)
    console.print()

    # Kalshi markets
    city_series_map = {
        "nyc": ["KXRAINNYC","KXHIGHNY","KXLOWNYC"],
        "chi": ["KXHIGHCHI","KXLOWCHI"],
        "mia": ["KXHIGHMIA","KXLOWMIA"],
        "lax": ["KXHIGHLAX","KXLOWLAX"],
        "sea": ["KXRAINSEA","KXHIGHTSEA"],
        "hou": ["KXRAINHOU","KXHIGHTHOU"],
        "den": ["KXHIGHDEN","KXLOWDEN"],
        "bos": ["KXHIGHTBOS"],
        "phi": ["KXHIGHPHIL","KXLOWPHIL"],
    }
    series_to_fetch = []
    for c in cities.lower().split(","):
        series_to_fetch.extend(city_series_map.get(c.strip(), []))
    series_to_fetch.extend(["KXHURCTOT","KXHURCTOTMAJ","KXGTEMP"])
    series_to_fetch = list(dict.fromkeys(series_to_fetch))

    with _spinner("Fetching Kalshi weather markets …") as p:
        p.add_task(""); time.sleep(0.05)
    wm = _weather_markets_live(series_to_fetch, limit=10)
    render_kalshi_markets(wm, "Kalshi — Open Weather Markets")
    console.print()
    render_structure(wm)


@cli.command("dash")
@click.option("--lat",  type=float, default=40.7128, show_default=True)
@click.option("--lon",  type=float, default=-74.0060, show_default=True)
@click.option("--area", default="NY", show_default=True,
              help="State for NWS alerts")
def dash_cmd(lat, lon, area):
    """
    Quick dashboard (defaults to New York City).
    Current obs · 3-day forecast · active alerts · top weather markets.
    """
    console.clear()
    console.print(Panel(
        "[bold cyan]Weather × Prediction Market Dashboard[/]\n"
        f"[grey50]{lat}, {lon}  |  alerts: {area}  |  "
        f"{datetime.datetime.utcnow().strftime('%Y-%m-%d %H:%M UTC')}[/]",
        box=box.DOUBLE, border_style="cyan",
    ))

    # Current obs
    stns = _nws_stations(lat, lon)
    feats = (stns.get("features") or [])
    if feats:
        sid = feats[0]["properties"]["stationIdentifier"]
        render_observation(_nws_obs(sid), sid)
    console.print()

    # 3-day forecast
    meta, fc = _nws_forecast(lat, lon)
    if fc:
        props = (meta.get("properties") or {})
        city  = (props.get("relativeLocation") or {}).get("properties", {})
        render_forecast(fc, f"{city.get('city','?')}, {city.get('state','?')}", n=6)
    console.print()

    # Alerts
    render_alerts(_nws_alerts(area.upper()), area.upper())
    console.print()

    # Kalshi top weather markets
    wm = _weather_markets_live(
        ["KXRAINNYC","KXHIGHNY","KXLOWNYC","KXTEMPNYCH",
         "KXHURCTOT","KXHURCTOTMAJ","KXGTEMP","KXTEMPMON"],
        limit=5,
    )
    render_kalshi_markets(wm, "Live Kalshi Weather Markets")


# ═══════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    cli()
