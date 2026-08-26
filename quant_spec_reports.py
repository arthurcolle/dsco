"""Quantitative speculation report builders for weather markets.

The functions in this module are deterministic and input-driven. The default
sample data is synthetic and is intended for report layout, workflow testing,
and analyst ideation, not as live market data.
"""

from __future__ import annotations

import csv
import json
import math
import random
import re
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Sequence

from trading_ops import (
    MarketSnapshot,
    adaptive_order_timeout,
    bid_ask_depth_imbalance,
    book_shock_monitor,
    classify_spread_regime,
    cvar_empirical,
    kelly_fraction_cap,
    liquidity_score,
    no_trade_classifier,
    passive_fill_probability,
    portfolio_stress_test_scenarios,
    tail_risk_budget,
    trade_reason_code,
    var_gaussian,
    volatility_throttle,
)


DEFAULT_ASSUMPTIONS = {
    "data_status": "synthetic_sample",
    "unit": "binary contract dollars per $1 payout",
    "fee_model": "fee_rate * entry_price * (1 - entry_price)",
    "slippage_model": "half spread plus quantity/depth impact",
    "decision_use": "speculative research and workflow testing only",
}


@dataclass(frozen=True, slots=True)
class SpeculativeMarketCase:
    market: str
    city: str
    contract: str
    target_date: str
    fair_prob: float
    bid: float
    ask: float
    confidence: float
    regime: str
    sigma_f: float
    volume: float
    open_interest: float
    bid_depth: float
    ask_depth: float
    hours_to_settlement: float
    half_life_hours: float
    catalyst: str
    notes: str = ""

    @classmethod
    def from_mapping(cls, data: dict[str, Any]) -> "SpeculativeMarketCase":
        required = {
            "market",
            "city",
            "contract",
            "target_date",
            "fair_prob",
            "bid",
            "ask",
            "confidence",
            "regime",
            "sigma_f",
            "volume",
            "open_interest",
            "bid_depth",
            "ask_depth",
            "hours_to_settlement",
            "half_life_hours",
            "catalyst",
        }
        missing = sorted(required - set(data))
        if missing:
            raise ValueError(f"missing market case fields: {', '.join(missing)}")
        return cls(
            market=str(data["market"]),
            city=str(data["city"]),
            contract=str(data["contract"]),
            target_date=str(data["target_date"]),
            fair_prob=_clamp_prob(float(data["fair_prob"])),
            bid=_clamp_prob(float(data["bid"])),
            ask=_clamp_prob(float(data["ask"])),
            confidence=_clamp_prob(float(data["confidence"])),
            regime=str(data["regime"]),
            sigma_f=max(0.0, float(data["sigma_f"])),
            volume=max(0.0, float(data["volume"])),
            open_interest=max(0.0, float(data["open_interest"])),
            bid_depth=max(0.0, float(data["bid_depth"])),
            ask_depth=max(0.0, float(data["ask_depth"])),
            hours_to_settlement=max(0.0, float(data["hours_to_settlement"])),
            half_life_hours=max(0.1, float(data["half_life_hours"])),
            catalyst=str(data["catalyst"]),
            notes=str(data.get("notes", "")),
        )


def _clamp_prob(value: float) -> float:
    return max(0.0, min(1.0, value))


def _parse_as_of(value: datetime | str | None) -> datetime:
    if value is None:
        return datetime.now(timezone.utc)
    if isinstance(value, datetime):
        return value.astimezone(timezone.utc) if value.tzinfo else value.replace(tzinfo=timezone.utc)
    normalized = value.replace("Z", "+00:00")
    parsed = datetime.fromisoformat(normalized)
    return parsed.astimezone(timezone.utc) if parsed.tzinfo else parsed.replace(tzinfo=timezone.utc)


def _round(value: float, digits: int = 4) -> float:
    return round(float(value), digits)


def _side_direction(side: str) -> float:
    if side == "BUY_YES":
        return 1.0
    if side == "SELL_YES":
        return -1.0
    return 0.0


def _normal_cdf(value: float) -> float:
    return 0.5 * (1.0 + math.erf(value / math.sqrt(2.0)))


def _normal_pdf_scalar(value: float) -> float:
    return math.exp(-0.5 * value * value) / math.sqrt(2.0 * math.pi)


def _inverse_normal_cdf(p: float) -> float:
    """Acklam approximation for the inverse standard normal CDF."""
    p = min(1.0 - 1e-12, max(1e-12, p))
    a = [
        -3.969683028665376e01,
        2.209460984245205e02,
        -2.759285104469687e02,
        1.383577518672690e02,
        -3.066479806614716e01,
        2.506628277459239e00,
    ]
    b = [
        -5.447609879822406e01,
        1.615858368580409e02,
        -1.556989798598866e02,
        6.680131188771972e01,
        -1.328068155288572e01,
    ]
    c = [
        -7.784894002430293e-03,
        -3.223964580411365e-01,
        -2.400758277161838e00,
        -2.549732539343734e00,
        4.374664141464968e00,
        2.938163982698783e00,
    ]
    d = [
        7.784695709041462e-03,
        3.224671290700398e-01,
        2.445134137142996e00,
        3.754408661907416e00,
    ]
    plow = 0.02425
    phigh = 1.0 - plow
    if p < plow:
        q = math.sqrt(-2.0 * math.log(p))
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / (
            (((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0
        )
    if p > phigh:
        q = math.sqrt(-2.0 * math.log(1.0 - p))
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / (
            (((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0
        )
    q = p - 0.5
    r = q * q
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (
        ((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0
    )


def _percentile(values: Sequence[float], q: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(float(value) for value in values)
    idx = max(0.0, min(1.0, q)) * (len(ordered) - 1)
    lo = int(math.floor(idx))
    hi = int(math.ceil(idx))
    if lo == hi:
        return ordered[lo]
    frac = idx - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def _selected_edge_rows(edge_report: dict[str, Any], limit: int = 5) -> list[dict[str, Any]]:
    selected = [
        row for row in edge_report["rows"]
        if row["decision"] == "TRADE_CANDIDATE"
    ][:limit]
    if selected:
        return selected
    return edge_report["rows"][: min(3, len(edge_report["rows"]))]


def _contract_family(contract: str) -> str:
    lower = contract.lower()
    if "rain" in lower:
        return "rain"
    if "low" in lower:
        return "low_temp"
    if "high" in lower:
        return "high_temp"
    return "other"


def _infer_threshold(case: SpeculativeMarketCase) -> float | None:
    match = re.search(r"-T(\d+(?:\.\d+)?)", case.market)
    if match:
        return float(match.group(1))
    match = re.search(r"(?:above|below)\s+(\d+(?:\.\d+)?)F", case.contract)
    return float(match.group(1)) if match else None


def _case_lookup(cases: Sequence[SpeculativeMarketCase]) -> dict[str, SpeculativeMarketCase]:
    return {case.market: case for case in cases}


def sample_speculative_cases() -> list[SpeculativeMarketCase]:
    """Return deterministic sample cases for report demos and tests."""
    raw = [
        {
            "market": "KXHIGHNY-26APR11-T75",
            "city": "nyc",
            "contract": "NYC high above 75F",
            "target_date": "2026-04-11",
            "fair_prob": 0.64,
            "bid": 0.51,
            "ask": 0.53,
            "confidence": 0.72,
            "regime": "warm_advection",
            "sigma_f": 3.4,
            "volume": 2600,
            "open_interest": 840,
            "bid_depth": 410,
            "ask_depth": 380,
            "hours_to_settlement": 31,
            "half_life_hours": 18,
            "catalyst": "12z guidance warmed boundary layer by 1.8F",
            "notes": "Most sensitive to afternoon cloud cover.",
        },
        {
            "market": "KXHIGHCHI-26APR11-T68",
            "city": "chicago",
            "contract": "Chicago high above 68F",
            "target_date": "2026-04-11",
            "fair_prob": 0.31,
            "bid": 0.425,
            "ask": 0.44,
            "confidence": 0.69,
            "regime": "lake_breeze",
            "sigma_f": 4.6,
            "volume": 1780,
            "open_interest": 620,
            "bid_depth": 260,
            "ask_depth": 220,
            "hours_to_settlement": 33,
            "half_life_hours": 11,
            "catalyst": "Lake breeze setup compresses upper-tail outcomes",
            "notes": "Edge decays quickly if wind shifts offshore.",
        },
        {
            "market": "KXRAINSEA-26APR11",
            "city": "seattle",
            "contract": "Seattle measurable rain",
            "target_date": "2026-04-11",
            "fair_prob": 0.58,
            "bid": 0.47,
            "ask": 0.48,
            "confidence": 0.64,
            "regime": "marine_front",
            "sigma_f": 2.8,
            "volume": 1420,
            "open_interest": 480,
            "bid_depth": 190,
            "ask_depth": 205,
            "hours_to_settlement": 27,
            "half_life_hours": 9,
            "catalyst": "Shortwave timing favors rain before cutoff",
            "notes": "Settlement definition matters near trace threshold.",
        },
        {
            "market": "KXHIGHAUS-26APR12-T86",
            "city": "austin",
            "contract": "Austin high above 86F",
            "target_date": "2026-04-12",
            "fair_prob": 0.47,
            "bid": 0.52,
            "ask": 0.56,
            "confidence": 0.56,
            "regime": "dryline",
            "sigma_f": 5.8,
            "volume": 980,
            "open_interest": 360,
            "bid_depth": 150,
            "ask_depth": 170,
            "hours_to_settlement": 55,
            "half_life_hours": 16,
            "catalyst": "Dryline position is west of consensus track",
            "notes": "Useful watch, but distribution is wide.",
        },
        {
            "market": "KXLOWDEN-26APR12-T34",
            "city": "denver",
            "contract": "Denver low below 34F",
            "target_date": "2026-04-12",
            "fair_prob": 0.71,
            "bid": 0.62,
            "ask": 0.66,
            "confidence": 0.61,
            "regime": "radiational_cooling",
            "sigma_f": 4.1,
            "volume": 760,
            "open_interest": 540,
            "bid_depth": 120,
            "ask_depth": 110,
            "hours_to_settlement": 45,
            "half_life_hours": 14,
            "catalyst": "Clear-sky cooling risk above public forecast",
            "notes": "Snow cover quality controls downside tail.",
        },
        {
            "market": "KXRAINHOU-26APR11",
            "city": "houston",
            "contract": "Houston measurable rain",
            "target_date": "2026-04-11",
            "fair_prob": 0.28,
            "bid": 0.35,
            "ask": 0.40,
            "confidence": 0.58,
            "regime": "cap_strength",
            "sigma_f": 3.9,
            "volume": 690,
            "open_interest": 310,
            "bid_depth": 92,
            "ask_depth": 84,
            "hours_to_settlement": 24,
            "half_life_hours": 7,
            "catalyst": "Cap strength implies lower convective coverage",
            "notes": "Watch radar initiation before adding risk.",
        },
        {
            "market": "KXHIGHPHX-26APR12-T96",
            "city": "phoenix",
            "contract": "Phoenix high above 96F",
            "target_date": "2026-04-12",
            "fair_prob": 0.52,
            "bid": 0.48,
            "ask": 0.55,
            "confidence": 0.50,
            "regime": "ridge_edge",
            "sigma_f": 5.2,
            "volume": 530,
            "open_interest": 280,
            "bid_depth": 72,
            "ask_depth": 64,
            "hours_to_settlement": 57,
            "half_life_hours": 20,
            "catalyst": "Ridge axis timing is still unstable",
            "notes": "Spread is too wide for immediate action.",
        },
    ]
    return [SpeculativeMarketCase.from_mapping(item) for item in raw]


def load_cases(path: str | Path) -> list[SpeculativeMarketCase]:
    payload = json.loads(Path(path).read_text(encoding="utf-8"))
    rows = payload.get("cases", payload) if isinstance(payload, dict) else payload
    if not isinstance(rows, list):
        raise ValueError("case input must be a list or an object with a cases list")
    return [SpeculativeMarketCase.from_mapping(row) for row in rows]


def score_case(
    case: SpeculativeMarketCase,
    *,
    quantity: float = 25.0,
    fee_rate: float = 0.0125,
) -> dict[str, Any]:
    if case.ask < case.bid:
        raise ValueError(f"{case.market} ask is below bid")

    buy_edge = case.fair_prob - case.ask
    sell_edge = case.bid - case.fair_prob
    if buy_edge >= sell_edge and buy_edge > 0:
        side = "BUY_YES"
        entry = case.ask
        gross_edge = buy_edge
        depth = case.ask_depth
    elif sell_edge > 0:
        side = "SELL_YES"
        entry = case.bid
        gross_edge = sell_edge
        depth = case.bid_depth
    else:
        side = "HOLD"
        entry = (case.bid + case.ask) / 2.0
        gross_edge = max(buy_edge, sell_edge)
        depth = min(case.bid_depth, case.ask_depth)

    spread = case.ask - case.bid
    mid = (case.ask + case.bid) / 2.0
    spread_regime = classify_spread_regime(spread=spread, mid_price=mid)
    liq = liquidity_score(
        bid_size=case.bid_depth,
        ask_size=case.ask_depth,
        spread=spread,
        volume=case.volume,
        open_interest=case.open_interest,
    )
    depth = max(1.0, depth)
    fee = fee_rate * entry * (1.0 - entry)
    slippage = 0.5 * spread + min(0.04, quantity / depth * max(spread, 0.01))
    net_edge = gross_edge - fee - slippage
    tradeability = no_trade_classifier(
        edge=net_edge,
        confidence=case.confidence,
        liquidity=liq,
        spread_regime=spread_regime,
        min_edge=0.015,
    )
    reason = trade_reason_code(
        edge=net_edge,
        confidence=case.confidence,
        liquidity=liq,
        spread_regime=spread_regime,
    )
    if tradeability.tradeable and side != "HOLD":
        decision = "TRADE_CANDIDATE"
    elif gross_edge > 0:
        decision = "WATCH"
    else:
        decision = "NO_TRADE"

    decay = math.exp(-case.hours_to_settlement / case.half_life_hours)
    decayed_net_edge = net_edge * decay
    kelly = kelly_fraction_cap(edge=max(0.0, net_edge), confidence=case.confidence, max_fraction=0.18)
    score = 100.0 * net_edge * math.sqrt(max(0.0, case.confidence)) * (1.0 + min(1.0, liq / 8.0))
    return {
        "market": case.market,
        "city": case.city,
        "contract": case.contract,
        "target_date": case.target_date,
        "side": side,
        "decision": decision,
        "reason": reason if decision != "TRADE_CANDIDATE" else "EDGE_OK",
        "fair_prob": _round(case.fair_prob),
        "bid": _round(case.bid),
        "ask": _round(case.ask),
        "entry_price": _round(entry),
        "gross_edge": _round(gross_edge),
        "fee": _round(fee),
        "slippage": _round(slippage),
        "net_edge": _round(net_edge),
        "decayed_net_edge": _round(decayed_net_edge),
        "confidence": _round(case.confidence),
        "rank_score": _round(score, 3),
        "kelly_fraction": _round(kelly),
        "liquidity_score": _round(liq, 3),
        "spread": _round(spread),
        "spread_regime": spread_regime,
        "depth_imbalance": _round(bid_ask_depth_imbalance(case.bid_depth, case.ask_depth), 3),
        "sigma_f": _round(case.sigma_f, 2),
        "regime": case.regime,
        "hours_to_settlement": _round(case.hours_to_settlement, 1),
        "half_life_hours": _round(case.half_life_hours, 1),
        "catalyst": case.catalyst,
        "notes": case.notes,
    }


def build_edge_screener_report(
    cases: Sequence[SpeculativeMarketCase],
    *,
    as_of: datetime | str | None = None,
    quantity: float = 25.0,
    fee_rate: float = 0.0125,
) -> dict[str, Any]:
    rows = [
        score_case(case, quantity=quantity, fee_rate=fee_rate)
        for case in cases
    ]
    rows.sort(key=lambda row: (row["rank_score"], row["net_edge"]), reverse=True)
    candidates = [row for row in rows if row["decision"] == "TRADE_CANDIDATE"]
    positive = [row["net_edge"] for row in rows if row["net_edge"] > 0]
    return {
        "report": "edge_screener",
        "as_of": _parse_as_of(as_of).isoformat(),
        "assumptions": {**DEFAULT_ASSUMPTIONS, "quantity": quantity, "fee_rate": fee_rate},
        "summary": {
            "case_count": len(rows),
            "trade_candidates": len(candidates),
            "watch_count": sum(1 for row in rows if row["decision"] == "WATCH"),
            "best_market": rows[0]["market"] if rows else None,
            "best_net_edge": rows[0]["net_edge"] if rows else 0.0,
            "avg_positive_net_edge": _round(sum(positive) / len(positive), 4) if positive else 0.0,
        },
        "rows": rows,
    }


def build_scenario_ladder_report(
    cases: Sequence[SpeculativeMarketCase],
    *,
    as_of: datetime | str | None = None,
    quantity: float = 25.0,
    fee_rate: float = 0.0125,
) -> dict[str, Any]:
    edge = build_edge_screener_report(cases, as_of=as_of, quantity=quantity, fee_rate=fee_rate)
    selected = [
        row for row in edge["rows"]
        if row["decision"] == "TRADE_CANDIDATE"
    ][:5]
    if not selected:
        selected = edge["rows"][:3]
    scenarios = [
        ("model_revert", -0.070, -0.030, "late model cycle gives back the edge"),
        ("base_case", 0.000, 0.000, "current fair value holds"),
        ("trend_confirmation", 0.045, 0.025, "next cycle confirms signal direction"),
        ("settlement_squeeze", 0.085, 0.055, "thin book chases the tail outcome"),
        ("liquidity_air_pocket", -0.035, -0.075, "spread widens while fair value softens"),
    ]
    rows = []
    for scenario, fair_shift, mark_shift, description in scenarios:
        total_pnl = 0.0
        worst_leg = None
        for row in selected:
            direction = 1.0 if row["side"] == "BUY_YES" else -1.0
            scenario_fair = _clamp_prob(row["fair_prob"] + direction * fair_shift)
            scenario_mark = _clamp_prob(row["entry_price"] + direction * mark_shift)
            expected_pnl = quantity * direction * (scenario_fair - row["entry_price"] - row["slippage"] - row["fee"])
            mark_to_market = quantity * direction * (scenario_mark - row["entry_price"])
            total_pnl += expected_pnl
            leg = {
                "scenario": scenario,
                "market": row["market"],
                "side": row["side"],
                "scenario_fair": _round(scenario_fair),
                "scenario_mark": _round(scenario_mark),
                "expected_pnl": _round(expected_pnl, 2),
                "mark_to_market": _round(mark_to_market, 2),
            }
            if worst_leg is None or leg["expected_pnl"] < worst_leg["expected_pnl"]:
                worst_leg = leg
        rows.append(
            {
                "scenario": scenario,
                "description": description,
                "selected_markets": len(selected),
                "portfolio_expected_pnl": _round(total_pnl, 2),
                "worst_leg_market": worst_leg["market"] if worst_leg else None,
                "worst_leg_pnl": worst_leg["expected_pnl"] if worst_leg else 0.0,
            }
        )
    return {
        "report": "scenario_ladder",
        "as_of": edge["as_of"],
        "assumptions": edge["assumptions"],
        "selected_markets": selected,
        "rows": rows,
    }


def build_liquidity_execution_report(
    cases: Sequence[SpeculativeMarketCase],
    *,
    as_of: datetime | str | None = None,
    quantity: float = 25.0,
    fee_rate: float = 0.0125,
) -> dict[str, Any]:
    scored = build_edge_screener_report(cases, as_of=as_of, quantity=quantity, fee_rate=fee_rate)
    median_spread = _median([row["spread"] for row in scored["rows"]]) or 0.04
    rows = []
    for case, score in zip(cases, [score_case(item, quantity=quantity, fee_rate=fee_rate) for item in cases]):
        spread = case.ask - case.bid
        snapshot = MarketSnapshot(
            case.market,
            _parse_as_of(as_of),
            bid=case.bid,
            ask=case.ask,
            bid_size=case.bid_depth,
            ask_size=case.ask_depth,
            bid_depth=case.bid_depth,
            ask_depth=case.ask_depth,
            volume=case.volume,
            open_interest=case.open_interest,
        )
        liq = liquidity_score(
            bid_size=case.bid_depth,
            ask_size=case.ask_depth,
            spread=spread,
            volume=case.volume,
            open_interest=case.open_interest,
        )
        volatility = max(0.25, case.sigma_f / 4.0)
        timeout = adaptive_order_timeout(
            spread_regime=score["spread_regime"],
            liquidity=liq,
            volatility=volatility,
            base_timeout_seconds=28.0,
        )
        arrival_rate = max(0.001, case.volume / (6.5 * 3600.0))
        cancel_rate = max(0.0005, case.open_interest / (48.0 * 3600.0))
        queue_ahead = max(1.0, 0.35 * (case.ask_depth if score["side"] == "BUY_YES" else case.bid_depth))
        fill_prob = passive_fill_probability(
            queue_ahead=queue_ahead,
            arrival_rate=arrival_rate,
            cancel_rate=cancel_rate,
            timeout_seconds=timeout,
        )
        shock = book_shock_monitor(
            current_spread=spread,
            median_spread=median_spread,
            depth_imbalance=bid_ask_depth_imbalance(case.bid_depth, case.ask_depth),
        )
        throttle = volatility_throttle(
            volatility=volatility,
            spread=spread,
            baseline_volatility=1.0,
            baseline_spread=median_spread,
        )
        rows.append(
            {
                "market": case.market,
                "side": score["side"],
                "decision": score["decision"],
                "mid_price": _round(snapshot.mid_price or 0.0),
                "spread": _round(spread),
                "spread_regime": score["spread_regime"],
                "liquidity_score": _round(liq, 3),
                "depth_imbalance": _round(bid_ask_depth_imbalance(case.bid_depth, case.ask_depth), 3),
                "passive_fill_probability": _round(fill_prob),
                "suggested_timeout_seconds": _round(timeout, 1),
                "volatility_throttle": _round(throttle),
                "book_shock_triggered": bool(shock["triggered"]),
                "tail_risk_budget": _round(tail_risk_budget(liquidity_score_value=liq, base_budget=quantity)),
            }
        )
    rows.sort(key=lambda row: (row["decision"] != "TRADE_CANDIDATE", -row["passive_fill_probability"], row["spread"]))
    return {
        "report": "liquidity_execution",
        "as_of": scored["as_of"],
        "assumptions": scored["assumptions"],
        "summary": {
            "median_spread": _round(median_spread),
            "shock_count": sum(1 for row in rows if row["book_shock_triggered"]),
            "avg_fill_probability": _round(sum(row["passive_fill_probability"] for row in rows) / len(rows), 4) if rows else 0.0,
        },
        "rows": rows,
    }


def build_tail_risk_report(
    cases: Sequence[SpeculativeMarketCase],
    *,
    as_of: datetime | str | None = None,
    quantity: float = 25.0,
    fee_rate: float = 0.0125,
) -> dict[str, Any]:
    edge = build_edge_screener_report(cases, as_of=as_of, quantity=quantity, fee_rate=fee_rate)
    selected = [row for row in edge["rows"] if row["decision"] == "TRADE_CANDIDATE"][:5]
    if not selected:
        selected = edge["rows"][:3]
    rows = []
    pnl_values = []
    scenarios = portfolio_stress_test_scenarios()
    scenarios.append(
        type(scenarios[0])(
            "model_break",
            price_shock=-0.12,
            volatility_multiplier=2.2,
            correlation_multiplier=1.5,
            liquidity_multiplier=0.25,
        )
    )
    for scenario in scenarios:
        total = 0.0
        for row in selected:
            shock = -abs(scenario.price_shock) * scenario.correlation_multiplier
            liquidity_penalty = row["slippage"] * (1.0 / max(0.1, scenario.liquidity_multiplier) - 1.0)
            leg_pnl = quantity * (shock + row["net_edge"] - liquidity_penalty)
            total += leg_pnl
        pnl_values.append(total)
        rows.append(
            {
                "scenario": scenario.name,
                "price_shock": _round(scenario.price_shock),
                "volatility_multiplier": _round(scenario.volatility_multiplier),
                "correlation_multiplier": _round(scenario.correlation_multiplier),
                "liquidity_multiplier": _round(scenario.liquidity_multiplier),
                "portfolio_pnl": _round(total, 2),
            }
        )

    returns = [value / max(1.0, quantity * max(1, len(selected))) for value in pnl_values]
    worst = min(rows, key=lambda row: row["portfolio_pnl"]) if rows else None
    return {
        "report": "tail_risk",
        "as_of": edge["as_of"],
        "assumptions": edge["assumptions"],
        "selected_markets": selected,
        "summary": {
            "selected_count": len(selected),
            "worst_scenario": worst["scenario"] if worst else None,
            "worst_pnl": worst["portfolio_pnl"] if worst else 0.0,
            "gaussian_var_95": _round(var_gaussian(returns, confidence=0.95), 4),
            "empirical_cvar_95": _round(cvar_empirical(returns, confidence=0.95), 4),
        },
        "rows": rows,
    }


def build_signal_decay_report(
    cases: Sequence[SpeculativeMarketCase],
    *,
    as_of: datetime | str | None = None,
    quantity: float = 25.0,
    fee_rate: float = 0.0125,
) -> dict[str, Any]:
    edge = build_edge_screener_report(cases, as_of=as_of, quantity=quantity, fee_rate=fee_rate)
    rows = []
    for row in edge["rows"]:
        half_life = max(0.1, row["half_life_hours"])
        refresh_in = max(0.5, half_life * math.log(2.0))
        sigma_penalty = min(0.07, row["sigma_f"] / 100.0)
        refresh_edge = row["net_edge"] * math.exp(-refresh_in / half_life) - sigma_penalty
        if row["decision"] == "TRADE_CANDIDATE" and refresh_edge <= 0:
            action = "REFRESH_BEFORE_RISK"
        elif row["decision"] == "WATCH" and row["gross_edge"] > 0:
            action = "WAIT_FOR_CONFIRMATION"
        elif row["decision"] == "NO_TRADE":
            action = "IGNORE_UNTIL_REPRICE"
        else:
            action = "MONITOR"
        rows.append(
            {
                "market": row["market"],
                "decision": row["decision"],
                "action": action,
                "net_edge": row["net_edge"],
                "decayed_net_edge": row["decayed_net_edge"],
                "edge_after_refresh_window": _round(refresh_edge),
                "refresh_within_hours": _round(refresh_in, 1),
                "hours_to_settlement": row["hours_to_settlement"],
                "half_life_hours": row["half_life_hours"],
                "regime": row["regime"],
                "catalyst": row["catalyst"],
            }
        )
    rows.sort(key=lambda row: (row["action"] != "REFRESH_BEFORE_RISK", row["refresh_within_hours"]))
    return {
        "report": "signal_decay",
        "as_of": edge["as_of"],
        "assumptions": edge["assumptions"],
        "summary": {
            "refresh_before_risk": sum(1 for row in rows if row["action"] == "REFRESH_BEFORE_RISK"),
            "wait_for_confirmation": sum(1 for row in rows if row["action"] == "WAIT_FOR_CONFIRMATION"),
        },
        "rows": rows,
    }


def build_monte_carlo_report(
    cases: Sequence[SpeculativeMarketCase],
    *,
    as_of: datetime | str | None = None,
    quantity: float = 25.0,
    fee_rate: float = 0.0125,
    iterations: int = 5000,
    seed: int = 20260410,
) -> dict[str, Any]:
    edge = build_edge_screener_report(cases, as_of=as_of, quantity=quantity, fee_rate=fee_rate)
    selected = _selected_edge_rows(edge)
    rng = random.Random(seed)
    portfolio_pnls: list[float] = []
    leg_pnls: dict[str, list[float]] = {row["market"]: [] for row in selected}

    iterations = max(250, int(iterations))
    for _ in range(iterations):
        common_weather = rng.gauss(0.0, 1.0)
        liquidity_stress = max(0.0, rng.gauss(0.0, 1.0))
        regime_shocks = {row["regime"]: rng.gauss(0.0, 1.0) for row in selected}
        total = 0.0
        for row in selected:
            direction = _side_direction(row["side"])
            if direction == 0.0:
                continue
            uncertainty = 0.015 + 0.085 * (1.0 - row["confidence"]) + min(0.05, row["sigma_f"] / 140.0)
            fair_shift = 0.018 * common_weather + 0.026 * regime_shocks[row["regime"]] + uncertainty * rng.gauss(0.0, 1.0)
            realized_prob = _clamp_prob(row["fair_prob"] + fair_shift)
            yes_settles = rng.random() < realized_prob
            settlement = 1.0 if yes_settles else 0.0
            stressed_slippage = row["slippage"] * (1.0 + 0.65 * liquidity_stress)
            if row["side"] == "BUY_YES":
                pnl = quantity * (settlement - row["entry_price"] - row["fee"] - stressed_slippage)
            else:
                pnl = quantity * (row["entry_price"] - settlement - row["fee"] - stressed_slippage)
            total += pnl
            leg_pnls[row["market"]].append(pnl)
        portfolio_pnls.append(total)

    mean_pnl = sum(portfolio_pnls) / len(portfolio_pnls) if portfolio_pnls else 0.0
    loss_rows = [value for value in portfolio_pnls if value < 0]
    tail_cut = _percentile(portfolio_pnls, 0.05)
    tail = [value for value in portfolio_pnls if value <= tail_cut]
    distribution = [
        {"quantile": "p01", "portfolio_pnl": _round(_percentile(portfolio_pnls, 0.01), 2)},
        {"quantile": "p05", "portfolio_pnl": _round(tail_cut, 2)},
        {"quantile": "p25", "portfolio_pnl": _round(_percentile(portfolio_pnls, 0.25), 2)},
        {"quantile": "p50", "portfolio_pnl": _round(_percentile(portfolio_pnls, 0.50), 2)},
        {"quantile": "p75", "portfolio_pnl": _round(_percentile(portfolio_pnls, 0.75), 2)},
        {"quantile": "p95", "portfolio_pnl": _round(_percentile(portfolio_pnls, 0.95), 2)},
        {"quantile": "p99", "portfolio_pnl": _round(_percentile(portfolio_pnls, 0.99), 2)},
    ]
    rows = []
    for row in selected:
        values = leg_pnls[row["market"]]
        rows.append(
            {
                "market": row["market"],
                "side": row["side"],
                "decision": row["decision"],
                "mean_pnl": _round(sum(values) / len(values), 2) if values else 0.0,
                "p05_pnl": _round(_percentile(values, 0.05), 2),
                "p95_pnl": _round(_percentile(values, 0.95), 2),
                "loss_probability": _round(sum(1 for value in values if value < 0) / len(values), 4) if values else 0.0,
                "net_edge": row["net_edge"],
                "fair_prob": row["fair_prob"],
                "entry_price": row["entry_price"],
            }
        )
    rows.sort(key=lambda row: row["mean_pnl"], reverse=True)
    return {
        "report": "monte_carlo",
        "as_of": edge["as_of"],
        "assumptions": {
            **edge["assumptions"],
            "iterations": iterations,
            "seed": seed,
            "simulation": "binary settlement with common weather, regime, idiosyncratic, and liquidity shocks",
        },
        "selected_markets": selected,
        "summary": {
            "iterations": iterations,
            "selected_count": len(selected),
            "mean_pnl": _round(mean_pnl, 2),
            "median_pnl": _round(_percentile(portfolio_pnls, 0.50), 2),
            "p05_pnl": _round(tail_cut, 2),
            "p95_pnl": _round(_percentile(portfolio_pnls, 0.95), 2),
            "loss_probability": _round(len(loss_rows) / len(portfolio_pnls), 4) if portfolio_pnls else 0.0,
            "expected_shortfall_5": _round(sum(tail) / len(tail), 2) if tail else 0.0,
        },
        "distribution": distribution,
        "histogram": _histogram(portfolio_pnls, bins=24),
        "rows": rows,
    }


def build_correlation_allocation_report(
    cases: Sequence[SpeculativeMarketCase],
    *,
    as_of: datetime | str | None = None,
    quantity: float = 25.0,
    fee_rate: float = 0.0125,
    risk_budget: float | None = None,
) -> dict[str, Any]:
    edge = build_edge_screener_report(cases, as_of=as_of, quantity=quantity, fee_rate=fee_rate)
    rows = [row for row in edge["rows"] if row["net_edge"] > 0][:7]
    cases_by_market = _case_lookup(cases)
    if risk_budget is None:
        risk_budget = max(quantity, quantity * max(1, len(_selected_edge_rows(edge))) * 0.85)

    matrix: dict[str, dict[str, float]] = {row["market"]: {} for row in rows}
    for left in rows:
        left_case = cases_by_market[left["market"]]
        for right in rows:
            right_case = cases_by_market[right["market"]]
            if left["market"] == right["market"]:
                corr = 1.0
            else:
                corr = 0.08
                if left_case.target_date == right_case.target_date:
                    corr += 0.12
                if left_case.city == right_case.city:
                    corr += 0.35
                if left_case.regime == right_case.regime:
                    corr += 0.25
                if _contract_family(left_case.contract) == _contract_family(right_case.contract):
                    corr += 0.12
                corr *= _side_direction(left["side"]) * _side_direction(right["side"])
                corr = max(-0.85, min(0.85, corr))
            matrix[left["market"]][right["market"]] = _round(corr, 4)

    raw_scores = {}
    risk_units = {}
    for row in rows:
        market = row["market"]
        corr_penalty = sum(abs(matrix[market][other["market"]]) for other in rows if other["market"] != market)
        variance_proxy = max(0.02, math.sqrt(row["fair_prob"] * (1.0 - row["fair_prob"])))
        liquidity_bonus = min(1.5, max(0.25, row["liquidity_score"] / 5.0))
        risk_units[market] = variance_proxy * (1.0 + row["spread"] * 5.0 + corr_penalty / max(1, len(rows) - 1))
        raw_scores[market] = max(0.0, row["net_edge"]) * row["confidence"] * liquidity_bonus / max(0.05, risk_units[market])

    total_score = sum(raw_scores.values()) or 1.0
    allocations = {
        market: risk_budget * score / total_score
        for market, score in raw_scores.items()
    }
    report_rows = []
    for row in rows:
        market = row["market"]
        corr_penalty = sum(abs(matrix[market][other["market"]]) for other in rows if other["market"] != market)
        contracts = allocations[market]
        report_rows.append(
            {
                "market": market,
                "side": row["side"],
                "decision": row["decision"],
                "allocation_contracts": _round(contracts, 2),
                "expected_pnl": _round(contracts * row["net_edge"], 2),
                "risk_unit": _round(risk_units[market], 4),
                "corr_penalty": _round(corr_penalty, 4),
                "net_edge": row["net_edge"],
                "confidence": row["confidence"],
                "liquidity_score": row["liquidity_score"],
            }
        )
    report_rows.sort(key=lambda row: row["allocation_contracts"], reverse=True)
    weights = [row["allocation_contracts"] for row in report_rows]
    total_weight = sum(weights) or 1.0
    concentration = sum((weight / total_weight) ** 2 for weight in weights)
    weighted_corr = 0.0
    for left in report_rows:
        for right in report_rows:
            weighted_corr += (
                left["allocation_contracts"] / total_weight
                * right["allocation_contracts"] / total_weight
                * matrix[left["market"]][right["market"]]
            )
    return {
        "report": "correlation_allocation",
        "as_of": edge["as_of"],
        "assumptions": {**edge["assumptions"], "risk_budget_contracts": _round(risk_budget, 2)},
        "summary": {
            "selected_count": len(report_rows),
            "risk_budget_contracts": _round(risk_budget, 2),
            "effective_bets": _round(1.0 / max(1e-9, concentration), 2),
            "weighted_correlation": _round(weighted_corr, 4),
            "largest_allocation_market": report_rows[0]["market"] if report_rows else None,
        },
        "correlation_matrix": matrix,
        "rows": report_rows,
    }


def build_market_disagreement_report(
    cases: Sequence[SpeculativeMarketCase],
    *,
    as_of: datetime | str | None = None,
    quantity: float = 25.0,
    fee_rate: float = 0.0125,
) -> dict[str, Any]:
    edge = build_edge_screener_report(cases, as_of=as_of, quantity=quantity, fee_rate=fee_rate)
    cases_by_market = _case_lookup(cases)
    rows = []
    for row in edge["rows"]:
        case = cases_by_market[row["market"]]
        mid = (case.bid + case.ask) / 2.0
        disagreement = case.fair_prob - mid
        liq_weight = min(0.88, max(0.10, row["liquidity_score"] / 12.0))
        market_noise = max(0.012, row["spread"] * 0.75 + 1.0 / math.sqrt(max(1.0, case.volume + case.open_interest)))
        model_noise = max(0.015, 0.12 * (1.0 - case.confidence) + min(0.06, case.sigma_f / 150.0))
        model_precision = 1.0 / (model_noise * model_noise)
        market_precision = liq_weight / (market_noise * market_noise)
        posterior = (model_precision * case.fair_prob + market_precision * mid) / (model_precision + market_precision)
        z_score = disagreement / math.sqrt(model_noise * model_noise + market_noise * market_noise)
        if abs(z_score) >= 1.5 and row["net_edge"] > 0:
            read = "MODEL_DISLOCATION"
        elif abs(z_score) >= 1.0:
            read = "MARKET_DISAGREES"
        elif row["spread_regime"] in {"wide", "stressed"}:
            read = "PRICE_DISCOVERY_WEAK"
        else:
            read = "CONSENSUS_ALIGNED"
        rows.append(
            {
                "market": row["market"],
                "contract": row["contract"],
                "model_fair": _round(case.fair_prob),
                "market_mid": _round(mid),
                "posterior_prob": _round(posterior),
                "disagreement": _round(disagreement),
                "z_score": _round(z_score, 3),
                "read": read,
                "net_edge": row["net_edge"],
                "spread_regime": row["spread_regime"],
                "liquidity_score": row["liquidity_score"],
            }
        )
    rows.sort(key=lambda row: abs(row["z_score"]), reverse=True)
    strongest = rows[0] if rows else None
    return {
        "report": "market_disagreement",
        "as_of": edge["as_of"],
        "assumptions": {
            **edge["assumptions"],
            "posterior": "precision-weighted blend of model fair and market midpoint",
        },
        "summary": {
            "strongest_market": strongest["market"] if strongest else None,
            "strongest_z_score": strongest["z_score"] if strongest else 0.0,
            "dislocation_count": sum(1 for row in rows if row["read"] == "MODEL_DISLOCATION"),
            "avg_abs_z": _round(sum(abs(row["z_score"]) for row in rows) / len(rows), 3) if rows else 0.0,
        },
        "rows": rows,
    }


def build_weather_sensitivity_report(
    cases: Sequence[SpeculativeMarketCase],
    *,
    as_of: datetime | str | None = None,
    quantity: float = 25.0,
    fee_rate: float = 0.0125,
) -> dict[str, Any]:
    edge = build_edge_screener_report(cases, as_of=as_of, quantity=quantity, fee_rate=fee_rate)
    cases_by_market = _case_lookup(cases)
    rows = []
    for row in edge["rows"]:
        case = cases_by_market[row["market"]]
        threshold = _infer_threshold(case)
        family = _contract_family(case.contract)
        sigma = max(0.75, case.sigma_f)
        inferred_mean = None
        delta_per_1f = 0.0
        pin_risk = 0.0
        if threshold is not None and family in {"high_temp", "low_temp"}:
            if family == "high_temp":
                z = _inverse_normal_cdf(1.0 - case.fair_prob)
                inferred_mean = threshold - sigma * z
                shifted_prob = 1.0 - _normal_cdf((threshold - 1.0 - inferred_mean) / sigma)
                pin_risk = _normal_cdf((threshold + 1.0 - inferred_mean) / sigma) - _normal_cdf((threshold - 1.0 - inferred_mean) / sigma)
                local_delta = _normal_pdf_scalar((threshold - inferred_mean) / sigma) / sigma
            else:
                z = _inverse_normal_cdf(case.fair_prob)
                inferred_mean = threshold - sigma * z
                shifted_prob = _normal_cdf((threshold - inferred_mean - 1.0) / sigma)
                pin_risk = _normal_cdf((threshold + 1.0 - inferred_mean) / sigma) - _normal_cdf((threshold - 1.0 - inferred_mean) / sigma)
                local_delta = -_normal_pdf_scalar((threshold - inferred_mean) / sigma) / sigma
            delta_per_1f = shifted_prob - case.fair_prob
        else:
            local_delta = case.fair_prob * (1.0 - case.fair_prob) / max(1.0, sigma)
            delta_per_1f = local_delta
            pin_risk = 0.0

        market_delta_value = quantity * _side_direction(row["side"]) * delta_per_1f
        rows.append(
            {
                "market": row["market"],
                "family": family,
                "threshold": _round(threshold, 2) if threshold is not None else None,
                "inferred_mean_f": _round(inferred_mean, 2) if inferred_mean is not None else None,
                "sigma_f": row["sigma_f"],
                "fair_prob": row["fair_prob"],
                "delta_per_1f": _round(delta_per_1f),
                "local_probability_delta": _round(local_delta),
                "pin_risk_2f": _round(pin_risk),
                "market_delta_value": _round(market_delta_value, 2),
                "decision": row["decision"],
                "side": row["side"],
                "regime": row["regime"],
            }
        )
    rows.sort(key=lambda row: (row["pin_risk_2f"], abs(row["market_delta_value"])), reverse=True)
    highest = rows[0] if rows else None
    return {
        "report": "weather_sensitivity",
        "as_of": edge["as_of"],
        "assumptions": {
            **edge["assumptions"],
            "temperature_model": "normal approximation inferred from fair probability and threshold",
        },
        "summary": {
            "highest_pin_market": highest["market"] if highest else None,
            "highest_pin_risk_2f": highest["pin_risk_2f"] if highest else 0.0,
            "largest_abs_delta_market": max(rows, key=lambda row: abs(row["market_delta_value"]))["market"] if rows else None,
        },
        "rows": rows,
    }


def build_quant_spec_bundle(
    cases: Sequence[SpeculativeMarketCase],
    *,
    as_of: datetime | str | None = None,
    quantity: float = 25.0,
    fee_rate: float = 0.0125,
) -> dict[str, Any]:
    parsed_as_of = _parse_as_of(as_of)
    kwargs = {"as_of": parsed_as_of, "quantity": quantity, "fee_rate": fee_rate}
    return {
        "as_of": parsed_as_of.isoformat(),
        "case_count": len(cases),
        "cases": cases_to_jsonable(cases),
        "reports": {
            "edge_screener": build_edge_screener_report(cases, **kwargs),
            "scenario_ladder": build_scenario_ladder_report(cases, **kwargs),
            "liquidity_execution": build_liquidity_execution_report(cases, **kwargs),
            "tail_risk": build_tail_risk_report(cases, **kwargs),
            "signal_decay": build_signal_decay_report(cases, **kwargs),
            "monte_carlo": build_monte_carlo_report(cases, **kwargs),
            "correlation_allocation": build_correlation_allocation_report(cases, **kwargs),
            "market_disagreement": build_market_disagreement_report(cases, **kwargs),
            "weather_sensitivity": build_weather_sensitivity_report(cases, **kwargs),
        },
    }


def write_quant_spec_reports(
    bundle: dict[str, Any],
    out_dir: str | Path,
) -> dict[str, Any]:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    created: list[str] = []

    report_files = {
        "edge_screener": ("edge_screener.md", render_edge_screener_md),
        "scenario_ladder": ("scenario_ladder.md", render_scenario_ladder_md),
        "liquidity_execution": ("liquidity_execution.md", render_liquidity_execution_md),
        "tail_risk": ("tail_risk.md", render_tail_risk_md),
        "signal_decay": ("signal_decay.md", render_signal_decay_md),
        "monte_carlo": ("monte_carlo.md", render_monte_carlo_md),
        "correlation_allocation": ("correlation_allocation.md", render_correlation_allocation_md),
        "market_disagreement": ("market_disagreement.md", render_market_disagreement_md),
        "weather_sensitivity": ("weather_sensitivity.md", render_weather_sensitivity_md),
    }
    for key, (filename, renderer) in report_files.items():
        path = out / filename
        path.write_text(renderer(bundle["reports"][key]), encoding="utf-8")
        created.append(filename)

    csv_files = {
        "edge_screener": "edge_screener.csv",
        "scenario_ladder": "scenario_ladder.csv",
        "liquidity_execution": "liquidity_execution.csv",
        "tail_risk": "tail_risk.csv",
        "signal_decay": "signal_decay.csv",
        "monte_carlo": "monte_carlo.csv",
        "correlation_allocation": "correlation_allocation.csv",
        "market_disagreement": "market_disagreement.csv",
        "weather_sensitivity": "weather_sensitivity.csv",
    }
    for key, filename in csv_files.items():
        rows = bundle["reports"][key]["rows"]
        _write_csv(out / filename, rows)
        created.append(filename)

    bundle_path = out / "bundle.json"
    bundle_path.write_text(json.dumps(bundle, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    created.append("bundle.json")

    if "cases" in bundle:
        cases_path = out / "cases.json"
        cases_path.write_text(json.dumps({"cases": bundle["cases"]}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        created.append("cases.json")

    index_path = out / "index.md"
    index_path.write_text(render_index_md(bundle), encoding="utf-8")
    created.append("index.md")

    manifest = {
        "created": sorted([*created, "manifest.json"]),
        "as_of": bundle["as_of"],
        "case_count": bundle["case_count"],
        "note": "Speculative quantitative research reports. Synthetic sample data unless a case input file was supplied.",
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    created.append("manifest.json")
    return manifest


def _write_csv(path: Path, rows: Sequence[dict[str, Any]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames = sorted({key for row in rows for key in row})
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def _markdown_table(headers: Sequence[str], rows: Iterable[Sequence[Any]]) -> str:
    header = "| " + " | ".join(headers) + " |"
    sep = "| " + " | ".join("---" for _ in headers) + " |"
    body = []
    for row in rows:
        values = [str(value).replace("|", "/") for value in row]
        body.append("| " + " | ".join(values) + " |")
    return "\n".join([header, sep, *body])


def _fmt_pct(value: float) -> str:
    return f"{100.0 * float(value):.1f}%"


def _fmt_dollars(value: float) -> str:
    return f"${float(value):.2f}"


def _front_matter(report: dict[str, Any], title: str) -> str:
    return (
        f"# {title}\n\n"
        f"As of: `{report['as_of']}`\n\n"
        "Data status: synthetic sample unless regenerated with `--input`.\n\n"
    )


def render_edge_screener_md(report: dict[str, Any]) -> str:
    rows = report["rows"]
    summary = report["summary"]
    table = _markdown_table(
        ["Rank", "Market", "Side", "Decision", "Fair", "Entry", "Net Edge", "Kelly", "Reason"],
        [
            [
                idx + 1,
                row["market"],
                row["side"],
                row["decision"],
                _fmt_pct(row["fair_prob"]),
                _fmt_pct(row["entry_price"]),
                _fmt_pct(row["net_edge"]),
                _fmt_pct(row["kelly_fraction"]),
                row["reason"],
            ]
            for idx, row in enumerate(rows)
        ],
    )
    return (
        _front_matter(report, "Quant Spec Edge Screener")
        + f"- Cases: `{summary['case_count']}`\n"
        + f"- Trade candidates: `{summary['trade_candidates']}`\n"
        + f"- Best market: `{summary['best_market']}` with `{_fmt_pct(summary['best_net_edge'])}` net edge\n\n"
        + table
        + "\n"
    )


def render_scenario_ladder_md(report: dict[str, Any]) -> str:
    table = _markdown_table(
        ["Scenario", "Description", "Selected", "Expected PnL", "Worst Leg", "Worst Leg PnL"],
        [
            [
                row["scenario"],
                row["description"],
                row["selected_markets"],
                _fmt_dollars(row["portfolio_expected_pnl"]),
                row["worst_leg_market"],
                _fmt_dollars(row["worst_leg_pnl"]),
            ]
            for row in report["rows"]
        ],
    )
    selected = ", ".join(row["market"] for row in report["selected_markets"]) or "none"
    return _front_matter(report, "Scenario Ladder") + f"Selected markets: `{selected}`\n\n" + table + "\n"


def render_liquidity_execution_md(report: dict[str, Any]) -> str:
    summary = report["summary"]
    table = _markdown_table(
        ["Market", "Decision", "Spread", "Regime", "Fill Prob", "Timeout", "Throttle", "Shock"],
        [
            [
                row["market"],
                row["decision"],
                _fmt_pct(row["spread"]),
                row["spread_regime"],
                _fmt_pct(row["passive_fill_probability"]),
                f"{row['suggested_timeout_seconds']}s",
                _fmt_pct(row["volatility_throttle"]),
                row["book_shock_triggered"],
            ]
            for row in report["rows"]
        ],
    )
    return (
        _front_matter(report, "Liquidity And Execution Report")
        + f"- Median spread: `{_fmt_pct(summary['median_spread'])}`\n"
        + f"- Book shock count: `{summary['shock_count']}`\n"
        + f"- Average passive fill probability: `{_fmt_pct(summary['avg_fill_probability'])}`\n\n"
        + table
        + "\n"
    )


def render_tail_risk_md(report: dict[str, Any]) -> str:
    summary = report["summary"]
    table = _markdown_table(
        ["Scenario", "Price Shock", "Vol Mult", "Corr Mult", "Liq Mult", "Portfolio PnL"],
        [
            [
                row["scenario"],
                _fmt_pct(row["price_shock"]),
                row["volatility_multiplier"],
                row["correlation_multiplier"],
                row["liquidity_multiplier"],
                _fmt_dollars(row["portfolio_pnl"]),
            ]
            for row in report["rows"]
        ],
    )
    return (
        _front_matter(report, "Tail Risk Stress Report")
        + f"- Worst scenario: `{summary['worst_scenario']}` / `{_fmt_dollars(summary['worst_pnl'])}`\n"
        + f"- Gaussian VaR 95: `{_fmt_pct(summary['gaussian_var_95'])}`\n"
        + f"- Empirical CVaR 95: `{_fmt_pct(summary['empirical_cvar_95'])}`\n\n"
        + table
        + "\n"
    )


def render_signal_decay_md(report: dict[str, Any]) -> str:
    summary = report["summary"]
    table = _markdown_table(
        ["Market", "Decision", "Action", "Net Edge", "Decayed Edge", "Refresh Hrs", "Regime"],
        [
            [
                row["market"],
                row["decision"],
                row["action"],
                _fmt_pct(row["net_edge"]),
                _fmt_pct(row["decayed_net_edge"]),
                row["refresh_within_hours"],
                row["regime"],
            ]
            for row in report["rows"]
        ],
    )
    return (
        _front_matter(report, "Signal Decay And Refresh Report")
        + f"- Refresh before risk: `{summary['refresh_before_risk']}`\n"
        + f"- Wait for confirmation: `{summary['wait_for_confirmation']}`\n\n"
        + table
        + "\n"
    )


def render_monte_carlo_md(report: dict[str, Any]) -> str:
    summary = report["summary"]
    dist_table = _markdown_table(
        ["Quantile", "Portfolio PnL"],
        [[row["quantile"], _fmt_dollars(row["portfolio_pnl"])] for row in report["distribution"]],
    )
    leg_table = _markdown_table(
        ["Market", "Side", "Mean PnL", "P05", "P95", "Loss Prob"],
        [
            [
                row["market"],
                row["side"],
                _fmt_dollars(row["mean_pnl"]),
                _fmt_dollars(row["p05_pnl"]),
                _fmt_dollars(row["p95_pnl"]),
                _fmt_pct(row["loss_probability"]),
            ]
            for row in report["rows"]
        ],
    )
    return (
        _front_matter(report, "Monte Carlo PnL Report")
        + f"- Iterations: `{summary['iterations']}`\n"
        + f"- Mean PnL: `{_fmt_dollars(summary['mean_pnl'])}`\n"
        + f"- Loss probability: `{_fmt_pct(summary['loss_probability'])}`\n"
        + f"- Expected shortfall 5: `{_fmt_dollars(summary['expected_shortfall_5'])}`\n\n"
        + "## Portfolio Distribution\n\n"
        + dist_table
        + "\n\n## Leg Contributions\n\n"
        + leg_table
        + "\n"
    )


def render_correlation_allocation_md(report: dict[str, Any]) -> str:
    summary = report["summary"]
    table = _markdown_table(
        ["Market", "Side", "Allocation", "Expected PnL", "Risk Unit", "Corr Penalty"],
        [
            [
                row["market"],
                row["side"],
                row["allocation_contracts"],
                _fmt_dollars(row["expected_pnl"]),
                row["risk_unit"],
                row["corr_penalty"],
            ]
            for row in report["rows"]
        ],
    )
    return (
        _front_matter(report, "Correlation-Aware Allocation Report")
        + f"- Risk budget: `{summary['risk_budget_contracts']}` contracts\n"
        + f"- Effective bets: `{summary['effective_bets']}`\n"
        + f"- Weighted correlation: `{summary['weighted_correlation']}`\n"
        + f"- Largest allocation: `{summary['largest_allocation_market']}`\n\n"
        + table
        + "\n"
    )


def render_market_disagreement_md(report: dict[str, Any]) -> str:
    summary = report["summary"]
    table = _markdown_table(
        ["Market", "Model", "Mid", "Posterior", "Disagreement", "Z", "Read"],
        [
            [
                row["market"],
                _fmt_pct(row["model_fair"]),
                _fmt_pct(row["market_mid"]),
                _fmt_pct(row["posterior_prob"]),
                _fmt_pct(row["disagreement"]),
                row["z_score"],
                row["read"],
            ]
            for row in report["rows"]
        ],
    )
    return (
        _front_matter(report, "Model-Vs-Market Disagreement Report")
        + f"- Strongest market: `{summary['strongest_market']}`\n"
        + f"- Strongest z-score: `{summary['strongest_z_score']}`\n"
        + f"- Model dislocations: `{summary['dislocation_count']}`\n"
        + f"- Average abs z-score: `{summary['avg_abs_z']}`\n\n"
        + table
        + "\n"
    )


def render_weather_sensitivity_md(report: dict[str, Any]) -> str:
    summary = report["summary"]
    table = _markdown_table(
        ["Market", "Family", "Threshold", "Mean", "Delta/1F", "Pin Risk", "Dollar Delta"],
        [
            [
                row["market"],
                row["family"],
                row["threshold"],
                row["inferred_mean_f"],
                _fmt_pct(row["delta_per_1f"]),
                _fmt_pct(row["pin_risk_2f"]),
                _fmt_dollars(row["market_delta_value"]),
            ]
            for row in report["rows"]
        ],
    )
    return (
        _front_matter(report, "Weather Threshold Sensitivity Report")
        + f"- Highest pin-risk market: `{summary['highest_pin_market']}`\n"
        + f"- Highest 2F pin risk: `{_fmt_pct(summary['highest_pin_risk_2f'])}`\n"
        + f"- Largest absolute dollar delta: `{summary['largest_abs_delta_market']}`\n\n"
        + table
        + "\n"
    )


def render_index_md(bundle: dict[str, Any]) -> str:
    edge = bundle["reports"]["edge_screener"]
    risk = bundle["reports"]["tail_risk"]
    liquidity = bundle["reports"]["liquidity_execution"]
    monte = bundle["reports"]["monte_carlo"]
    allocation = bundle["reports"]["correlation_allocation"]
    disagreement = bundle["reports"]["market_disagreement"]
    return (
        "# Quantitative Speculation Reports\n\n"
        f"As of: `{bundle['as_of']}`\n\n"
        "These reports are deterministic research artifacts. They are useful for ranking possible weather-market ideas, "
        "examining scenario sensitivity, and testing reporting workflows. They are not live market data and are not trading advice.\n\n"
        "## Reports\n\n"
        "- [Edge screener](edge_screener.md)\n"
        "- [Scenario ladder](scenario_ladder.md)\n"
        "- [Liquidity and execution](liquidity_execution.md)\n"
        "- [Tail risk stress](tail_risk.md)\n"
        "- [Signal decay and refresh](signal_decay.md)\n"
        "- [Monte Carlo PnL](monte_carlo.md)\n"
        "- [Correlation-aware allocation](correlation_allocation.md)\n"
        "- [Model-vs-market disagreement](market_disagreement.md)\n"
        "- [Weather threshold sensitivity](weather_sensitivity.md)\n\n"
        "Regenerate with real cases using:\n\n"
        "```bash\n"
        "python3 scripts/generate_quant_spec_reports.py --input artifacts/reports/quant_speculation/cases.json\n"
        "```\n\n"
        "## Snapshot\n\n"
        f"- Cases: `{bundle['case_count']}`\n"
        f"- Trade candidates: `{edge['summary']['trade_candidates']}`\n"
        f"- Best market: `{edge['summary']['best_market']}`\n"
        f"- Worst stress scenario: `{risk['summary']['worst_scenario']}` / `{_fmt_dollars(risk['summary']['worst_pnl'])}`\n"
        f"- Average passive fill probability: `{_fmt_pct(liquidity['summary']['avg_fill_probability'])}`\n"
        f"- Monte Carlo mean PnL: `{_fmt_dollars(monte['summary']['mean_pnl'])}` / loss probability `{_fmt_pct(monte['summary']['loss_probability'])}`\n"
        f"- Effective correlation-adjusted bets: `{allocation['summary']['effective_bets']}`\n"
        f"- Strongest model-market disagreement: `{disagreement['summary']['strongest_market']}`\n"
    )


def _median(values: Sequence[float]) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    mid = len(ordered) // 2
    if len(ordered) % 2:
        return float(ordered[mid])
    return (float(ordered[mid - 1]) + float(ordered[mid])) / 2.0


def _histogram(values: Sequence[float], *, bins: int = 20) -> list[dict[str, Any]]:
    if not values:
        return []
    bins = max(3, int(bins))
    lo = min(values)
    hi = max(values)
    if math.isclose(lo, hi):
        return [{"lo": _round(lo, 2), "hi": _round(hi, 2), "count": len(values)}]
    width = (hi - lo) / bins
    counts = [0 for _ in range(bins)]
    for value in values:
        idx = min(bins - 1, int((value - lo) / width))
        counts[idx] += 1
    return [
        {
            "lo": _round(lo + idx * width, 2),
            "hi": _round(lo + (idx + 1) * width, 2),
            "mid": _round(lo + (idx + 0.5) * width, 2),
            "count": count,
        }
        for idx, count in enumerate(counts)
    ]


def cases_to_jsonable(cases: Sequence[SpeculativeMarketCase]) -> list[dict[str, Any]]:
    return [asdict(case) for case in cases]
