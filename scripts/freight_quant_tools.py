#!/usr/bin/env python3
"""
FREIGHT QUANT AGENT — Tool Registry & Execution Layer

Complete tool surface for a fully autonomous AI-agent freight/FFA
quantitative investment fund. Every tool is a callable that returns
structured data for the agent's reasoning loop.

Architecture:
  Agent Loop → Tool Selection → Tool Execution → Signal Extraction → Portfolio Decision → Execution

7 Tool Domains:
  1. MARKET DATA      — prices, indices, curves, settlement
  2. VESSEL INTEL     — AIS tracking, fleet positioning, congestion
  3. CARGO FLOWS      — commodity movements, storage, inventory
  4. MACRO/RATES      — yields, FX, oil, commodities, central bank
  5. GEOPOLITICAL     — events, sentiment, chokepoint risk, sanctions
  6. FUNDAMENTAL      — fleet supply, orderbook, scrapping, yard delivery
  7. EXECUTION        — order entry, risk limits, portfolio state, clearing
"""

import json, os, time, statistics, hashlib, math
from datetime import datetime, timedelta
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError
from dataclasses import dataclass, field
from typing import Any, Callable, Optional
from enum import Enum

# ── Config ──────────────────────────────────────────────────────────────────

FRED_KEY = os.environ.get("FRED_API_KEY", "")
BALTIC_KEY = os.environ.get("BALTIC_API_KEY", "")
CME_KEY = os.environ.get("CME_API_KEY", "")
KPLER_KEY = os.environ.get("KPLER_API_KEY", "")
VORTEXA_KEY = os.environ.get("VORTEXA_API_KEY", "")
SPIRE_KEY = os.environ.get("SPIRE_AIS_KEY", "")
GDELT_KEY = os.environ.get("GDELT_API_KEY", "")  # free, but key for cloud
UA = "dsco-freight-quant/1.0 (research@dsco.dev)"
CACHE_DIR = os.path.expanduser("~/.dsco/cache/freight_quant")
os.makedirs(CACHE_DIR, exist_ok=True)


# ── Tool Framework ──────────────────────────────────────────────────────────

class ToolDomain(Enum):
    MARKET_DATA = "market_data"
    VESSEL_INTEL = "vessel_intel"
    CARGO_FLOWS = "cargo_flows"
    MACRO = "macro"
    GEOPOLITICAL = "geopolitical"
    FUNDAMENTAL = "fundamental"
    EXECUTION = "execution"

class DataLatency(Enum):
    REALTIME = "realtime"       # <1s (websocket, streaming)
    NEAR_RT = "near_realtime"   # 1-60s (REST poll, AIS)
    DELAYED = "delayed"         # 1-15min (free tiers, FRED)
    EOD = "end_of_day"          # T+0 close
    WEEKLY = "weekly"           # EIA, Baker Hughes
    MONTHLY = "monthly"         # OPEC, IEA

@dataclass
class ToolSpec:
    """Specification for a tool the agent can invoke."""
    name: str
    domain: ToolDomain
    description: str
    provider: str
    endpoint: str
    auth: str                    # "none", "api_key", "oauth", "subscription"
    latency: DataLatency
    cost_per_call: float         # USD, 0.0 = free
    rate_limit: str              # e.g. "120/min", "10/sec"
    params: dict                 # parameter schema
    returns: dict                # return schema
    signal_type: str             # "alpha", "risk", "execution", "context"
    edge_decay: str              # how fast the signal decays: "seconds", "minutes", "hours", "days"
    fn: Optional[Callable] = None  # actual implementation if available

    def to_dict(self):
        return {
            "name": self.name,
            "domain": self.domain.value,
            "description": self.description,
            "provider": self.provider,
            "endpoint": self.endpoint,
            "auth": self.auth,
            "latency": self.latency.value,
            "cost_per_call": self.cost_per_call,
            "rate_limit": self.rate_limit,
            "signal_type": self.signal_type,
            "edge_decay": self.edge_decay,
            "params": self.params,
            "returns": self.returns,
        }


# ── HTTP Helpers ────────────────────────────────────────────────────────────

def _fetch(url, headers=None, timeout=15):
    hdrs = {"User-Agent": UA}
    if headers:
        hdrs.update(headers)
    req = Request(url, headers=hdrs)
    try:
        with urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())
    except Exception:
        return None

def _cached(key, url, ttl=300, headers=None):
    path = os.path.join(CACHE_DIR, hashlib.md5(key.encode()).hexdigest())
    if os.path.exists(path) and (time.time() - os.path.getmtime(path)) < ttl:
        with open(path) as f:
            return json.load(f)
    data = _fetch(url, headers)
    if data:
        with open(path, "w") as f:
            json.dump(data, f)
    return data

def _fred(series_id, limit=30):
    if not FRED_KEY:
        return []
    url = (f"https://api.stlouisfed.org/fred/series/observations?"
           f"series_id={series_id}&api_key={FRED_KEY}&file_type=json"
           f"&sort_order=desc&limit={limit}")
    data = _cached(f"fred_{series_id}", url, ttl=3600)
    if data and "observations" in data:
        return [{"date": o["date"], "value": float(o["value"])}
                for o in data["observations"] if o["value"] != "."]
    return []


# ════════════════════════════════════════════════════════════════════════════
# DOMAIN 1: MARKET DATA — Prices, Indices, Curves, Settlement
# ════════════════════════════════════════════════════════════════════════════

def baltic_spot_rates(route_ids: list[str] = None):
    """Baltic Exchange spot rate assessments.
    The single most important data feed. Daily assessed freight rates
    for 60+ routes across dry bulk, tanker, gas, container.
    Settlement basis for all FFAs."""
    # Baltic Exchange API: https://api.balticexchange.com
    if BALTIC_KEY:
        url = "https://api.balticexchange.com/api/v1/assessments"
        headers = {"x-apikey": BALTIC_KEY}
        data = _fetch(url, headers)
        if data and route_ids:
            return [d for d in data if d.get("routeCode") in route_ids]
        return data
    return {"status": "need_baltic_key", "note": "Subscribe at balticexchange.com"}

def baltic_ffa_curves(route_id: str = "TD3C"):
    """Baltic Forward Assessments (BFA) — the FFA forward curve.
    Published daily by Baltic Exchange brokers panel.
    Shows market-implied future freight rates by tenor."""
    if BALTIC_KEY:
        url = f"https://api.balticexchange.com/api/v1/forward-curves/{route_id}"
        return _fetch(url, {"x-apikey": BALTIC_KEY})
    return {"status": "need_baltic_key"}

def baltic_indices():
    """Baltic composite indices: BDI, BCI, BPI, BSI, BHSI, BDTI, BCTI.
    The benchmarks that move the market."""
    if BALTIC_KEY:
        return _fetch("https://api.balticexchange.com/api/v1/indices",
                      {"x-apikey": BALTIC_KEY})
    return {"status": "need_baltic_key"}

def cme_freight_settlements():
    """CME NYMEX freight futures settlement prices.
    Cleared FFA contracts: TD3C, TC2, Cape 5TC, Panamax 4TC, etc.
    Essential for mark-to-market and basis risk analysis."""
    if CME_KEY:
        url = ("https://www.cmegroup.com/CmeWS/mvc/Settlements/Futures/"
               "Settlements/5750/FUT?tradeDate=")  # 5750 = freight product group
        return _fetch(url)
    return {"status": "need_cme_key", "alt": "CME DataMine API or CME Market Data API"}

def sgx_ffa_volumes():
    """SGX AsiaClear FFA cleared volumes.
    SGX handles ~40% of global dry bulk FFA clearing.
    Volume = conviction. Low volume = illiquid = dangerous."""
    return {"status": "scrape_baltic_volumes_page",
            "url": "https://www.balticexchange.com/en/data-services/freight-derivatives-/Volumes.html"}

def ice_brent_wti_spread():
    """ICE Brent and NYMEX WTI futures — the crude oil basis.
    Brent-WTI spread drives trans-Atlantic VLCC arb economics."""
    brent = _fred("DCOILBRENTEU", 60)
    wti = _fred("DCOILWTICO", 60)
    if brent and wti:
        spread = brent[0]["value"] - wti[0]["value"]
        return {
            "brent": brent[0], "wti": wti[0], "spread": round(spread, 2),
            "spread_20d_avg": round(statistics.mean(
                [b["value"] - w["value"] for b, w in zip(brent[:20], wti[:20])]), 2)
            if len(brent) >= 20 and len(wti) >= 20 else None,
            "brent_history": brent[:20], "wti_history": wti[:20],
        }
    return {}

def bunker_prices():
    """Ship fuel (bunker) prices — VLSFO, HSFO, MGO.
    Critical for TCE calculation: TCE = (freight revenue - bunker cost) / days.
    Bunker is 50-70% of voyage cost."""
    # Ship&Bunker scrape or Platts API
    return {"status": "need_platts_or_shipbunker",
            "endpoints": {
                "ship_bunker": "https://shipandbunker.com/prices",
                "platts": "https://www.spglobal.com/commodities/en/our-methodology/price-assessments/shipping/bunker-fuel.html",
            },
            "key_ports": ["Singapore", "Fujairah", "Rotterdam", "Houston", "Shanghai"],
            "products": ["VLSFO_380", "HSFO_380", "MGO"]}

def container_indices():
    """Container freight indices: FBX (Freightos), SCFI (Shanghai), CCFI, WCI (Drewry).
    Multiple indices because container market is fragmented and opaque."""
    # Freightos has a free tier
    return {"status": "multiple_sources",
            "freightos_fbx": "https://fbx.freightos.com/api/",
            "scfi": "https://en.sse.net.cn/indices/scfinew.jsp",
            "drewry_wci": "https://www.drewry.co.uk/supply-chain-advisors/world-container-index"}


MARKET_DATA_TOOLS = [
    ToolSpec(
        name="baltic_spot_rates",
        domain=ToolDomain.MARKET_DATA,
        description="Baltic Exchange daily spot rate assessments for 60+ freight routes (dry, tanker, gas, container). The settlement benchmark for all FFAs.",
        provider="Baltic Exchange",
        endpoint="https://api.balticexchange.com/api/v1/assessments",
        auth="api_key",
        latency=DataLatency.EOD,
        cost_per_call=0.0,  # included in subscription
        rate_limit="1000/day",
        params={"route_ids": "list[str] — Baltic route codes (TD3C, C5, P1A_82, etc.)"},
        returns={"routes": "list of {route_code, rate, unit, date, assessment_type}"},
        signal_type="alpha",
        edge_decay="hours",
        fn=baltic_spot_rates,
    ),
    ToolSpec(
        name="baltic_ffa_curves",
        domain=ToolDomain.MARKET_DATA,
        description="Baltic Forward Assessments — FFA forward curves by route. Shows market-implied future freight rates across tenors (M+1 through CAL+2). The curve shape (backwardation/contango) is itself a signal.",
        provider="Baltic Exchange",
        endpoint="https://api.balticexchange.com/api/v1/forward-curves/{route_id}",
        auth="api_key",
        latency=DataLatency.EOD,
        cost_per_call=0.0,
        rate_limit="1000/day",
        params={"route_id": "str — Baltic route code"},
        returns={"curve": "list of {tenor, bid, ask, settle, volume, open_interest}"},
        signal_type="alpha",
        edge_decay="hours",
        fn=baltic_ffa_curves,
    ),
    ToolSpec(
        name="baltic_indices",
        domain=ToolDomain.MARKET_DATA,
        description="Baltic composite indices (BDI, BCI, BPI, BSI, BHSI, BDTI, BCTI). Headline numbers that move capital allocation across the sector.",
        provider="Baltic Exchange",
        endpoint="https://api.balticexchange.com/api/v1/indices",
        auth="api_key",
        latency=DataLatency.EOD,
        cost_per_call=0.0,
        rate_limit="1000/day",
        params={},
        returns={"indices": "dict of {index_code: {value, change, change_pct, date}}"},
        signal_type="context",
        edge_decay="hours",
        fn=baltic_indices,
    ),
    ToolSpec(
        name="cme_freight_settlements",
        domain=ToolDomain.MARKET_DATA,
        description="CME NYMEX cleared freight futures settlement prices. Mark-to-market basis for cleared FFA positions. Includes volume, OI, and daily settlement.",
        provider="CME Group",
        endpoint="https://www.cmegroup.com/CmeWS/mvc/Settlements/",
        auth="api_key",
        latency=DataLatency.EOD,
        cost_per_call=0.0,
        rate_limit="unlimited",
        params={"product_group": "str — CME product code (5750=freight)"},
        returns={"settlements": "list of {contract, month, settle, volume, oi, change}"},
        signal_type="alpha",
        edge_decay="hours",
        fn=cme_freight_settlements,
    ),
    ToolSpec(
        name="sgx_ffa_volumes",
        domain=ToolDomain.MARKET_DATA,
        description="SGX AsiaClear FFA cleared volumes by route. Volume is conviction — low volume means illiquid positions and dangerous basis risk.",
        provider="SGX / Baltic Exchange",
        endpoint="https://www.balticexchange.com/.../Volumes.html",
        auth="none",
        latency=DataLatency.EOD,
        cost_per_call=0.0,
        rate_limit="none",
        params={},
        returns={"volumes": "dict of {route: {volume_lots, oi_lots, notional_usd}}"},
        signal_type="risk",
        edge_decay="days",
        fn=sgx_ffa_volumes,
    ),
    ToolSpec(
        name="ice_brent_wti_spread",
        domain=ToolDomain.MARKET_DATA,
        description="Brent-WTI crude oil spread (via FRED). Drives transatlantic VLCC arbitrage economics. Widening spread = more US crude export demand = more VLCC demand in Atlantic.",
        provider="FRED / ICE / NYMEX",
        endpoint="https://api.stlouisfed.org/fred/series/observations",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.0,
        rate_limit="120/min",
        params={},
        returns={"brent": "float", "wti": "float", "spread": "float", "history": "list"},
        signal_type="alpha",
        edge_decay="hours",
        fn=ice_brent_wti_spread,
    ),
    ToolSpec(
        name="bunker_prices",
        domain=ToolDomain.MARKET_DATA,
        description="Ship fuel (bunker) prices at key ports: VLSFO, HSFO, MGO. Bunker is 50-70% of voyage cost. Essential for TCE calculation and voyage P&L.",
        provider="Ship&Bunker / Platts / Argus",
        endpoint="https://shipandbunker.com/prices",
        auth="subscription",
        latency=DataLatency.EOD,
        cost_per_call=0.0,
        rate_limit="none",
        params={"port": "str", "product": "str — VLSFO_380, HSFO_380, MGO"},
        returns={"prices": "dict of {port: {product: price_per_mt}}"},
        signal_type="alpha",
        edge_decay="hours",
        fn=bunker_prices,
    ),
    ToolSpec(
        name="container_indices",
        domain=ToolDomain.MARKET_DATA,
        description="Container freight indices: Freightos FBX, Shanghai SCFI, CCFI, Drewry WCI. Multiple views of the container market from different vantage points.",
        provider="Freightos / SSE / Drewry",
        endpoint="https://fbx.freightos.com/api/",
        auth="api_key",
        latency=DataLatency.WEEKLY,
        cost_per_call=0.0,
        rate_limit="100/day",
        params={"index": "str — FBX, SCFI, CCFI, WCI", "lane": "str — route code"},
        returns={"index_value": "float", "change_pct": "float", "lanes": "list"},
        signal_type="alpha",
        edge_decay="days",
        fn=container_indices,
    ),
]


# ════════════════════════════════════════════════════════════════════════════
# DOMAIN 2: VESSEL INTELLIGENCE — AIS, Fleet Positioning, Congestion
# ════════════════════════════════════════════════════════════════════════════

VESSEL_INTEL_TOOLS = [
    ToolSpec(
        name="ais_vessel_positions",
        domain=ToolDomain.VESSEL_INTEL,
        description="Real-time AIS vessel positions. Track every tanker, bulker, container ship globally. The raw signal that everything else derives from: congestion, ton-miles, floating storage, dark fleet activity.",
        provider="Spire Maritime / Kpler MarineTraffic",
        endpoint="https://ais.spire.com/vessels/",
        auth="api_key",
        latency=DataLatency.NEAR_RT,
        cost_per_call=0.01,
        rate_limit="1000/min",
        params={"mmsi": "str", "imo": "str", "vessel_type": "str (tanker/bulk/container)",
                "bbox": "list[float] — [lat_min, lon_min, lat_max, lon_max]",
                "timestamp_after": "datetime", "timestamp_before": "datetime"},
        returns={"positions": "list of {mmsi, imo, lat, lon, speed, heading, draught, timestamp, destination}"},
        signal_type="alpha",
        edge_decay="minutes",
    ),
    ToolSpec(
        name="chokepoint_transit_count",
        domain=ToolDomain.VESSEL_INTEL,
        description="Real-time vessel transit counts through chokepoints: Hormuz, Suez, Bab el-Mandeb, Malacca, Panama, Gibraltar, Dover, Cape. Count = demand proxy. Drop in count = disruption signal.",
        provider="Spire / Kpler / UN COMTRADE",
        endpoint="https://ais.spire.com/analytics/chokepoints",
        auth="api_key",
        latency=DataLatency.NEAR_RT,
        cost_per_call=0.05,
        rate_limit="100/min",
        params={"chokepoint": "str — hormuz, suez, panama, malacca, bab_el_mandeb, cape",
                "vessel_type": "str", "period": "str — 1d, 7d, 30d"},
        returns={"transits": "int", "vs_avg": "float — % vs 30d average",
                 "by_type": "dict of {vessel_type: count}", "trend": "list"},
        signal_type="alpha",
        edge_decay="minutes",
    ),
    ToolSpec(
        name="port_congestion",
        domain=ToolDomain.VESSEL_INTEL,
        description="Port congestion metrics: vessels waiting, average wait time, berth utilization. Congestion at loading ports = supply constraint = rate support. Congestion at discharge = demand signal.",
        provider="Kpler / MarineTraffic / Portcast",
        endpoint="https://api.kpler.com/v2/ports/congestion",
        auth="api_key",
        latency=DataLatency.NEAR_RT,
        cost_per_call=0.02,
        rate_limit="500/min",
        params={"port": "str — UN/LOCODE", "vessel_type": "str", "min_dwt": "int"},
        returns={"vessels_waiting": "int", "avg_wait_days": "float",
                 "berth_utilization": "float", "vs_30d_avg": "float"},
        signal_type="alpha",
        edge_decay="hours",
    ),
    ToolSpec(
        name="floating_storage",
        domain=ToolDomain.VESSEL_INTEL,
        description="Floating storage detection: tankers stationary for 7+ days with cargo aboard. Floating storage = contango play or supply glut. Rising floating storage = bearish for crude, but absorbs tonnage = bullish for freight.",
        provider="Kpler / Vortexa",
        endpoint="https://api.vortexa.com/v6/vessels/floating-storage",
        auth="api_key",
        latency=DataLatency.NEAR_RT,
        cost_per_call=0.10,
        rate_limit="60/min",
        params={"vessel_class": "str — VLCC, Suezmax, Aframax",
                "region": "str — MEG, WAF, USG, SEA", "min_days": "int"},
        returns={"count": "int", "total_dwt": "int", "total_barrels": "int",
                 "avg_duration_days": "float", "trend_7d": "float"},
        signal_type="alpha",
        edge_decay="hours",
    ),
    ToolSpec(
        name="dark_fleet_tracking",
        domain=ToolDomain.VESSEL_INTEL,
        description="Dark fleet / AIS-off vessel detection. Vessels turning off transponders = sanctions evasion (Iran, Russia, Venezuela crude). Dark fleet size affects effective supply: more dark fleet = less legitimate tonnage = higher rates for compliant fleet.",
        provider="Windward / TankerTrackers / Kpler",
        endpoint="https://api.windward.ai/v2/vessels/dark-activity",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.50,
        rate_limit="30/min",
        params={"vessel_class": "str", "region": "str", "gap_hours_min": "int"},
        returns={"dark_vessels": "int", "suspected_sts": "list",
                 "dark_fleet_dwt": "int", "pct_of_fleet": "float"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="ton_mile_demand",
        domain=ToolDomain.VESSEL_INTEL,
        description="Ton-mile demand calculation from AIS cargo tracking. Ton-miles = cargo_tons × distance_sailed. THE fundamental demand metric for freight. Route substitution (MEG→Cape vs MEG→Suez) massively changes ton-miles for same cargo volume.",
        provider="Kpler / Clarksons / SSY",
        endpoint="https://api.kpler.com/v2/freight/ton-miles",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.10,
        rate_limit="60/min",
        params={"vessel_class": "str", "period": "str — monthly, weekly",
                "commodity": "str — crude, CPP, DPP, coal, iron_ore, grain"},
        returns={"ton_miles": "float — billion ton-miles", "yoy_change": "float",
                 "trend": "list", "by_route": "dict"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="vessel_speed_analysis",
        domain=ToolDomain.VESSEL_INTEL,
        description="Fleet average speed analysis. Slow-steaming = oversupply (owners stretch voyage to absorb tonnage). Speed-up = tight market (rush to load/discharge). Speed is a real-time supply adjustment mechanism.",
        provider="Spire / Kpler",
        endpoint="https://ais.spire.com/analytics/fleet-speed",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.05,
        rate_limit="100/min",
        params={"vessel_class": "str", "status": "str — laden, ballast",
                "region": "str"},
        returns={"avg_speed_knots": "float", "vs_design_speed_pct": "float",
                 "vs_30d_avg": "float", "effective_supply_impact_pct": "float"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="ship_to_ship_transfers",
        domain=ToolDomain.VESSEL_INTEL,
        description="Ship-to-ship (STS) transfer detection from AIS proximity analysis. STS = sanctions evasion, crude blending, or floating storage operations. Concentration of STS = shadow market activity.",
        provider="Windward / Vortexa / Kpler",
        endpoint="https://api.vortexa.com/v6/sts-events",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.20,
        rate_limit="30/min",
        params={"region": "str", "period": "str", "vessel_class": "str"},
        returns={"events": "list of {lat, lon, vessel1, vessel2, duration, estimated_cargo}",
                 "count": "int", "total_volume": "float"},
        signal_type="alpha",
        edge_decay="days",
    ),
]


# ════════════════════════════════════════════════════════════════════════════
# DOMAIN 3: CARGO FLOWS — Commodity Movements, Storage, Inventory
# ════════════════════════════════════════════════════════════════════════════

CARGO_FLOW_TOOLS = [
    ToolSpec(
        name="crude_cargo_flows",
        domain=ToolDomain.CARGO_FLOWS,
        description="Real-time crude oil cargo movements: origin, destination, vessel, cargo size, ETA. Track every barrel moving on water. The ground truth that physical traders live by.",
        provider="Kpler / Vortexa",
        endpoint="https://api.kpler.com/v2/flows",
        auth="api_key",
        latency=DataLatency.NEAR_RT,
        cost_per_call=0.10,
        rate_limit="60/min",
        params={"commodity": "str — crude, fuel_oil, naphtha, lng, lpg",
                "origin_zone": "str — MEG, WAF, USG, NSea, LatAm",
                "destination_zone": "str — China, Europe, India, Japan, Korea, USG",
                "period": "str — 7d, 30d, 90d",
                "vessel_class": "str — VLCC, Suezmax, Aframax"},
        returns={"flows": "list of {cargo_id, origin, dest, vessel, dwt, volume_bbl, departure, eta, grade}",
                 "total_volume": "float", "vs_period_avg": "float"},
        signal_type="alpha",
        edge_decay="hours",
    ),
    ToolSpec(
        name="onshore_oil_storage",
        domain=ToolDomain.CARGO_FLOWS,
        description="Satellite-derived onshore oil storage tank fill levels. Shadow count on floating-roof tanks = inventory estimate before official EIA/API/IEA reports. 2-3 day edge over government statistics.",
        provider="Ursa Space / Kayrros / Orbital Insight",
        endpoint="https://api.ursaspace.com/v2/storage",
        auth="api_key",
        latency=DataLatency.WEEKLY,
        cost_per_call=1.00,
        rate_limit="10/min",
        params={"region": "str — cushing, ras_tanura, fujairah, rotterdam, singapore, saldanha",
                "product": "str — crude, products"},
        returns={"fill_level_pct": "float", "volume_mbl": "float",
                 "change_wow": "float", "vs_5yr_avg_pct": "float",
                 "satellite_date": "str"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="eia_petroleum_status",
        domain=ToolDomain.CARGO_FLOWS,
        description="EIA Weekly Petroleum Status Report. The most-watched oil inventory data in the world. Crude stocks, product stocks, refinery utilization, imports/exports. Moves oil prices instantly on Wednesday 10:30 ET.",
        provider="US EIA",
        endpoint="https://api.eia.gov/v2/petroleum/sum/sndw/data/",
        auth="api_key",
        latency=DataLatency.WEEKLY,
        cost_per_call=0.0,
        rate_limit="unlimited",
        params={"series": "str — WCESTUS1 (crude stocks), WGTSTUS1 (gasoline), etc.",
                "frequency": "str — weekly"},
        returns={"value": "float — thousand barrels", "date": "str",
                 "change": "float", "vs_5yr_avg": "float"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="lng_cargo_tracker",
        domain=ToolDomain.CARGO_FLOWS,
        description="LNG cargo movements and destination tracking. Qatar, Australia, US LNG exports. Critical for gas carrier (VLGC/MGC) rate signals and European energy security.",
        provider="Kpler / ICIS / Refinitiv",
        endpoint="https://api.kpler.com/v2/flows?commodity=lng",
        auth="api_key",
        latency=DataLatency.NEAR_RT,
        cost_per_call=0.10,
        rate_limit="60/min",
        params={"origin": "str — Qatar, US, Australia, Nigeria",
                "destination": "str — Europe, Japan, Korea, China",
                "period": "str"},
        returns={"cargoes": "list", "total_volume_mt": "float",
                 "vs_period_avg": "float"},
        signal_type="alpha",
        edge_decay="hours",
    ),
    ToolSpec(
        name="iron_ore_shipments",
        domain=ToolDomain.CARGO_FLOWS,
        description="Iron ore shipment tracking from Brazil (Vale) and Australia (BHP/Rio/FMG). Iron ore is 30%+ of Capesize demand. Brazil shipments = long-haul ton-miles. Australia = short-haul. Mix drives Capesize rate direction.",
        provider="Kpler / SSY / Braemar",
        endpoint="https://api.kpler.com/v2/flows?commodity=iron_ore",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.10,
        rate_limit="60/min",
        params={"origin": "str — Brazil, Australia, India, South_Africa",
                "destination": "str — China, Japan, Korea, Europe"},
        returns={"shipments_mt": "float", "brazil_share_pct": "float",
                 "ton_miles_impact": "float"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="grain_shipments",
        domain=ToolDomain.CARGO_FLOWS,
        description="Grain/soybean/corn shipment tracking. Seasonal grain trade drives Panamax and Supramax demand. US Gulf, Brazil Santos, Argentina Rosario, Black Sea origins.",
        provider="USDA / Kpler / AXS Marine",
        endpoint="https://api.kpler.com/v2/flows?commodity=grain",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.10,
        rate_limit="60/min",
        params={"origin": "str — USG, Brazil, Argentina, BlackSea, Australia",
                "period": "str", "crop": "str — soybean, corn, wheat"},
        returns={"volume_mt": "float", "vs_seasonal_avg": "float"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="strategic_petroleum_reserves",
        domain=ToolDomain.CARGO_FLOWS,
        description="Strategic Petroleum Reserve levels (US, IEA coordinated, China, Japan, Korea). SPR releases = bearish crude but generate VLCC demand to move the oil. SPR levels near lows = less ammunition for price caps.",
        provider="EIA / IEA",
        endpoint="https://api.eia.gov/v2/petroleum/stoc/wstk/data/",
        auth="api_key",
        latency=DataLatency.WEEKLY,
        cost_per_call=0.0,
        rate_limit="unlimited",
        params={"series": "str — SPR inventory code"},
        returns={"spr_mbl": "float", "change_wow": "float",
                 "days_of_import_cover": "float"},
        signal_type="context",
        edge_decay="days",
    ),
]


# ════════════════════════════════════════════════════════════════════════════
# DOMAIN 4: MACRO / RATES — Yields, FX, Oil, Commodities, Central Bank
# ════════════════════════════════════════════════════════════════════════════

def fred_treasury_curve():
    mats = {"1M":"DGS1MO","3M":"DGS3MO","6M":"DGS6MO","1Y":"DGS1",
            "2Y":"DGS2","5Y":"DGS5","10Y":"DGS10","30Y":"DGS30"}
    curve = {}
    for label, sid in mats.items():
        obs = _fred(sid, 5)
        if obs:
            curve[label] = obs[0]["value"]
    return curve

def fred_vix():
    obs = _fred("VIXCLS", 30)
    if obs:
        return {"vix": obs[0]["value"],
                "vix_20d": statistics.mean([o["value"] for o in obs[:20]]) if len(obs) >= 20 else obs[0]["value"],
                "percentile_1y": None}  # would need 252 obs
    return {}

MACRO_TOOLS = [
    ToolSpec(
        name="treasury_yield_curve",
        domain=ToolDomain.MACRO,
        description="US Treasury yield curve from FRED. Curve inversion = recession signal = demand destruction for shipping. Steepening = recovery = bullish for cyclical freight.",
        provider="FRED",
        endpoint="https://api.stlouisfed.org/fred/series/observations",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.0,
        rate_limit="120/min",
        params={},
        returns={"curve": "dict of {maturity: yield_pct}"},
        signal_type="context",
        edge_decay="hours",
        fn=fred_treasury_curve,
    ),
    ToolSpec(
        name="vix_fear_index",
        domain=ToolDomain.MACRO,
        description="CBOE VIX volatility index. High VIX = risk-off = freight demand uncertainty. VIX spike during geopolitical event = pricing dislocation opportunity in FFAs.",
        provider="FRED / CBOE",
        endpoint="https://api.stlouisfed.org/fred/series/observations?series_id=VIXCLS",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.0,
        rate_limit="120/min",
        params={},
        returns={"vix": "float", "vix_20d": "float", "percentile_1y": "float"},
        signal_type="risk",
        edge_decay="minutes",
        fn=fred_vix,
    ),
    ToolSpec(
        name="fx_rates",
        domain=ToolDomain.MACRO,
        description="FX rates: USD/CNY, USD/JPY, USD/EUR, USD/BRL. Weak CNY = cheaper Chinese imports = bullish Capesize. Strong USD = expensive commodities = mixed signal.",
        provider="FRED / ECB",
        endpoint="https://api.stlouisfed.org/fred/series/observations",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.0,
        rate_limit="120/min",
        params={"pairs": "list[str] — DEXCHUS, DEXJPUS, DEXUSEU"},
        returns={"rates": "dict of {pair: rate}"},
        signal_type="context",
        edge_decay="hours",
    ),
    ToolSpec(
        name="china_pmi",
        domain=ToolDomain.MACRO,
        description="China Manufacturing PMI (official NBS + Caixin). The single most important leading indicator for dry bulk demand. PMI > 50 = expansion = bullish Capesize/Panamax.",
        provider="NBS / Caixin / FRED",
        endpoint="https://api.stlouisfed.org/fred/series/observations?series_id=MPMICNMA",
        auth="api_key",
        latency=DataLatency.MONTHLY,
        cost_per_call=0.0,
        rate_limit="120/min",
        params={},
        returns={"pmi": "float", "direction": "str — expanding/contracting",
                 "new_orders": "float", "new_export_orders": "float"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="opec_production",
        domain=ToolDomain.MACRO,
        description="OPEC+ production levels and compliance. Higher OPEC output = more tanker demand (especially VLCC from MEG). Cuts = less crude on water. Watch Saudi/UAE compliance vs quota.",
        provider="OPEC / IEA / EIA",
        endpoint="https://api.eia.gov/v2/international/data/",
        auth="api_key",
        latency=DataLatency.MONTHLY,
        cost_per_call=0.0,
        rate_limit="unlimited",
        params={"country": "str — Saudi_Arabia, UAE, Iraq, Russia, Iran"},
        returns={"production_kbd": "float", "quota_kbd": "float",
                 "compliance_pct": "float", "spare_capacity_kbd": "float"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="china_steel_production",
        domain=ToolDomain.MACRO,
        description="China steel production (monthly). Steel = iron ore demand = Capesize demand. China is 55% of global steel. Production cuts (environmental, demand) = bearish Capesize.",
        provider="NBS / World Steel Association",
        endpoint="https://api.stlouisfed.org/fred/series/observations?series_id=CHNCRUSTEAM",
        auth="api_key",
        latency=DataLatency.MONTHLY,
        cost_per_call=0.0,
        rate_limit="120/min",
        params={},
        returns={"production_mt": "float", "yoy_pct": "float",
                 "capacity_util_pct": "float"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="credit_spreads",
        domain=ToolDomain.MACRO,
        description="HY and IG credit spreads from FRED. Widening = risk-off = shipping companies' debt cost rises, potential for distressed selling of vessels. Tightening = easy money = fleet expansion risk.",
        provider="FRED / BofA",
        endpoint="https://api.stlouisfed.org/fred/series/observations",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.0,
        rate_limit="120/min",
        params={"series": "str — BAMLH0A0HYM2, BAMLC0A0CM"},
        returns={"hy_oas_bps": "float", "ig_oas_bps": "float", "change": "float"},
        signal_type="risk",
        edge_decay="hours",
    ),
    ToolSpec(
        name="nat_gas_prices",
        domain=ToolDomain.MACRO,
        description="Natural gas prices: Henry Hub, TTF (Europe), JKM (Asia). Gas prices drive LNG carrier demand and clean energy transition calculus. TTF spike = European energy crisis = bullish LNG carriers.",
        provider="FRED / ICE",
        endpoint="https://api.stlouisfed.org/fred/series/observations?series_id=DHHNGSP",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.0,
        rate_limit="120/min",
        params={},
        returns={"henry_hub": "float", "ttf": "float", "jkm": "float",
                 "jkm_hh_spread": "float"},
        signal_type="alpha",
        edge_decay="hours",
    ),
]


# ════════════════════════════════════════════════════════════════════════════
# DOMAIN 5: GEOPOLITICAL — Events, Sentiment, Chokepoint Risk, Sanctions
# ════════════════════════════════════════════════════════════════════════════

GEOPOLITICAL_TOOLS = [
    ToolSpec(
        name="gdelt_event_stream",
        domain=ToolDomain.GEOPOLITICAL,
        description="GDELT real-time global event stream. 100+ languages, 15-minute updates. Query by geography (Hormuz, Suez, Taiwan Strait), event type (military action, blockade, sanctions), and actor (IRGC, Houthis, PLA Navy). Sentiment scoring via GCAM (2,300 emotions).",
        provider="GDELT Project (Google Jigsaw)",
        endpoint="https://api.gdeltproject.org/api/v2/doc/doc",
        auth="none",
        latency=DataLatency.NEAR_RT,
        cost_per_call=0.0,
        rate_limit="unlimited",
        params={"query": "str — search terms",
                "mode": "str — artlist, timeline, tonechart",
                "timespan": "str — 15min, 1h, 24h, 7d",
                "sourcelang": "str — english, arabic, chinese, farsi"},
        returns={"articles": "list of {url, title, tone, source, datetime, domain}",
                 "avg_tone": "float — -100 to +100",
                 "volume": "int — article count"},
        signal_type="alpha",
        edge_decay="minutes",
    ),
    ToolSpec(
        name="chokepoint_risk_monitor",
        domain=ToolDomain.GEOPOLITICAL,
        description="Composite chokepoint risk score combining AIS transit data, GDELT event volume, war risk premium levels, and naval force positioning. The unified geopolitical risk layer for freight.",
        provider="Internal (AIS + GDELT + Insurance composite)",
        endpoint="internal://chokepoint_risk",
        auth="none",
        latency=DataLatency.NEAR_RT,
        cost_per_call=0.0,
        rate_limit="unlimited",
        params={"chokepoint": "str — hormuz, suez, bab_el_mandeb, malacca, panama, taiwan_strait"},
        returns={"risk_score": "float 0-100", "components": "dict",
                 "war_risk_premium_pct": "float", "transit_change_pct": "float",
                 "event_volume_vs_avg": "float", "naval_presence": "str"},
        signal_type="alpha",
        edge_decay="minutes",
    ),
    ToolSpec(
        name="sanctions_tracker",
        domain=ToolDomain.GEOPOLITICAL,
        description="Sanctions monitoring: OFAC SDN list changes, EU sanctions, vessel blacklists. New sanctions = vessels removed from legitimate fleet = tighter supply = higher rates. Sanctions evasion (dark fleet) creates parallel market.",
        provider="OFAC / EU / UN / WindwardAI",
        endpoint="https://sanctionssearch.ofac.treas.gov/api/",
        auth="none",
        latency=DataLatency.EOD,
        cost_per_call=0.0,
        rate_limit="unlimited",
        params={"entity_type": "str — vessel, company, individual",
                "search": "str — name or IMO number"},
        returns={"sanctioned": "bool", "programs": "list[str]",
                 "date_added": "str", "related_entities": "list"},
        signal_type="risk",
        edge_decay="days",
    ),
    ToolSpec(
        name="war_risk_premiums",
        domain=ToolDomain.GEOPOLITICAL,
        description="War risk insurance premiums by zone. Published by Lloyd's JWC (Joint War Committee). Premium level = market-priced geopolitical risk. Sharp increases = market expects conflict escalation.",
        provider="Lloyd's JWC / Insurance brokers",
        endpoint="https://www.lloyds.com/market-resources/lloyds-agency/joint-war-committee",
        auth="subscription",
        latency=DataLatency.EOD,
        cost_per_call=0.0,
        rate_limit="none",
        params={"zone": "str — persian_gulf, red_sea, black_sea, south_china_sea"},
        returns={"premium_pct_hull": "float", "change_from_last": "float",
                 "listed_areas": "list[str]", "jwc_update_date": "str"},
        signal_type="alpha",
        edge_decay="hours",
    ),
    ToolSpec(
        name="caldara_iacoviello_gpr",
        domain=ToolDomain.GEOPOLITICAL,
        description="Caldara-Iacoviello Geopolitical Risk Index. Academic benchmark used by central banks and institutional investors. Monthly, country-level. GPR spike = flight-to-safety + shipping disruption risk.",
        provider="Matteo Iacoviello (Federal Reserve Board)",
        endpoint="https://www.matteoiacoviello.com/gpr.htm",
        auth="none",
        latency=DataLatency.MONTHLY,
        cost_per_call=0.0,
        rate_limit="none",
        params={},
        returns={"gpr_global": "float", "gpr_acts": "float — actual threats",
                 "gpr_threats": "float — threatened acts",
                 "percentile_historical": "float"},
        signal_type="context",
        edge_decay="days",
    ),
    ToolSpec(
        name="piracy_attack_log",
        domain=ToolDomain.GEOPOLITICAL,
        description="IMB Piracy Reporting Centre live map + ICC/IMB attack database. Piracy attacks force rerouting, increase insurance, and tighten effective supply. Key regions: Gulf of Guinea, Strait of Malacca, Gulf of Aden.",
        provider="ICC International Maritime Bureau",
        endpoint="https://www.icc-ccs.org/piracy-reporting-centre/live-piracy-map",
        auth="none",
        latency=DataLatency.EOD,
        cost_per_call=0.0,
        rate_limit="none",
        params={"region": "str", "period": "str"},
        returns={"incidents": "list of {date, lat, lon, vessel_type, attack_type, outcome}",
                 "count_ytd": "int", "vs_prior_year": "float"},
        signal_type="risk",
        edge_decay="days",
    ),
]


# ════════════════════════════════════════════════════════════════════════════
# DOMAIN 6: FUNDAMENTAL — Fleet Supply, Orderbook, Scrapping, Yards
# ════════════════════════════════════════════════════════════════════════════

FUNDAMENTAL_TOOLS = [
    ToolSpec(
        name="fleet_supply_database",
        domain=ToolDomain.FUNDAMENTAL,
        description="Global fleet count by vessel class, age profile, and DWT. The denominator in the supply-demand equation. Fleet growth rate is the multi-year structural driver of freight rates.",
        provider="Clarksons Research / Equasis / IHS Markit",
        endpoint="https://www.clarksons.net/api/fleet",
        auth="subscription",
        latency=DataLatency.MONTHLY,
        cost_per_call=0.0,
        rate_limit="none",
        params={"vessel_class": "str — VLCC, Suezmax, Capesize, Panamax, etc.",
                "min_dwt": "int", "max_age": "int"},
        returns={"count": "int", "total_dwt": "int",
                 "age_profile": "dict of {age_bucket: count}",
                 "growth_rate_yoy": "float"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="orderbook_deliveries",
        domain=ToolDomain.FUNDAMENTAL,
        description="Shipyard orderbook and delivery schedule. Orderbook-to-fleet ratio is THE forward-looking supply indicator. Ratio > 20% = supply tsunami coming = bearish for rates in 2-3 years. Ratio < 10% = structural tightness.",
        provider="Clarksons / VesselsValue / Braemar",
        endpoint="https://www.clarksons.net/api/orderbook",
        auth="subscription",
        latency=DataLatency.MONTHLY,
        cost_per_call=0.0,
        rate_limit="none",
        params={"vessel_class": "str", "delivery_year": "int"},
        returns={"orderbook_count": "int", "orderbook_dwt": "int",
                 "ob_to_fleet_pct": "float",
                 "deliveries_by_quarter": "dict",
                 "slippage_rate_pct": "float"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="scrapping_demolition",
        domain=ToolDomain.FUNDAMENTAL,
        description="Vessel demolition/scrapping activity. Scrapping removes supply = bullish for rates. Scrap steel prices in Alang/Gadani drive scrapping economics. Old vessels (20+ years) are first to go when rates collapse.",
        provider="Clarksons / GMS / Best Oasis",
        endpoint="https://www.clarksons.net/api/demolition",
        auth="subscription",
        latency=DataLatency.MONTHLY,
        cost_per_call=0.0,
        rate_limit="none",
        params={"vessel_class": "str", "period": "str"},
        returns={"scrapped_count": "int", "scrapped_dwt": "int",
                 "scrap_price_per_ldt": "float",
                 "avg_age_scrapped": "float",
                 "annualized_rate": "float"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="newbuild_prices",
        domain=ToolDomain.FUNDAMENTAL,
        description="Newbuilding prices by vessel class. High NB prices = owners reluctant to order = future supply constraint = bullish medium-term. NB-to-secondhand ratio signals market cycle position.",
        provider="Clarksons / VesselsValue",
        endpoint="https://www.clarksons.net/api/newbuild-prices",
        auth="subscription",
        latency=DataLatency.MONTHLY,
        cost_per_call=0.0,
        rate_limit="none",
        params={"vessel_class": "str"},
        returns={"nb_price_mm": "float", "change_yoy": "float",
                 "nb_to_secondhand_ratio": "float",
                 "yard_capacity_util_pct": "float"},
        signal_type="context",
        edge_decay="days",
    ),
    ToolSpec(
        name="secondhand_vessel_sales",
        domain=ToolDomain.FUNDAMENTAL,
        description="S&P (Sale & Purchase) market transactions. Vessel sale prices = real-time asset valuation. Rising S&P prices = owners bullish = rate expectations elevated. Volume of sales = liquidity in the physical market.",
        provider="VesselsValue / Clarksons / Allied",
        endpoint="https://api.vesselsvalue.com/v2/sales",
        auth="api_key",
        latency=DataLatency.EOD,
        cost_per_call=0.10,
        rate_limit="100/day",
        params={"vessel_class": "str", "age_max": "int", "period": "str"},
        returns={"sales": "list of {vessel, dwt, age, price_mm, buyer, seller, date}",
                 "avg_price_mm": "float", "volume": "int"},
        signal_type="alpha",
        edge_decay="days",
    ),
    ToolSpec(
        name="dry_dock_schedule",
        domain=ToolDomain.FUNDAMENTAL,
        description="Dry-docking and regulatory survey schedule. Vessels in dry-dock are temporarily removed from trading fleet. Bunching of dry-dock periods = temporary supply tightness. EEXI/CII compliance surveys add to downtime.",
        provider="Clarksons / ClassNK / Lloyd's Register",
        endpoint="https://www.clarksons.net/api/drydock",
        auth="subscription",
        latency=DataLatency.MONTHLY,
        cost_per_call=0.0,
        rate_limit="none",
        params={"vessel_class": "str", "quarter": "str"},
        returns={"vessels_in_dock": "int", "pct_of_fleet": "float",
                 "avg_dock_days": "float",
                 "eexi_compliance_pct": "float"},
        signal_type="context",
        edge_decay="days",
    ),
    ToolSpec(
        name="shipping_equity_prices",
        domain=ToolDomain.FUNDAMENTAL,
        description="Listed shipping company equity prices (Frontline, Euronav, Star Bulk, Golden Ocean, ZIM, Maersk, Hapag-Lloyd, COSCO, etc.). Equity = leveraged bet on freight rates. Equity-to-FFA divergence = alpha signal.",
        provider="Yahoo Finance / Alpha Vantage",
        endpoint="https://www.alphavantage.co/query",
        auth="api_key",
        latency=DataLatency.DELAYED,
        cost_per_call=0.0,
        rate_limit="75/min",
        params={"tickers": "list[str] — FRO, EURN, SBLK, GOGL, ZIM, APMM.CO, HLAG.DE"},
        returns={"prices": "dict of {ticker: {price, change_pct, volume, market_cap}}"},
        signal_type="alpha",
        edge_decay="minutes",
    ),
]


# ════════════════════════════════════════════════════════════════════════════
# DOMAIN 7: EXECUTION — Orders, Risk, Portfolio, Clearing
# ════════════════════════════════════════════════════════════════════════════

EXECUTION_TOOLS = [
    ToolSpec(
        name="cme_order_entry",
        domain=ToolDomain.EXECUTION,
        description="CME REST Order Entry API for freight futures. Submit, modify, cancel orders on CME Globex (cleared FFAs). Non-latency-sensitive — designed for programmatic strategies, not HFT.",
        provider="CME Group",
        endpoint="https://api.cmegroup.com/oe/v1/orders",
        auth="oauth",
        latency=DataLatency.REALTIME,
        cost_per_call=0.0,
        rate_limit="100/sec",
        params={"action": "str — buy, sell, cancel, modify",
                "product": "str — CME product code",
                "month": "str — contract month",
                "quantity": "int — lots",
                "price": "float — limit price",
                "order_type": "str — limit, market, stop",
                "tif": "str — GTC, DAY, IOC, FOK"},
        returns={"order_id": "str", "status": "str — working, filled, cancelled",
                 "fill_price": "float", "fill_qty": "int", "timestamp": "str"},
        signal_type="execution",
        edge_decay="seconds",
    ),
    ToolSpec(
        name="otc_ffa_rfq",
        domain=ToolDomain.EXECUTION,
        description="OTC FFA Request-for-Quote via broker platforms (Clarksons, SSY, Braemar, FIS). The majority of FFA volume is still OTC bilateral, cleared through SGX AsiaClear or EEX. Agent sends RFQ, receives bid/offer from broker panel.",
        provider="FIS / Clarksons / SSY / Braemar",
        endpoint="https://api.fis.com/freight/rfq",
        auth="oauth",
        latency=DataLatency.NEAR_RT,
        cost_per_call=0.0,
        rate_limit="10/min",
        params={"route": "str — Baltic route code",
                "direction": "str — buy, sell",
                "tenor": "str — MAR26, Q2-26, CAL27",
                "quantity": "int — lots",
                "clearing": "str — SGX, EEX, CME, bilateral"},
        returns={"bid": "float", "offer": "float", "mid": "float",
                 "broker": "str", "valid_until": "str"},
        signal_type="execution",
        edge_decay="seconds",
    ),
    ToolSpec(
        name="portfolio_state",
        domain=ToolDomain.EXECUTION,
        description="Current portfolio position and P&L state. All open FFA/futures positions, margin utilization, Greeks, VaR. The agent must check portfolio state before every trade decision.",
        provider="Internal PMS",
        endpoint="internal://portfolio",
        auth="none",
        latency=DataLatency.REALTIME,
        cost_per_call=0.0,
        rate_limit="unlimited",
        params={},
        returns={"positions": "list of {route, tenor, direction, qty, avg_price, mtm, pnl}",
                 "total_pnl": "float", "unrealized_pnl": "float",
                 "margin_used": "float", "margin_available": "float",
                 "var_95": "float", "max_drawdown": "float"},
        signal_type="execution",
        edge_decay="seconds",
    ),
    ToolSpec(
        name="risk_limits",
        domain=ToolDomain.EXECUTION,
        description="Risk limit checks before order submission. Max position per route, max portfolio VaR, max daily loss, max notional, concentration limits. Hard limits that the agent CANNOT override.",
        provider="Internal Risk Engine",
        endpoint="internal://risk-limits",
        auth="none",
        latency=DataLatency.REALTIME,
        cost_per_call=0.0,
        rate_limit="unlimited",
        params={"proposed_trade": "dict — route, direction, qty, price"},
        returns={"approved": "bool",
                 "limit_checks": "dict of {check_name: {passed: bool, current: float, limit: float}}",
                 "rejection_reason": "str or null"},
        signal_type="execution",
        edge_decay="seconds",
    ),
    ToolSpec(
        name="margin_calculator",
        domain=ToolDomain.EXECUTION,
        description="Initial and variation margin calculator for FFA positions. FFAs are cash-settled but require margin. Margin = f(volatility, position size, correlation offsets). Margin spike during crisis = forced liquidation risk.",
        provider="CME CORE / SGX / Internal",
        endpoint="https://www.cmegroup.com/tools-information/cme-core.html",
        auth="api_key",
        latency=DataLatency.NEAR_RT,
        cost_per_call=0.0,
        rate_limit="100/min",
        params={"positions": "list of {route, tenor, direction, qty}"},
        returns={"initial_margin": "float", "maintenance_margin": "float",
                 "variation_margin": "float", "margin_call_threshold": "float"},
        signal_type="execution",
        edge_decay="minutes",
    ),
    ToolSpec(
        name="execution_tca",
        domain=ToolDomain.EXECUTION,
        description="Transaction Cost Analysis. Post-trade analysis of execution quality: slippage vs VWAP, market impact, timing cost, broker comparison. Essential for agent self-improvement.",
        provider="Internal Analytics",
        endpoint="internal://tca",
        auth="none",
        latency=DataLatency.DELAYED,
        cost_per_call=0.0,
        rate_limit="unlimited",
        params={"trade_id": "str", "benchmark": "str — vwap, arrival, twap"},
        returns={"slippage_bps": "float", "market_impact_bps": "float",
                 "timing_cost_bps": "float", "total_cost_bps": "float"},
        signal_type="execution",
        edge_decay="hours",
    ),
]


# ════════════════════════════════════════════════════════════════════════════
# FULL TOOL REGISTRY
# ════════════════════════════════════════════════════════════════════════════

ALL_TOOLS = (
    MARKET_DATA_TOOLS +
    VESSEL_INTEL_TOOLS +
    CARGO_FLOW_TOOLS +
    MACRO_TOOLS +
    GEOPOLITICAL_TOOLS +
    FUNDAMENTAL_TOOLS +
    EXECUTION_TOOLS
)

TOOL_INDEX = {t.name: t for t in ALL_TOOLS}


# ════════════════════════════════════════════════════════════════════════════
# AGENT REASONING LOOP — How the tools compose into a trading strategy
# ════════════════════════════════════════════════════════════════════════════

AGENT_LOOP = """
┌─────────────────────────────────────────────────────────────────────┐
│                    FREIGHT QUANT AGENT LOOP                        │
│                                                                     │
│  Every 15 minutes (or on event trigger):                           │
│                                                                     │
│  1. SENSE                                                          │
│     ├─ gdelt_event_stream → geopolitical events last 15min         │
│     ├─ chokepoint_transit_count → all 6 chokepoints                │
│     ├─ ais_vessel_positions → fleet positioning changes            │
│     ├─ port_congestion → key loading/discharge ports               │
│     └─ war_risk_premiums → insurance market signal                 │
│                                                                     │
│  2. PRICE                                                          │
│     ├─ baltic_spot_rates → latest assessments                      │
│     ├─ baltic_ffa_curves → forward curve shape                     │
│     ├─ cme_freight_settlements → cleared futures prices            │
│     ├─ ice_brent_wti_spread → crude oil basis                     │
│     ├─ bunker_prices → voyage cost inputs                         │
│     └─ container_indices → box market context                      │
│                                                                     │
│  3. FLOW                                                           │
│     ├─ crude_cargo_flows → who's moving what where                 │
│     ├─ iron_ore_shipments → Capesize demand driver                │
│     ├─ lng_cargo_tracker → gas carrier demand                     │
│     ├─ floating_storage → contango/tonnage absorption             │
│     ├─ ton_mile_demand → the fundamental demand metric            │
│     └─ onshore_oil_storage → inventory vs consensus               │
│                                                                     │
│  4. CONTEXT                                                        │
│     ├─ treasury_yield_curve → recession/recovery signal           │
│     ├─ china_pmi → dry bulk demand leading indicator              │
│     ├─ china_steel_production → iron ore/Cape demand              │
│     ├─ opec_production → crude tanker supply driver               │
│     ├─ fleet_supply_database → structural supply                  │
│     ├─ orderbook_deliveries → future supply                       │
│     └─ shipping_equity_prices → equity-FFA divergence             │
│                                                                     │
│  5. DECIDE                                                         │
│     ├─ Signal aggregation across all domains                       │
│     ├─ Regime classification (crisis/normal/recovery)              │
│     ├─ Route-level alpha score (-100 to +100)                     │
│     ├─ Curve trade identification (calendar spreads)              │
│     ├─ Cross-asset relative value (tanker vs dry vs gas)          │
│     └─ Position sizing (Kelly criterion + risk limits)            │
│                                                                     │
│  6. EXECUTE                                                        │
│     ├─ risk_limits → pre-trade compliance check                   │
│     ├─ portfolio_state → current exposure                         │
│     ├─ margin_calculator → margin impact                          │
│     ├─ cme_order_entry OR otc_ffa_rfq → execute                  │
│     └─ execution_tca → post-trade quality analysis                │
│                                                                     │
│  7. LEARN                                                          │
│     ├─ P&L attribution by signal source                           │
│     ├─ Tool reliability scoring (which tools predicted well?)     │
│     ├─ Edge decay estimation (how fast does each signal decay?)   │
│     └─ Strategy parameter update (Bayesian)                       │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
"""

SIGNAL_CHAIN_EXAMPLES = """
═══════════════════════════════════════════════════════════════════════
EXAMPLE SIGNAL CHAINS — How tools compose into trade decisions
═══════════════════════════════════════════════════════════════════════

CHAIN 1: Hormuz Crisis Trade
  gdelt_event_stream("Hormuz blockade") → risk_score = 95
  chokepoint_transit_count("hormuz") → transits -70% vs avg
  war_risk_premiums("persian_gulf") → 0.25% → 0.50%
  ais_vessel_positions(bbox=Hormuz) → 150 tankers anchored
  floating_storage(region="MEG") → +200% → tonnage absorbed
  crude_cargo_flows(origin="MEG") → flows collapsed
  crude_cargo_flows(origin="WAF", dest="China") → flows +40%
  ton_mile_demand(class="VLCC") → ton-miles +25% (Cape diversion)
  baltic_ffa_curves("TD3C") → extreme backwardation
  ──────────────────────────────────────
  DECISION: Long TD3C front-month FFA, Long TD15 (WAf→China)
            Short TD3C Q3 (backwardation play)
            Long LPG VLGC USG→Japan

CHAIN 2: China Restocking Play
  china_pmi() → 52.3 (expanding, up from 49.8)
  china_steel_production() → +8% MoM restart
  iron_ore_shipments(origin="Brazil") → seasonal surge starting
  port_congestion("Qingdao") → vessels waiting +30%
  ton_mile_demand(class="Capesize") → rising
  fleet_supply_database("Capesize") → fleet growth only 1.2%
  orderbook_deliveries("Capesize") → OB/fleet = 8% (low)
  baltic_ffa_curves("C5") → contango (market hasn't priced in)
  shipping_equity_prices(["GOGL","SBLK"]) → equities leading FFAs
  ──────────────────────────────────────
  DECISION: Long Cape 5TC Q2 FFA, Long Cape vs Panamax spread

CHAIN 3: Sanctions Evasion Alpha
  sanctions_tracker(type="vessel") → 15 new VLCC sanctions this month
  dark_fleet_tracking(class="VLCC") → dark fleet = 12% of global VLCC
  ship_to_ship_transfers(region="SE_Asia") → STS events +60%
  ais_vessel_positions(vessel_type="tanker", speed=0) → unusual clustering
  fleet_supply_database("VLCC") → effective fleet shrinks by dark fleet %
  baltic_spot_rates(["TD3C","TD15"]) → rates elevated
  ──────────────────────────────────────
  DECISION: Compliant VLCC fleet is 12% smaller than headline.
            Structural rate support. Long VLCC FFAs across curve.

CHAIN 4: Container Dual-Chokepoint Crisis
  gdelt_event_stream("Houthi Red Sea") → attacks continuing
  chokepoint_transit_count("bab_el_mandeb") → -85% vs normal
  chokepoint_transit_count("hormuz") → -70% (new crisis)
  container_indices("FBX") → FBX01 (China-USWC) +40%
  port_congestion("Singapore") → massive congestion from rerouting
  vessel_speed_analysis("container") → speed UP (schedule recovery)
  ton_mile_demand("container") → +30% (Cape rerouting both ways)
  shipping_equity_prices(["ZIM","APMM.CO"]) → equities surging
  ──────────────────────────────────────
  DECISION: Container rates have further to run.
            Long ZIM/Maersk equity (leveraged box rate play).
            No direct container FFA liquidity — use equity proxy.
"""


# ── CLI: Print the full tool surface ────────────────────────────────────────

BOLD = "\033[1m"
DIM = "\033[2m"
CYAN = "\033[96m"
GREEN = "\033[92m"
YELLOW = "\033[93m"
RED = "\033[91m"
MAGENTA = "\033[95m"
RESET = "\033[0m"
BG_CYAN = "\033[46m"

DOMAIN_COLORS = {
    ToolDomain.MARKET_DATA: CYAN,
    ToolDomain.VESSEL_INTEL: GREEN,
    ToolDomain.CARGO_FLOWS: YELLOW,
    ToolDomain.MACRO: MAGENTA,
    ToolDomain.GEOPOLITICAL: RED,
    ToolDomain.FUNDAMENTAL: "\033[94m",  # blue
    ToolDomain.EXECUTION: "\033[97m",     # white
}

def render():
    print()
    print(f"  {BG_CYAN}{BOLD} FREIGHT QUANT AGENT — TOOL SURFACE {RESET}")
    print(f"  {DIM}Complete tool registry for autonomous freight/FFA quant fund{RESET}")
    print(f"  {'━' * 76}")
    print()

    # Summary
    by_domain = {}
    for t in ALL_TOOLS:
        by_domain.setdefault(t.domain, []).append(t)

    free_count = sum(1 for t in ALL_TOOLS if t.cost_per_call == 0)
    rt_count = sum(1 for t in ALL_TOOLS if t.latency in (DataLatency.REALTIME, DataLatency.NEAR_RT))
    alpha_count = sum(1 for t in ALL_TOOLS if t.signal_type == "alpha")

    print(f"  {BOLD}REGISTRY SUMMARY{RESET}")
    print(f"  Total tools:     {BOLD}{len(ALL_TOOLS)}{RESET}")
    print(f"  Domains:         {len(by_domain)}")
    print(f"  Alpha signals:   {alpha_count}")
    print(f"  Real-time:       {rt_count}")
    print(f"  Free/open:       {free_count}")
    print()

    # By domain
    for domain, tools in by_domain.items():
        color = DOMAIN_COLORS.get(domain, "")
        label = domain.value.upper().replace("_", " ")
        print(f"  {color}{BOLD}{'═' * 76}{RESET}")
        print(f"  {color}{BOLD}{label}{RESET}  ({len(tools)} tools)")
        print(f"  {color}{'─' * 76}{RESET}")

        for t in tools:
            latency_icon = {"realtime": "⚡", "near_realtime": "◉", "delayed": "◐",
                           "end_of_day": "○", "weekly": "◇", "monthly": "△"}.get(t.latency.value, "?")
            signal_icon = {"alpha": "α", "risk": "⚠", "execution": "▶", "context": "◈"}.get(t.signal_type, "?")
            auth_icon = {"none": "🔓", "api_key": "🔑", "subscription": "💳", "oauth": "🔐"}.get(t.auth, "?")
            cost_str = "FREE" if t.cost_per_call == 0 else f"${t.cost_per_call:.2f}"
            impl = "✓" if t.fn else " "

            print(f"  {color}{impl}{RESET} {BOLD}{t.name}{RESET}")
            print(f"    {DIM}{t.description[:100]}{'...' if len(t.description) > 100 else ''}{RESET}")
            print(f"    {latency_icon} {t.latency.value:<15s}  {signal_icon} {t.signal_type:<10s}  "
                  f"{auth_icon} {t.auth:<12s}  {cost_str:<8s}  decay:{t.edge_decay}")
            print(f"    {DIM}Provider: {t.provider}  │  Rate: {t.rate_limit}{RESET}")
            print()

    # Agent loop
    print(f"  {BOLD}{'═' * 76}{RESET}")
    print(AGENT_LOOP)

    # Signal chains
    print(SIGNAL_CHAIN_EXAMPLES)

    # Providers summary
    print(f"  {BOLD}DATA PROVIDER STACK{RESET}")
    print(f"  {'━' * 76}")
    providers = [
        ("Baltic Exchange",    "api.balticexchange.com",    "FFA curves, spot rates, indices",         "$2-5k/yr"),
        ("CME Group",          "cmegroup.com/market-data",  "Cleared FFA settlements, order entry",    "Per-trade"),
        ("SGX AsiaClear",      "sgx.com",                   "OTC FFA clearing, 40% dry bulk market",   "Per-trade"),
        ("Kpler/MarineTraffic", "api.kpler.com",            "AIS, cargo flows, freight analytics",     "$10-50k/yr"),
        ("Vortexa",            "api.vortexa.com",           "Energy cargo tracking, floating storage", "$20-80k/yr"),
        ("Spire Maritime",     "ais.spire.com",             "AIS positions, 350k vessels, 5s latency", "$5-20k/yr"),
        ("FRED",               "api.stlouisfed.org",        "Macro: yields, oil, VIX, FX, PMI",       "FREE"),
        ("EIA",                "api.eia.gov",               "Petroleum status, SPR, production",       "FREE"),
        ("GDELT",              "api.gdeltproject.org",      "Geopolitical events, sentiment, 100+ lang", "FREE"),
        ("OFAC/Sanctions",     "sanctionssearch.ofac.treas.gov", "SDN list, vessel blacklists",        "FREE"),
        ("Clarksons Research", "clarksons.net",             "Fleet DB, orderbook, S&P, NB prices",     "$15-30k/yr"),
        ("VesselsValue",       "vesselsvalue.com",          "Real-time vessel valuations, S&P",        "$5-15k/yr"),
        ("Windward AI",        "windward.ai",               "Dark fleet, sanctions evasion, risk",     "$10-30k/yr"),
        ("Ursa Space/Kayrros", "ursaspace.com",             "Satellite oil storage fill levels",       "$20-50k/yr"),
        ("Freightos",          "fbx.freightos.com",         "Container indices (FBX)",                 "FREE tier"),
        ("Ship&Bunker/Platts", "shipandbunker.com",         "Bunker fuel prices",                      "$1-5k/yr"),
        ("IMB Piracy Centre",  "icc-ccs.org",               "Piracy attack log, live map",             "FREE"),
    ]
    print(f"  {'Provider':<22s} {'Data':<45s} {'Cost':>12s}")
    print(f"  {'─' * 76}")
    for name, _, data, cost in providers:
        cost_color = GREEN if "FREE" in cost else YELLOW if "1-5k" in cost else RED
        print(f"  {name:<22s} {DIM}{data[:45]:<45s}{RESET} {cost_color}{cost:>12s}{RESET}")

    total_min = 2+0+5+10+20+5+0+0+0+0+15+5+10+20+0+1+0  # conservative
    total_max = 5+0+20+50+80+20+0+0+0+0+30+15+30+50+0+5+0
    print(f"  {'─' * 76}")
    print(f"  {BOLD}Total data cost:  ${total_min}k-${total_max}k/year{RESET}")
    print(f"  {DIM}vs Bloomberg Terminal: $24k/seat/yr (and doesn't have AIS, cargo flows, or satellite){RESET}")
    print()

    render_competitive_intel()


# ════════════════════════════════════════════════════════════════════════════
# COMPETITIVE INTELLIGENCE — Who Uses What
# ════════════════════════════════════════════════════════════════════════════

# Compiled from Institutional Investor, Neudata, FreightWaves, Hedge Fund
# Journal, AIMA, public filings, conference presentations, job postings,
# and vendor case studies as of March 2026.

FUND_STACKS = {
    # ── Quant / Systematic Funds ──
    "CargoMetrics": {
        "type": "Quant hedge fund (shipping-native)",
        "aum": "~$250M peak",
        "hq": "Boston",
        "founded": 2008,
        "strategy": "Systematic commodity futures + FX, driven by proprietary AIS signal extraction",
        "edge": "First-mover in AIS-to-alpha. Patented analytics platform. 1B+ data points/day. Decade of historical AIS.",
        "known_data": [
            ("AIS / Vessel Tracking", "Proprietary terrestrial + satellite AIS network", "CORE — this IS the fund"),
            ("Customs / Trade Data", "Proprietary ingestion from hundreds of sources", "Cargo type inference"),
            ("Baltic Exchange", "Member — spot rates + FFA curves", "Settlement benchmark"),
            ("FRED / Macro", "Treasury, oil, VIX", "Context signals"),
            ("Weather", "Unknown provider", "Commodity impact"),
        ],
        "known_investors": "Eric Schmidt (Google), Howard Morgan (RenTech co-founder), Paul Tudor Jones, Idan Ofer, Clarksons PLC, Maersk Tankers",
        "signal_licensing": "Yes — sells time series to other quant funds (hundreds of series, 10+ yr history)",
        "notes": "Maersk Tankers strategic partner. Originally pure fund, now hybrid fund + data vendor. Compass Platform is their SaaS product.",
    },

    "Citadel Commodities": {
        "type": "Multi-strategy hedge fund — commodities desk",
        "aum": "$65B+ (fund-wide), commodities P&L >$4B in 2023",
        "hq": "Miami / Chicago / London",
        "founded": 1990,
        "strategy": "Discretionary + systematic commodity trading across energy, metals, ags. Weather-driven.",
        "edge": "20-person PhD atmospheric science team. Terabytes/day of weather data → actionable forecasts. HPC cluster for weather modeling.",
        "known_data": [
            ("Weather / Atmospheric", "In-house PhD team + ECMWF/GFS/HRES models", "PRIMARY differentiator"),
            ("AIS / Vessel Tracking", "Kpler, Vortexa, likely CargoMetrics signals", "Commodity flow inference"),
            ("Satellite Imagery", "SpaceKnow (factory activity), Ursa/Kayrros (oil storage)", "Alt-data alpha"),
            ("Baltic Exchange", "Full data subscription", "Freight rate benchmarks"),
            ("Platts / Argus", "Price assessments across all commodities", "Physical market pricing"),
            ("Refinitiv/LSEG Eikon", "Broad commodity data terminal", "Reference data, news"),
            ("Bloomberg Terminal", "Enterprise deployment", "Execution, analytics"),
            ("CME/ICE/SGX", "Direct market data feeds", "Real-time futures"),
            ("FRED / EIA / USDA", "Government macro + supply data", "Fundamental context"),
            ("Neudata", "Alt-data marketplace for sourcing new datasets", "Dataset discovery"),
        ],
        "signal_licensing": "No — all proprietary",
        "notes": "Expanded commodity team significantly 2022-2024. Weather team is unique — 60+ PhD fields. Competition with Trafigura's algo teams.",
    },

    "Breakwave Advisors": {
        "type": "Freight-specialist fund / ETF advisor",
        "aum": "BDRY ETF (~$50M) + BWET ETF",
        "hq": "New York",
        "founded": 2017,
        "strategy": "Long-only freight futures ETFs. BDRY = 50% Cape + 40% Panamax + 10% Supra FFAs rolling 3-month weighted average.",
        "edge": "Baltic Exchange member. Pure-play freight exposure vehicle for institutional investors who can't trade FFAs directly.",
        "known_data": [
            ("Baltic Exchange", "Member — full data suite", "PRIMARY — indices settle the FFAs they hold"),
            ("CME / SGX", "Cleared FFA settlement prices", "Mark-to-market"),
            ("Clarksons Research", "Fleet supply, orderbook, S&P", "Fundamental context for publications"),
            ("AIS Data", "Likely Kpler or Signal Ocean", "Vessel positioning for research"),
        ],
        "signal_licensing": "No — publishes free research (Dry Bulk Primer, Shipping Correlation Matrix)",
        "notes": "3.50% expense ratio (!). BDRY is the only publicly traded dry bulk FFA ETF. Recently added BWET for tankers.",
    },

    # ── Physical Trading Houses ──
    "Trafigura": {
        "type": "Physical commodity trading house + Galena hedge fund arm",
        "aum": "$318B revenue (2023), Galena fund arm",
        "hq": "Geneva / Singapore",
        "founded": 1993,
        "strategy": "Physical commodity trading (oil, metals, minerals). Freight as integral cost/profit center. FFA hedging. Galena runs paper trading strategies.",
        "edge": "Massive physical flow = proprietary information on actual cargo movements. Own storage, vessels, terminals. IT staff grew 60% while trading staff grew 15%.",
        "known_data": [
            ("Kpler", "Enterprise — cargo flow tracking, AIS, freight analytics", "Primary digital intelligence"),
            ("Vortexa", "Enterprise — energy cargo tracking", "Triangulation with Kpler"),
            ("S&P Global / Platts", "Price assessments, shipping benchmarks", "Physical market pricing"),
            ("Baltic Exchange", "Full data subscription", "FFA settlement, freight indices"),
            ("Clarksons", "Fleet data, fixture reports", "Market fundamentals"),
            ("LSEG/Refinitiv", "Eikon terminals, shipping module", "Broad commodity data"),
            ("Internal AIS/CTRM", "Proprietary CTRM system built on top of vendor data", "Unified trading platform"),
            ("Weather", "MeteoGroup or similar", "Cargo routing, seasonal demand"),
            ("Satellite Storage", "Kayrros or Ursa for storage monitoring", "Inventory intelligence"),
        ],
        "signal_licensing": "No — all proprietary. Operational data is the moat.",
        "notes": "Galena Asset Management (hedge fund arm) est. 2003. Trafigura's operational integration (source → logistics → freight → storage → risk) IS the alpha.",
    },

    "Vitol": {
        "type": "Physical commodity trading house (world's largest independent oil trader)",
        "aum": "$500B+ revenue (2022), profit >$15B",
        "hq": "Geneva / Rotterdam / Houston / Singapore",
        "founded": 1966,
        "strategy": "Physical oil + refined products trading. Freight = cost center + profit center. World's largest independent energy trader by volume.",
        "edge": "Trades ~7M bbl/day. Lifted volumes +33% in 5 years with only +15% staff. IT staff +60%. CIO Gerard Delsad: 'Classical research just isn't enough.'",
        "known_data": [
            ("Kpler", "Enterprise-wide deployment", "Cargo flow intelligence"),
            ("Vortexa", "Enterprise subscription", "Energy cargo tracking + floating storage"),
            ("S&P Global / Platts", "Price assessments", "Benchmark pricing"),
            ("Argus Media", "Price assessments + freight", "Alternative price reference"),
            ("Baltic Exchange", "Full subscription", "Freight indices"),
            ("Bloomberg / LSEG", "Terminal deployment", "Execution + reference data"),
            ("Internal Proprietary", "Custom-built CTRM, risk, logistics systems", "Integration layer"),
            ("Satellite / Alt Data", "Increasing investment — specifics not public", "Forward-looking supply"),
            ("Weather", "Internal + vendor hybrid", "Routing, demand forecasting"),
        ],
        "signal_licensing": "No",
        "notes": "Gunvor CEO Tornqvist: 'Traders traditionally live on imperfections of the market. Digital technologies are erasing a lot of these opportunities.' — this is the existential threat to all physical traders.",
    },

    "Glencore": {
        "type": "Physical commodity trading + mining conglomerate",
        "aum": "$220B+ revenue, metal trading EBIT $1.57B (H1 2025 record)",
        "hq": "Baar, Switzerland",
        "founded": 1974,
        "strategy": "Integrated mining + trading. Physical flow in metals, energy, ags. Freight on owned + chartered tonnage.",
        "edge": "Owns mines, smelters, storage, ports, vessels. Information from physical operations = unmatched real-time commodity intelligence.",
        "known_data": [
            ("Kpler", "Enterprise deployment", "Trade flow tracking"),
            ("S&P Global / Platts", "Full commodity price suite", "Benchmark pricing"),
            ("Bloomberg", "Enterprise", "Execution + analytics"),
            ("Clarksons / SSY", "Freight market data + fixture intelligence", "Chartering operations"),
            ("Baltic Exchange", "Subscription", "FFA settlement"),
            ("Internal Operational Data", "Own mine/smelter/port throughput", "This IS the edge — no vendor can replicate"),
            ("Wood Mackenzie", "Energy + mining research", "Long-term fundamentals"),
        ],
        "signal_licensing": "No",
        "notes": "Glencore's edge is physical asset ownership, not data. Their traders know what's happening because they own the supply chain.",
    },

    # ── Maritime Intelligence Platforms (used by funds) ──
    "Signal Ocean (The Signal Group)": {
        "type": "Maritime intelligence platform — now includes AXSMarine",
        "aum": "N/A (data vendor)",
        "hq": "London / Athens",
        "founded": 2018,
        "strategy": "AI-powered maritime decision platform. Acquired AXSMarine (25yr-old chartering tools, 18K+ users, Alphaliner container DB).",
        "edge": "Patented data fusion: AIS + tonnage lists + cargo lists + port data + freight rates. AI-predicted vessel movements and availability.",
        "known_data": [
            ("Proprietary AIS Processing", "Own network + satellite + dynamic AIS", "50,000+ proprietary port/berth polygons"),
            ("AXSMarine Integration", "AXSDry, AXSTanker, Alphaliner, trade flows", "25 years of fixture/chartering data"),
            ("General Index", "Benchmark freight prices", "Daily market pricing"),
            ("Emissions Data", "CII, EEOI, AER metrics", "Regulatory compliance"),
        ],
        "signal_licensing": "Yes — Data Warehouse + API suite for quant integration (SQL, Python, Power BI, Tableau)",
        "notes": "AXSMarine acquisition makes this a potential one-stop-shop: chartering tools + market data + AI analytics. Financial markets module specifically targets hedge funds.",
    },

    "Oceanbolt (Veson Nautical)": {
        "type": "Dry bulk commodity intelligence platform",
        "aum": "N/A (data vendor, acquired by Veson 2021)",
        "hq": "Oslo → Boston (Veson)",
        "founded": 2019,
        "strategy": "AIS + commodity identification for dry bulk. 30B+ data points, 60M+ daily observations, 5B+ tons/yr tracked across 140+ commodities.",
        "edge": "Best-in-class commodity identification from AIS. 14,000 berth polygons × 12,000 bulk vessels × ML = knows what's in every bulker.",
        "known_data": [
            ("Proprietary AIS Engine", "Custom processing + berth polygon DB", "Commodity-linked vessel tracking"),
            ("Berth-Level Intelligence", "14,000 berths mapped to commodity types", "Cargo inference without customs data"),
            ("Trade Flow Models", "ML-based commodity identification", "140+ commodities tracked"),
        ],
        "signal_licensing": "Yes — Python SDK, R SDK, Excel, Power BI, Tableau, REST API",
        "notes": "K-Line (major Japanese shipowner) is a known client. API-first design. Strongest in dry bulk; less coverage for tanker/container vs Kpler/Vortexa.",
    },

    # ── Satellite / Alt-Data Specialists ──
    "PierSight": {
        "type": "SAR + AIS satellite constellation (pre-revenue, launching 2026)",
        "aum": "$8M total funding",
        "hq": "India",
        "founded": 2023,
        "strategy": "Own SAR + AIS-2.0 (VDES) satellites for persistent maritime surveillance. 1m resolution. 30-min revisit. First commercial satellite mid-2026.",
        "edge": "SAR penetrates weather/darkness. Detects dark vessels that turn off AIS. Ship-to-ship transfers visible regardless of transponder status.",
        "known_data": [
            ("Own SAR Constellation", "Launching mid-2026", "1m resolution, all-weather"),
            ("AIS-2.0 (VDES)", "Next-gen AIS onboard same satellite", "Dual sensor fusion"),
            ("MATSYA Platform", "Internal SAR + AIS processing", "Dark vessel detection, STS identification"),
        ],
        "signal_licensing": "Early access program open — targeting hedge funds + governments",
        "notes": "Backed by CE-Ventures (Gulftainer parent). Competes with ICEYE ($304M funded), Capella Space, Umbra. Watch this space — SAR + AIS fusion on same satellite is novel.",
    },

    "Ursa Space Systems": {
        "type": "SAR-based oil storage intelligence",
        "aum": "N/A (data vendor)",
        "hq": "Ithaca, NY",
        "founded": 2015,
        "strategy": "Measure floating-roof tank fill levels globally via SAR. 1.7M tanks measured, 389B barrels tracked. Cushing correlation with EIA: 0.98.",
        "edge": "Monday delivery = 2 days before Wednesday EIA report. Global coverage including China (where govt data is opaque).",
        "known_data": [
            ("SAR Imagery", "ICEYE, Capella, Umbra, Airbus", "Multi-vendor SAR procurement"),
            ("Tank Database", "1.7M floating roof tanks globally", "Complete global inventory"),
            ("Processing Pipeline", "Proprietary shadow-measurement algorithms", "Fill level from roof shadow geometry"),
        ],
        "signal_licensing": "Yes — STAC-compliant API, ordering API, custom AOI support. Designed for quant fund integration.",
        "notes": "Hedge funds buy Cushing/Fujairah/Singapore/China storage data to front-run EIA/API reports. This is a well-established alt-data play now.",
    },

    # ── Traditional Data Terminals ──
    "Clarksons Research": {
        "type": "Shipping market data & research (est. 1852)",
        "aum": "N/A (part of Clarkson PLC, ~£700M market cap)",
        "hq": "London",
        "founded": 1852,
        "strategy": "The definitive source for fleet data, orderbook, S&P transactions, newbuild prices, and shipping market fundamentals. Every shipping analyst's bible.",
        "edge": "172 years of data. Broadest coverage. Clarksons Shipping Intelligence Network (SIN) is the industry standard reference.",
        "known_data": [
            ("Fleet Database", "Every commercial vessel globally", "Hull, DWT, age, owner, flag, class"),
            ("Orderbook", "Every vessel on order at every yard", "Delivery schedule, slippage rates"),
            ("S&P Transactions", "Every reported vessel sale", "Asset valuations, buyer/seller intelligence"),
            ("Fixture Reports", "Reported chartering fixtures", "Rate discovery"),
            ("Newbuild Prices", "Yard quotes by vessel type", "Cycle positioning indicator"),
            ("Demolition Data", "Scrapping activity by breaker location", "Supply removal tracking"),
            ("Time Series", "Decades of historical rates, prices, volumes", "Backtesting, fundamental models"),
        ],
        "signal_licensing": "Yes — SIN (Shipping Intelligence Network) + API",
        "notes": "Also an investor in CargoMetrics. Every fund uses Clarksons in some form. The data is expensive but irreplaceable for fundamental analysis.",
    },
}

MARKET_STRUCTURE = """
═══════════════════════════════════════════════════════════════════════
MARKET STRUCTURE: WHO KNOWS WHAT AND WHEN
═══════════════════════════════════════════════════════════════════════

Information flows in freight markets are asymmetric. Understanding
who has what data — and when — is the meta-game.

TIER 1: REAL-TIME PHYSICAL FLOW (minutes)
  ├─ Physical traders (Vitol, Trafigura, Glencore, Gunvor, Mercuria)
  │   → They ARE the market. Their own cargo = ground truth.
  │   → They know what they're shipping before anyone else.
  │   → Risk: they know THEIR flows, not competitors'.
  │
  ├─ Kpler / Vortexa (cargo flow platforms)
  │   → AIS + ML = near-real-time cargo tracking for ALL vessels.
  │   → They see the market-wide picture the physicals can't.
  │   → Latency: minutes to hours for cargo identification.
  │
  └─ CargoMetrics (quant fund + data vendor)
      → Proprietary AIS signal extraction → futures trading signals.
      → 10+ year history = backtestable. Sells signals to other quants.

TIER 2: DAILY ASSESSED PRICES (T+0 evening)
  ├─ Baltic Exchange → spot rates, FFA curves (the benchmark)
  ├─ S&P Global Platts → physical crude/product price assessments
  ├─ Argus Media → alternative price assessments
  ├─ CME / SGX → cleared FFA settlement prices
  └─ Ship&Bunker → bunker fuel prices

TIER 3: ALTERNATIVE DATA (hours to days edge)
  ├─ Satellite oil storage (Ursa, Kayrros)
  │   → 2-3 day edge over EIA Wednesday report
  │
  ├─ SAR dark vessel detection (PierSight, ICEYE, Windward)
  │   → See ships that turn off AIS (sanctions evasion)
  │   → Reveals true effective fleet supply
  │
  ├─ GDELT / news sentiment
  │   → 15-minute event detection for geopolitical triggers
  │   → Strait of Hormuz, Red Sea, Taiwan Strait monitoring
  │
  └─ Weather (Citadel's 20 PhDs, ECMWF, GFS)
      → Agricultural demand + shipping route optimization
      → 3-5 day forecast edge for seasonal commodity flows

TIER 4: FUNDAMENTAL / STRUCTURAL (days to months)
  ├─ Clarksons Research → fleet DB, orderbook, S&P, NB prices
  ├─ VesselsValue → real-time vessel valuations
  ├─ EIA / IEA / OPEC → government supply/demand reports
  ├─ FRED → macro context (yields, PMI, credit)
  └─ China NBS → steel production, PMI, trade balance

TIER 5: ACADEMIC / STRUCTURAL (months to years)
  ├─ Fleet orderbook-to-fleet ratio → 2-3 year supply forecast
  ├─ Shipyard capacity utilization → build capacity constraints
  ├─ IMO regulations (EEXI/CII) → effective supply reduction
  └─ Trade pattern shifts (de-globalization, friend-shoring)

THE EDGE HIERARCHY:
  Physical flow > Satellite/AIS > Assessed prices > Government reports > Consensus
  Speed of signal > Accuracy of signal > Breadth of signal

WHERE THE ALPHA ACTUALLY IS:
  1. Physical flow information (physicals' edge — hard to replicate)
  2. AIS → cargo inference before official customs data (CargoMetrics' edge)
  3. Satellite storage before EIA (Ursa/Kayrros' edge)
  4. Weather → commodity demand (Citadel's edge)
  5. Geopolitical event detection → chokepoint risk (GDELT + AIS composit)
  6. Dark fleet sizing → true effective supply (SAR providers' edge)
  7. Cross-asset dislocation (FFA vs equity vs physical — structural)

WHAT NO ONE HAS (yet):
  - Real-time cargo content identification (AIS shows vessel, not cargo)
  - True OTC FFA bid/offer book (bilateral, opaque market)
  - Complete dark fleet coverage (SAR constellations still building out)
  - Real-time Chinese strategic reserves data (state secret)
  - Forward freight rate expectations from physical charterers (locked in emails)
"""

def render_competitive_intel():
    """Render the competitive intelligence section."""
    print(f"  {BOLD}{'═' * 76}{RESET}")
    print(f"  {BOLD}COMPETITIVE INTELLIGENCE: WHO USES WHAT{RESET}")
    print(f"  {DIM}Compiled from Institutional Investor, Neudata, FreightWaves, vendor case")
    print(f"  studies, job postings, conference presentations, public filings{RESET}")
    print(f"  {'━' * 76}")
    print()

    for name, info in FUND_STACKS.items():
        fund_type = info["type"]
        if "hedge fund" in fund_type.lower() or "quant" in fund_type.lower():
            color = CYAN
        elif "trading house" in fund_type.lower() or "Physical" in fund_type:
            color = YELLOW
        elif "platform" in fund_type.lower() or "vendor" in fund_type.lower() or "intelligence" in fund_type.lower():
            color = GREEN
        elif "satellite" in fund_type.lower() or "SAR" in fund_type.lower():
            color = MAGENTA
        else:
            color = ""

        print(f"  {color}{BOLD}{name}{RESET}")
        print(f"  {DIM}{info['type']}{RESET}")
        if "aum" in info:
            print(f"  AUM/Scale: {info['aum']}")
        print(f"  HQ: {info.get('hq', '?')}  │  Founded: {info.get('founded', '?')}")
        print(f"  Strategy: {info['strategy'][:90]}{'...' if len(info['strategy']) > 90 else ''}")
        print(f"  {RED}Edge: {info['edge'][:100]}{'...' if len(info['edge']) > 100 else ''}{RESET}")

        if "known_data" in info:
            print(f"  {BOLD}Data Stack:{RESET}")
            for data_name, provider, use in info["known_data"]:
                print(f"    ├─ {data_name:<28s} {DIM}{provider[:35]:<35s}{RESET}  {use[:30]}")

        if info.get("known_investors"):
            investors = info["known_investors"]
            print(f"  {DIM}Investors: {investors[:80]}{'...' if len(investors) > 80 else ''}{RESET}")

        if info.get("signal_licensing"):
            print(f"  Signal licensing: {info['signal_licensing'][:70]}")

        if info.get("notes"):
            print(f"  {DIM}Notes: {info['notes'][:100]}{'...' if len(info['notes']) > 100 else ''}{RESET}")

        print()

    print(MARKET_STRUCTURE)

    # Neudata market stats
    print(f"  {BOLD}ALT-DATA MARKET STATS (Neudata 2025 Report){RESET}")
    print(f"  {'─' * 76}")
    print(f"  Alt-data market size:       $18.74B (2025) → projected $135.8B by 2030")
    print(f"  Commodity data demand:       +577% YoY in client requests (2023)")
    print(f"  Avg datasets per fund:       20 datasets/year")
    print(f"  Avg spend per fund:          $1.6M/year on alt-data")
    print(f"  Budget outlook:              95% maintaining or increasing (2025)")
    print(f"  Top alt-data consumers:      16/20 top discretionary HFs use Neudata")
    print(f"                               8/10 top quant HFs use Neudata")
    print(f"  Citadel commodity P&L:       >$4B (2023)")
    print(f"  Kpler funding:               $200M (Insight Partners)")
    print(f"  Vortexa funding:             $34M (Series C)")
    print(f"  PierSight funding:           $8M (SAR+AIS satellites)")
    print()


# ── POWER PLAYERS: KEY PEOPLE IN FREIGHT / SHIPPING INTELLIGENCE ──────────

@dataclass
class PersonProfile:
    name: str
    role: str
    org: str
    org_type: str  # "quant_fund", "trading_house", "data_platform", "satellite", "benchmark", "research", "sell_side"
    education: str
    career_path: list  # list of strings, chronological
    key_achievement: str
    why_matters: str
    location: str


KEY_PEOPLE = [
    # ── QUANT FUNDS & HEDGE FUNDS ──
    PersonProfile(
        name="Scott Borgerson",
        role="Founder & former CEO",
        org="CargoMetrics",
        org_type="quant_fund",
        education="US Coast Guard Academy → Fletcher School (Tufts) MA → Fletcher School PhD",
        career_path=[
            "US Coast Guard officer, deployed worldwide",
            "Council on Foreign Relations fellow — ocean governance",
            "Co-founded CargoMetrics 2008/2010 in Boston with Fletcher classmate Rockford Weitz",
            "Recruited team of astrophysicists, mathematicians, quants",
            "Attracted Eric Schmidt, Paul Tudor Jones, Howard Morgan (RenTech co-founder) as investors",
            "Resigned as CEO Oct 2020",
            "Now involved in clean energy (H2X Global)",
        ],
        key_achievement="First to build a systematic hedge fund around AIS vessel tracking data. Patented platform processes billions of maritime data points to predict commodity flows before official statistics.",
        why_matters="Pioneer who proved that shipping data = tradeable alpha. Every cargo-tracking quant fund exists in CargoMetrics' wake.",
        location="Boston, MA",
    ),
    PersonProfile(
        name="Sebastian Barrack",
        role="Head of Commodities & Portfolio Committee Member",
        org="Citadel LLC",
        org_type="quant_fund",
        education="Bachelor's, Australian National University",
        career_path=[
            "20+ years at Macquarie Group building agriculture, energy, metals trading globally",
            "Global Co-Head of Metals, Mining & Agriculture at Macquarie (Sydney)",
            "Joined Citadel 2017 as Head of Commodities",
            "Built team including 20 PhD atmospheric scientists for weather-driven trading",
            "Oversees natural gas, power, crude, refined products, agriculture",
        ],
        key_achievement="Citadel commodities P&L exceeded $4B in 2023. Largest alt-manager commodity operation globally.",
        why_matters="Runs the single most powerful commodity trading desk outside physical trading houses. Citadel's weather-as-alpha approach is the benchmark for data-driven commodity trading.",
        location="Chicago / New York",
    ),
    PersonProfile(
        name="John Kartsonas",
        role="Founder & Managing Partner",
        org="Breakwave Advisors",
        org_type="quant_fund",
        education="Wall Street analyst background (specific university undisclosed)",
        career_path=[
            "2004-2009: Lead Transportation Analyst at Citi Investment Research",
            "Co-Founder of Sea Advisors Fund (shipping-focused)",
            "2011-2017: Senior PM at Carlyle Commodity Management — managed one of the largest freight futures funds globally",
            "2017: Founded Breakwave Advisors, registered as CTA with NFA",
            "2018: Launched BDRY (first-ever freight futures ETF)",
            "Launched BWET (tanker freight ETF)",
        ],
        key_achievement="Created freight-as-an-asset-class for retail/institutional investors. BDRY returned +283% in 2021 (best-performing ETF that year).",
        why_matters="Democratized freight market investing. Before BDRY, FFA exposure required institutional clearing. Now anyone can trade freight via exchange-listed products.",
        location="New York, NY",
    ),

    # ── DATA PLATFORM FOUNDERS ──
    PersonProfile(
        name="Fabio Kuhn",
        role="Co-Founder & CEO",
        org="Vortexa",
        org_type="data_platform",
        education="Harvard MLA (Information Management Systems) + Stanford Quant Finance/Risk Mgmt Certificate",
        career_path=[
            "Internet entrepreneur at 18 — built and sold a company",
            "Quant trader, statistical arbitrage at SF hedge fund",
            "Researcher at NASA Ames Research Center",
            "Technology leadership at Uniper (Düsseldorf)",
            "Head of Trading Technology at BP for 5 years",
            "2016: Co-founded Vortexa with Etienne Amic",
            "Raised $64M total (Series C), Morgan Stanley Expansion Capital",
        ],
        key_achievement="Built Vortexa into the dominant real-time waterborne energy cargo tracking platform — processing $1.8T+ in energy trades/year.",
        why_matters="Vortexa is to oil cargo tracking what Bloomberg is to financial data. Every major oil trader, refiner, and NOC uses it.",
        location="London, UK",
    ),
    PersonProfile(
        name="Etienne Amic",
        role="Co-Founder & former Chairman",
        org="Vortexa / VAKT / CommodiTech Ventures",
        org_type="data_platform",
        education="Theoretical physics degree",
        career_path=[
            "Managing Director at JP Morgan Chase — ran European energy business",
            "Executive Committee at Mercuria Energy Trading (after Mercuria acquired JPM energy)",
            "2016: Co-founded Vortexa with Fabio Kuhn",
            "2017: Co-founded CommodiTech Ventures (commodities tech investment fund, London)",
            "Became CEO of VAKT (blockchain post-trade platform for physical commodities)",
        ],
        key_achievement="Brought deep commodity trading network from JPM/Mercuria to Vortexa launch. Now building the post-trade infrastructure layer for physical commodities.",
        why_matters="Connected the Vortexa vision to the institutional commodity trading world. Bridges old-school physical trading with new-world data intelligence.",
        location="London, UK",
    ),
    PersonProfile(
        name="François Cazor",
        role="Co-Founder",
        org="Kpler",
        org_type="data_platform",
        education="Engineering degree (France)",
        career_path=[
            "Fixed-income trader",
            "Met co-founder Jean Maynier in NYC in 2009 during financial crisis",
            "Noticed CO2 trading market opportunity",
            "2014: Co-founded Kpler, starting with LNG cargo tracking",
            "Grew to 750+ employees, $100M+ ARR",
            "Sept 2024: Transitioned CEO to Mark Cunningham (ex-Wood Mackenzie)",
        ],
        key_achievement="Built Kpler from LNG tracker to full commodity intelligence platform. Raised $200M from Insight Partners. Now covers crude, LPG, dry bulk, agriculture.",
        why_matters="Kpler's AIS + ML cargo identification system is used by nearly every major commodity trading house and bank. Direct competitor to Vortexa, broader commodity coverage.",
        location="Paris / Brussels / London",
    ),
    PersonProfile(
        name="Jean Maynier",
        role="Co-Founder",
        org="Kpler",
        org_type="data_platform",
        education="Engineering degree (France)",
        career_path=[
            "Technology/data background",
            "Met François Cazor in NYC 2009",
            "2014: Co-founded Kpler",
            "Built the technical foundation for Kpler's cargo tracking platform",
        ],
        key_achievement="Technical architect behind Kpler's data fusion engine that processes AIS, satellite imagery, trade documents, and port data into unified cargo flow intelligence.",
        why_matters="The engineering brain behind one of the two dominant cargo intelligence platforms.",
        location="Paris / Brussels",
    ),
    PersonProfile(
        name="Ioannis Martinos",
        role="Founder & CEO",
        org="Signal Ocean / Signal Group",
        org_type="data_platform",
        education="Tufts Mechanical Engineering + MIT Aeronautical Engineering",
        career_path=[
            "Greek shipping dynasty — Thenamaris family (one of the largest tanker operators)",
            "Co-CEO of Thenamaris (100+ vessels)",
            "2014: Founded Signal Group — Signal Maritime (physical ship mgmt) + Signal Ocean (SaaS)",
            "Built patented data fusion platform combining AIS, port intel, commercial data",
            "Jan 2026: Acquired AXSMarine (25-year-old platform, 18K users) to consolidate position",
        ],
        key_achievement="Created 'a Bloomberg for shipping' — all but one oil major uses Signal Ocean. The AXSMarine acquisition created the most comprehensive shipping analytics suite.",
        why_matters="Only major shipping data platform founded by someone who actually operates ships. Unique physical + digital insight. Greek shipping connections = unmatched industry access.",
        location="Athens / London",
    ),

    # ── OCEANBOLT / VESON ──
    PersonProfile(
        name="Niclas Daehli Priess",
        role="Co-Founder (now Director of Product Management, Veson Nautical)",
        org="Oceanbolt / Veson Nautical",
        org_type="data_platform",
        education="Norwegian background",
        career_path=[
            "2019: Co-founded Oceanbolt in Norway — dry bulk trade flow intelligence",
            "Built AIS processing engine + proprietary geospatial berth polygon database (14K berths)",
            "Sept 2021: Acquired by Veson Nautical",
            "Now leads Oceanbolt product within Veson",
        ],
        key_achievement="Built the go-to platform for real-time dry bulk commodity flow analysis. 140+ commodities tracked, Python/R SDK for quant integration.",
        why_matters="Oceanbolt fills the dry bulk intelligence gap that Vortexa/Kpler fill for energy. Inside Veson's IMOS ecosystem (used by most major charterers), it's the analytics layer.",
        location="Oslo / Boston",
    ),

    # ── SATELLITE INTELLIGENCE ──
    PersonProfile(
        name="Adam Maher",
        role="CEO & Co-Founder",
        org="Ursa Space Systems",
        org_type="satellite",
        education="Cornell Engineering ecosystem",
        career_path=[
            "Co-founded Ursa in 2014 (Ithaca, NY) with Julie Baker and Derek Edinger",
            "Pioneered commercial SAR-based oil storage monitoring",
            "Built Global Oil Storage inventory report — flagship product",
            "Asset-light model: accesses every commercial SAR satellite without owning any",
            "Revenue $10.5M by 2025",
            "Named Via Satellite Top 10 Hottest Satellite Companies (2020)",
        ],
        key_achievement="Cushing oil storage estimates correlate 0.98 with EIA official data — released days earlier. Commodity funds use Ursa to front-run government statistics.",
        why_matters="Go-to provider of satellite-derived oil inventory data. Every energy-focused hedge fund uses Ursa or a competitor tracking Ursa's model.",
        location="Ithaca, NY",
    ),
    PersonProfile(
        name="Gaurav Seth",
        role="CEO & Co-Founder",
        org="PierSight Space",
        org_type="satellite",
        education="Indian Institute of Space Science and Technology (IIST)",
        career_path=[
            "8 years as scientist at ISRO Space Applications Centre — SAR satellite projects",
            "Met co-founder Vinit Bansal on ISRO project in 2019",
            "Sept 2023: Co-founded PierSight",
            "Built 'Varuna' — India's first private SAR satellite, in 9 months",
            "Raised $8M from Elevation Capital, Alpha Wave Global, All In Capital",
            "Building world's first combined SAR + AIS-2.0/VDES satellite (launch mid-2026)",
        ],
        key_achievement="Building maritime-focused SAR constellation at a fraction of existing SAR data costs. MATSYA AI platform makes SAR data queryable via natural language.",
        why_matters="If PierSight's constellation delivers, it will dramatically lower the cost of maritime surveillance data — potentially disrupting the $10K+/image SAR market.",
        location="Ahmedabad, India",
    ),

    # ── BENCHMARKS & RESEARCH ──
    PersonProfile(
        name="Jeremy Penn",
        role="Former CEO (retired ~2016)",
        org="Baltic Exchange",
        org_type="benchmark",
        education="Harvard Business School",
        career_path=[
            "Managing Director, Reuters Asia",
            "CEO, Baltic Exchange for ~12.5 years (2004-2016)",
            "Championed benchmark governance — influenced IOSCO benchmark standards",
            "Oversaw SGX acquisition of Baltic Exchange",
            "Succeeded by Mark Jackson",
        ],
        key_achievement="Led the Baltic Exchange through financialization of freight — BDI became a global macro indicator. Set governance standards for shipping benchmarks.",
        why_matters="The Baltic indices Penn governed (BDI, BDTI, BCTI, route assessments) are the foundational pricing layer for the entire FFA derivatives market.",
        location="London, UK",
    ),
    PersonProfile(
        name="Martin Stopford",
        role="Non-Executive President, Clarkson Research Services",
        org="Clarksons Research",
        org_type="research",
        education="PPE at Keble College, Oxford (Abbott Scholar) + PhD Economics, Birkbeck College London",
        career_path=[
            "1977: Group Economist at British Shipbuilders → Director of Business Development (1981)",
            "1988: Global Shipping Economist at Chase Manhattan Bank",
            "1990-2012: Managing Director of Clarkson Research — grew to 200+ products",
            "Joined main Clarksons board 2004",
            "2012: Retired, became Non-Exec President of CRSL",
            "Visiting Professor at Bayes Business School, Dalian Maritime, Copenhagen Business School",
        ],
        key_achievement="Author of 'Maritime Economics' (3rd ed, 2009) — THE definitive shipping economics textbook. Lloyd's List Lifetime Achievement Award (2010).",
        why_matters="Built the intellectual and data foundation that made Clarksons Research the gold standard for shipping market intelligence. 172 years of data under one roof.",
        location="London, UK",
    ),
    PersonProfile(
        name="Andi Case",
        role="Chief Executive Officer",
        org="Clarkson PLC",
        org_type="research",
        education="Career shipbroker",
        career_path=[
            "Started at C W Kellock, then Eggar Forrester (shipbroking)",
            "Head of Sale and Purchase at Braemar Seascope",
            "2006: Joined Clarksons as MD of H Clarkson & Company (shipbroking arm)",
            "2008: Promoted to Group CEO",
            "Lloyd's List Top 100 every year 2010-2018",
        ],
        key_achievement="Runs the world's largest shipbroking and shipping services company. Under his leadership Clarksons became a one-stop shop: broking, research, finance, project development.",
        why_matters="Controls the commercial empire that houses the industry's most trusted data. Clarksons' broker intelligence feeds back into their research products.",
        location="London, UK",
    ),

    # ── SELL-SIDE / ANALYSTS ──
    PersonProfile(
        name="Ben Nolan",
        role="Former Senior Analyst (now at Venture Global LNG)",
        org="Stifel Financial (formerly)",
        org_type="sell_side",
        education="BBA Finance, Texas A&M + MBA, University of Houston + CFA",
        career_path=[
            "Corporate financial analyst at EOG Resources (oil & gas)",
            "6 years at Jefferies as Equity Research Analyst covering shipping",
            "Knight Capital — maritime equity and debt",
            "2013-2025: Managing Director, Transportation Sector at Stifel",
            "Left mid-2025 for Venture Global LNG",
        ],
        key_achievement="Multiple StarMine and Institutional Investor awards. Considered the most respected shipping equity analyst on Wall Street for 20+ years.",
        why_matters="His research directly moved shipping stock prices and capital allocation decisions. The institutional investor bridge between freight fundamentals and equity markets.",
        location="Chicago, IL",
    ),
    PersonProfile(
        name="Omar Nokta",
        role="Senior Equity Analyst (returning to Clarksons Jan 2026)",
        org="Jefferies / Clarksons Capital Markets",
        org_type="sell_side",
        education="MBA, Fordham + BA, Texas A&M",
        career_path=[
            "2004-2013: Senior Research Analyst at Dahlman Rose & Co.",
            "Senior Shipping Analyst, Global Hunter Securities",
            "2015-2022: CEO & Head of US, Clarkson Capital Markets",
            "2022-2025: Senior Equity Analyst at Jefferies — covered marine shipping",
            "Jan 2026: Returning to Clarksons",
        ],
        key_achievement="62-66% success rate on recommendations, 300+ stock ratings, ~13.6% avg return per rating. Covers 27 stocks across shipping sectors.",
        why_matters="Premier sell-side shipping analyst. His move back to Clarksons signals convergence of broking, research, and capital markets in shipping.",
        location="New York, NY",
    ),

    # ── NOTABLE MENTIONS ──
    PersonProfile(
        name="Jacques Gabillon",
        role="Chairman",
        org="Vortexa",
        org_type="data_platform",
        education="Former Goldman Sachs",
        career_path=[
            "Global Head of Commodities Research at Goldman Sachs",
            "Built Goldman's commodity quantitative research practice",
            "Appointed Chairman of Vortexa",
        ],
        key_achievement="Invented the 'Gabillon model' for commodity futures pricing — standard in energy derivatives. Brought Goldman's quantitative rigor to Vortexa's board.",
        why_matters="When Goldman's former commodities chief chairs your board, it validates that cargo flow data is now core financial infrastructure.",
        location="London, UK",
    ),
    PersonProfile(
        name="Vinit Bansal",
        role="CTO & Co-Founder",
        org="PierSight Space",
        org_type="satellite",
        education="BITS Pilani",
        career_path=[
            "Systems prototyping at National Instruments — worked with ISRO and Indian defence ministry",
            "Aerospace/defense hardware engineering",
            "Met Gaurav Seth on ISRO project in 2019",
            "Sept 2023: Co-founded PierSight",
            "Built Varuna satellite hardware including indigenous reflectarray antenna",
        ],
        key_achievement="Engineered a SAR system in CubeSat form factor — dramatically reducing per-satellite cost for maritime surveillance.",
        why_matters="The hardware brain behind PierSight's cost disruption in the SAR market.",
        location="Ahmedabad, India",
    ),
]


def render_people():
    """Render the power players map to terminal."""
    print()
    print(f"  {BOLD}{'═' * 76}{RESET}")
    print(f"  {BOLD}POWER PLAYERS: KEY PEOPLE IN FREIGHT / SHIPPING INTELLIGENCE{RESET}")
    print(f"  {DIM}Founders, operators, and decision-makers shaping the maritime data ecosystem{RESET}")
    print(f"  {'━' * 76}")
    print()

    # Group by org_type
    type_labels = {
        "quant_fund": ("QUANT FUNDS & HEDGE FUNDS", CYAN),
        "data_platform": ("DATA PLATFORM FOUNDERS", GREEN),
        "satellite": ("SATELLITE INTELLIGENCE", MAGENTA),
        "benchmark": ("BENCHMARKS & INDICES", YELLOW),
        "research": ("RESEARCH & BROKING", YELLOW),
        "sell_side": ("SELL-SIDE ANALYSTS", DIM),
        "trading_house": ("PHYSICAL TRADING HOUSES", RED),
    }

    grouped = {}
    for p in KEY_PEOPLE:
        grouped.setdefault(p.org_type, []).append(p)

    display_order = ["quant_fund", "data_platform", "satellite", "benchmark", "research", "sell_side", "trading_house"]
    for otype in display_order:
        if otype not in grouped:
            continue
        label, color = type_labels.get(otype, (otype.upper(), ""))
        print(f"  {color}{BOLD}── {label} ──{RESET}")
        print()

        for p in grouped[otype]:
            print(f"  {color}{BOLD}{p.name}{RESET}")
            print(f"  {p.role} @ {p.org}")
            print(f"  {DIM}{p.location} │ {p.education[:70]}{RESET}")
            print()

            # Career path (condensed)
            for i, step in enumerate(p.career_path):
                connector = "├─" if i < len(p.career_path) - 1 else "└─"
                print(f"    {DIM}{connector}{RESET} {step[:85]}{'...' if len(step) > 85 else ''}")
            print()

            print(f"  {BOLD}Achievement:{RESET} {p.key_achievement[:120]}{'...' if len(p.key_achievement) > 120 else ''}")
            print(f"  {RED}Why it matters:{RESET} {p.why_matters[:120]}{'...' if len(p.why_matters) > 120 else ''}")
            print()

        print()

    # Ecosystem map
    print(f"  {BOLD}ECOSYSTEM MAP: INFORMATION FLOW{RESET}")
    print(f"  {'─' * 76}")
    print(f"""
  {DIM}Physical World{RESET}        {DIM}Data Layer{RESET}              {DIM}Intelligence{RESET}          {DIM}Capital{RESET}
  ┌──────────┐       ┌──────────────┐       ┌──────────────┐    ┌──────────┐
  │ Vessels  │──AIS──│ Kpler        │──API──│ Citadel      │───│ FFA/CME  │
  │ Ports    │──SAR──│ Vortexa      │──API──│ CargoMetrics │───│ SGX      │
  │ Cargoes  │──docs─│ Signal Ocean │──API──│ Breakwave    │───│ ICE      │
  │ Terminals│──flow─│ Oceanbolt    │──API──│ Trafigura    │───│ BDRY/ETF │
  └──────────┘       │ Ursa Space   │       │ Banks/Brokers│    └──────────┘
                     │ PierSight    │       └──────────────┘
                     └──────────────┘
                           │
                     ┌─────┴──────┐
                     │ Baltic Exch │ ← Benchmarks (BDI, BDTI, routes)
                     │ Clarksons   │ ← Historical data + research
                     └────────────┘{RESET}
  """)

    print(f"  {BOLD}KEY RELATIONSHIPS{RESET}")
    print(f"  {'─' * 76}")
    relationships = [
        ("Borgerson (CargoMetrics)", "Howard Morgan (RenTech co-founder)", "Investor — brought quant DNA"),
        ("Gabillon (ex-Goldman)", "Vortexa", "Chairman — validated cargo data as financial infra"),
        ("Martinos (Signal Ocean)", "Greek shipping families", "Physical fleet insight → data product"),
        ("Cazor/Maynier (Kpler)", "Insight Partners ($200M)", "Scaled to dominant commodity platform"),
        ("Kartsonas (Breakwave)", "Baltic Exchange", "Licensed indices for first freight ETFs"),
        ("Nokta (analyst)", "Clarksons Capital Markets", "Bridging research → capital markets"),
        ("Kuhn (Vortexa)", "BP Trading (5 yrs)", "Built product from inside a supermajor"),
        ("Seth/Bansal (PierSight)", "ISRO", "Gov't space program → commercial maritime SAR"),
    ]
    for person, connection, context in relationships:
        print(f"    {person:<35s} ↔ {connection:<30s} {DIM}{context}{RESET}")
    print()


if __name__ == "__main__":
    render()
    render_competitive_intel()
    render_people()
