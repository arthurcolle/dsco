"""Trading and risk helpers for live forecast execution.

This module stays self-contained and pure-Python so it can be unit tested
without external services. It covers lightweight market snapshot history,
execution guardrails, trade rationale codes, and portfolio risk helpers.
"""

from __future__ import annotations

import hashlib
import math
import statistics
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from typing import Any, Sequence


def _utc(dt: datetime | None) -> datetime:
    if dt is None:
        return datetime.now(timezone.utc)
    if dt.tzinfo is None:
        return dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc)


def _clamp(value: float, lo: float = 0.0, hi: float = 1.0) -> float:
    return max(lo, min(hi, value))


def _mean(values: Sequence[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def _pstdev(values: Sequence[float]) -> float:
    if len(values) < 2:
        return 0.0
    return statistics.pstdev(values)


def _percentile(values: Sequence[float], q: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return float(values[0])
    ordered = sorted(float(v) for v in values)
    idx = q * (len(ordered) - 1)
    lo = int(math.floor(idx))
    hi = int(math.ceil(idx))
    if lo == hi:
        return ordered[lo]
    frac = idx - lo
    return ordered[lo] * (1 - frac) + ordered[hi] * frac


@dataclass(slots=True)
class MarketSnapshot:
    market: str
    captured_at: datetime
    bid: float | None = None
    ask: float | None = None
    bid_size: float = 0.0
    ask_size: float = 0.0
    bid_depth: float = 0.0
    ask_depth: float = 0.0
    volume: float = 0.0
    open_interest: float = 0.0
    metadata: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        self.captured_at = _utc(self.captured_at)

    @property
    def spread(self) -> float | None:
        if self.bid is None or self.ask is None:
            return None
        return max(0.0, float(self.ask) - float(self.bid))

    @property
    def mid_price(self) -> float | None:
        if self.bid is None and self.ask is None:
            return None
        if self.bid is None:
            return float(self.ask)
        if self.ask is None:
            return float(self.bid)
        return (float(self.bid) + float(self.ask)) / 2.0

    @property
    def spread_bps(self) -> float | None:
        mid = self.mid_price
        spread = self.spread
        if mid is None or spread is None or mid <= 0:
            return None
        return 10000.0 * spread / mid


@dataclass(slots=True)
class OrderEvent:
    order_id: str
    market: str
    side: str
    quantity: float
    price: float
    created_at: datetime
    updated_at: datetime | None = None
    status: str = "open"
    strategy: str = "default"
    metadata: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        self.created_at = _utc(self.created_at)
        self.updated_at = _utc(self.updated_at or self.created_at)

    @property
    def age_seconds(self) -> float:
        return max(0.0, (_utc(None) - self.updated_at).total_seconds())


@dataclass(slots=True)
class FillEvent:
    market: str
    side: str
    quantity: float
    price: float
    mid_after: float | None = None
    realized_slippage: float | None = None
    filled_at: datetime = field(default_factory=lambda: datetime.now(timezone.utc))
    metadata: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        self.filled_at = _utc(self.filled_at)


@dataclass(slots=True)
class RiskLimits:
    max_daily_loss: float = 0.0
    max_market_loss: float = 0.0
    max_position_size: float = 0.0
    max_trade_count_per_hour: int = 0
    max_concentration: float = 1.0
    max_drawdown: float = 0.0


@dataclass(slots=True)
class NoTradeDecision:
    tradeable: bool
    reason: str
    edge: float
    confidence: float
    liquidity_score: float
    spread_regime: str


@dataclass(slots=True)
class SafeModeState:
    active: bool
    reason: str
    failure_count: int
    recover_after: datetime | None = None

    def __post_init__(self) -> None:
        if self.recover_after is not None:
            self.recover_after = _utc(self.recover_after)


@dataclass(slots=True)
class StressScenario:
    name: str
    price_shock: float = 0.0
    volatility_multiplier: float = 1.0
    correlation_multiplier: float = 1.0
    liquidity_multiplier: float = 1.0


def add_market_snapshot(
    history: list[MarketSnapshot],
    snapshot: MarketSnapshot,
    *,
    maxlen: int = 256,
) -> list[MarketSnapshot]:
    history.append(snapshot)
    history.sort(key=lambda item: item.captured_at)
    if maxlen and len(history) > maxlen:
        del history[:-maxlen]
    return history


def latest_mid_price(history: Sequence[MarketSnapshot]) -> float | None:
    if not history:
        return None
    latest = max(history, key=lambda item: item.captured_at)
    return latest.mid_price


def snapshot_history_window(
    history: Sequence[MarketSnapshot],
    *,
    since: datetime,
) -> list[MarketSnapshot]:
    cutoff = _utc(since)
    return [item for item in history if item.captured_at >= cutoff]


def bid_ask_depth_imbalance(bid_depth: float, ask_depth: float) -> float:
    total = float(bid_depth) + float(ask_depth)
    if total <= 0:
        return 0.0
    return (float(bid_depth) - float(ask_depth)) / total


def classify_spread_regime(
    *,
    spread: float,
    mid_price: float | None = None,
) -> str:
    if spread <= 0:
        return "locked"
    if mid_price is None or mid_price <= 0:
        ratio = spread
    else:
        ratio = spread / mid_price
    if ratio <= 0.01:
        return "tight"
    if ratio <= 0.04:
        return "normal"
    if ratio <= 0.08:
        return "wide"
    return "stressed"


def liquidity_score(
    *,
    bid_size: float,
    ask_size: float,
    spread: float,
    volume: float = 0.0,
    open_interest: float = 0.0,
) -> float:
    size = math.log1p(max(0.0, bid_size) + max(0.0, ask_size))
    activity = math.log1p(max(0.0, volume) + max(0.0, open_interest))
    penalty = 1.0 + max(0.0, spread)
    return max(0.0, (size + 0.5 * activity) / penalty)


def estimate_slippage(
    *,
    side: str,
    quantity: float,
    book: MarketSnapshot,
) -> float:
    base_spread = book.spread or 0.0
    depth = max(1.0, (book.bid_depth if side.lower() == "buy" else book.ask_depth) or 0.0)
    side = side.lower()
    signed = 1.0 if side == "buy" else -1.0
    impact = base_spread / 2.0 + quantity / depth * max(base_spread, 0.05)
    return max(0.0, signed * impact)


def passive_fill_probability(
    *,
    queue_ahead: float,
    arrival_rate: float,
    cancel_rate: float,
    timeout_seconds: float,
) -> float:
    effective_rate = max(0.0, arrival_rate + cancel_rate)
    if timeout_seconds <= 0 or effective_rate <= 0:
        return 0.0
    progress = effective_rate * timeout_seconds / max(1.0, queue_ahead + 1.0)
    return _clamp(1.0 - math.exp(-progress))


def estimate_queue_position(
    *,
    queue_ahead: float,
    our_size: float,
    depth: float,
    cancel_rate: float = 0.0,
) -> float:
    denominator = max(1.0, depth + cancel_rate)
    return _clamp((queue_ahead + 0.5 * our_size) / denominator, 0.0, 1.0)


def volatility_throttle(
    *,
    volatility: float,
    spread: float,
    baseline_volatility: float = 1.0,
    baseline_spread: float = 0.02,
) -> float:
    vol_ratio = volatility / max(1e-9, baseline_volatility)
    spread_ratio = spread / max(1e-9, baseline_spread)
    score = 1.0 / (1.0 + max(0.0, vol_ratio - 1.0) + max(0.0, spread_ratio - 1.0))
    return _clamp(score)


def adaptive_order_timeout(
    *,
    spread_regime: str,
    liquidity: float,
    volatility: float,
    base_timeout_seconds: float = 30.0,
) -> float:
    regime_mult = {
        "locked": 0.5,
        "tight": 0.8,
        "normal": 1.0,
        "wide": 1.4,
        "stressed": 2.0,
    }.get(spread_regime, 1.0)
    liquidity_mult = 1.2 - _clamp(liquidity / 10.0, 0.0, 0.9)
    volatility_mult = 1.0 + _clamp(volatility / 5.0, 0.0, 2.0)
    return max(5.0, base_timeout_seconds * regime_mult * liquidity_mult * volatility_mult)


def cancel_replace_guardrail(
    *,
    recent_updates: int,
    window_seconds: float,
    max_updates: int = 3,
    max_window_seconds: float = 30.0,
) -> bool:
    return recent_updates > max_updates or window_seconds > max_window_seconds


def max_order_frequency_cap(
    *,
    ticker: str,
    recent_order_count: int,
    limit_per_window: int,
) -> bool:
    _ = ticker
    return recent_order_count >= limit_per_window


def select_stale_orders(
    orders: Sequence[OrderEvent],
    *,
    now: datetime | None = None,
    max_age_seconds: float = 60.0,
) -> list[OrderEvent]:
    current = _utc(now)
    stale = []
    for order in orders:
        age = (current - order.updated_at).total_seconds()
        if order.status == "open" and age >= max_age_seconds:
            stale.append(order)
    return stale


def trade_reason_code(
    *,
    edge: float,
    confidence: float,
    liquidity: float,
    spread_regime: str,
) -> str:
    if edge <= 0:
        return "NO_EDGE"
    if confidence < 0.35:
        return "LOW_CONFIDENCE"
    if liquidity < 1.0:
        return "LOW_LIQUIDITY"
    if spread_regime in {"wide", "stressed"}:
        return "WIDE_MARKET"
    if edge >= 0.08 and liquidity >= 3.0:
        return "HIGH_EDGE"
    return "STANDARD_EDGE"


def expected_value_at_fill(
    *,
    edge: float,
    market_price: float,
    slippage: float = 0.0,
    fees: float = 0.0,
) -> float:
    return edge - market_price - slippage - fees


def realized_vs_expected_slippage(
    fills: Sequence[FillEvent],
) -> dict[str, float]:
    if not fills:
        return {"count": 0, "expected": 0.0, "realized": 0.0, "delta": 0.0}
    expected = [float(f.metadata.get("expected_slippage", 0.0)) for f in fills]
    realized = [
        float(f.realized_slippage if f.realized_slippage is not None else 0.0)
        for f in fills
    ]
    exp = _mean(expected)
    rea = _mean(realized)
    return {
        "count": len(fills),
        "expected": exp,
        "realized": rea,
        "delta": rea - exp,
    }


def apply_dead_zone(edge: float, *, threshold: float = 0.02) -> float:
    return 0.0 if abs(edge) < threshold else edge


def edge_persistence_filter(
    edges: Sequence[float],
    *,
    threshold: float = 0.02,
    confirmations: int = 2,
) -> dict[str, Any]:
    recent = [edge for edge in edges if abs(edge) >= threshold]
    positive = [edge for edge in recent if edge > 0]
    passed = len(positive) >= confirmations
    return {
        "passed": passed,
        "confirmations": len(positive),
        "recent_edge_mean": _mean(list(edges)) if edges else 0.0,
    }


def book_shock_monitor(
    *,
    current_spread: float,
    median_spread: float,
    depth_imbalance: float,
) -> dict[str, Any]:
    spread_ratio = current_spread / max(1e-9, median_spread)
    triggered = spread_ratio >= 2.0 or abs(depth_imbalance) >= 0.65
    return {
        "triggered": triggered,
        "spread_ratio": spread_ratio,
        "depth_imbalance": depth_imbalance,
    }


def adverse_selection_monitor(
    fills: Sequence[FillEvent],
    *,
    post_fill_moves: Sequence[float],
) -> dict[str, Any]:
    if not fills:
        return {"rate": 0.0, "count": 0, "adverse_count": 0}
    adverse = 0
    for fill, move in zip(fills, post_fill_moves):
        side = fill.side.lower()
        if (side == "buy" and move < 0) or (side == "sell" and move > 0):
            adverse += 1
    rate = adverse / len(fills)
    return {"rate": rate, "count": len(fills), "adverse_count": adverse}


def duplicate_order_key(
    *,
    strategy: str,
    market: str,
    side: str,
    bucket: str,
    signal_time: datetime,
) -> str:
    payload = "|".join(
        [strategy, market, side.lower(), bucket, _utc(signal_time).isoformat()]
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def idempotency_key(
    *,
    market: str,
    side: str,
    quantity: float,
    price: float,
    business_key: str,
) -> str:
    payload = f"{market}|{side.lower()}|{quantity:.6f}|{price:.6f}|{business_key}"
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def halt_safe_mode_state(
    *,
    failure_count: int,
    threshold: int = 3,
    cooldown_seconds: int = 300,
    now: datetime | None = None,
    reason: str = "repeated_failures",
) -> SafeModeState:
    active = failure_count >= threshold
    recover_after = _utc(now) + timedelta(seconds=cooldown_seconds) if active else None
    return SafeModeState(active=active, reason=reason, failure_count=failure_count, recover_after=recover_after)


def no_trade_classifier(
    *,
    edge: float,
    confidence: float,
    liquidity: float,
    spread_regime: str,
    min_edge: float = 0.03,
) -> NoTradeDecision:
    if abs(edge) < min_edge:
        return NoTradeDecision(False, "NO_EDGE", edge, confidence, liquidity, spread_regime)
    if confidence < 0.4:
        return NoTradeDecision(False, "LOW_CONFIDENCE", edge, confidence, liquidity, spread_regime)
    if liquidity < 1.0:
        return NoTradeDecision(False, "LOW_LIQUIDITY", edge, confidence, liquidity, spread_regime)
    if spread_regime in {"wide", "stressed"}:
        return NoTradeDecision(False, "WIDE_MARKET", edge, confidence, liquidity, spread_regime)
    return NoTradeDecision(True, "TRADEABLE", edge, confidence, liquidity, spread_regime)


def heartbeat_payload(
    *,
    service: str,
    healthy: bool,
    queue_lag_seconds: float = 0.0,
    latency_ms: float = 0.0,
    details: dict[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "service": service,
        "healthy": healthy,
        "queue_lag_seconds": queue_lag_seconds,
        "latency_ms": latency_ms,
        "details": details or {},
        "timestamp_utc": _utc(None).isoformat(),
    }


def fee_aware_expected_return(
    *,
    edge: float,
    fees: float,
    slippage: float,
) -> float:
    return edge - fees - slippage


def var_gaussian(
    returns: Sequence[float],
    *,
    confidence: float = 0.95,
) -> float:
    if not returns:
        return 0.0
    mean = _mean(list(returns))
    std = _pstdev(list(returns))
    z = {
        0.90: 1.281551565545,
        0.95: 1.644853626951,
        0.975: 1.95996398454,
        0.99: 2.32634787404,
    }.get(round(confidence, 3), 1.644853626951)
    return max(0.0, -(mean - z * std))


def cvar_empirical(
    returns: Sequence[float],
    *,
    confidence: float = 0.95,
) -> float:
    if not returns:
        return 0.0
    cutoff = _percentile(list(returns), 1.0 - confidence)
    tail = [value for value in returns if value <= cutoff]
    if not tail:
        return 0.0
    return max(0.0, -_mean(tail))


def cluster_correlation_matrix(
    series_by_cluster: dict[str, Sequence[float]],
) -> dict[str, dict[str, float]]:
    clusters = list(series_by_cluster)
    matrix: dict[str, dict[str, float]] = {cluster: {} for cluster in clusters}
    for left in clusters:
        left_series = list(series_by_cluster[left])
        for right in clusters:
            right_series = list(series_by_cluster[right])
            n = min(len(left_series), len(right_series))
            if n < 2:
                corr = 0.0
            else:
                xs = left_series[:n]
                ys = right_series[:n]
                xm = _mean(xs)
                ym = _mean(ys)
                cov = sum((x - xm) * (y - ym) for x, y in zip(xs, ys)) / n
                xs_std = _pstdev(xs)
                ys_std = _pstdev(ys)
                corr = cov / (xs_std * ys_std) if xs_std and ys_std else 0.0
            matrix[left][right] = max(-1.0, min(1.0, corr))
    return matrix


def effective_independence_allocation(
    *,
    total_capital: float,
    signals: Sequence[dict[str, Any]],
    correlation_matrix: dict[str, dict[str, float]] | None = None,
) -> dict[str, float]:
    if not signals:
        return {}
    weights = {str(signal["market"]): float(signal.get("confidence", 1.0)) for signal in signals}
    if correlation_matrix:
        denom = 0.0
        names = list(weights)
        for left in names:
            for right in names:
                corr = correlation_matrix.get(left, {}).get(right, 0.0)
                denom += weights[left] * weights[right] * max(0.0, 1.0 + corr)
        n_eff = (sum(weights.values()) ** 2) / max(1e-9, denom)
    else:
        n_eff = float(len(weights))
    n_eff = max(1.0, n_eff)
    per_unit = total_capital / n_eff
    return {name: per_unit * weight / max(1e-9, sum(weights.values())) for name, weight in weights.items()}


def drawdown_limit_trigger(
    *,
    peak_equity: float,
    current_equity: float,
    max_drawdown: float,
) -> bool:
    if peak_equity <= 0:
        return False
    drawdown = (peak_equity - current_equity) / peak_equity
    return drawdown >= max_drawdown


def market_loss_hard_stop(
    *,
    realized_pnl: float,
    limit: float,
) -> bool:
    return realized_pnl <= -abs(limit)


def daily_loss_circuit_breaker(
    *,
    realized_pnl: float,
    threshold: float,
) -> bool:
    return realized_pnl <= -abs(threshold)


def trade_count_throttle(
    *,
    trades_in_hour: int,
    limit: int,
) -> bool:
    return trades_in_hour >= limit


def concentration_cap(
    *,
    exposure: float,
    portfolio_value: float,
    max_concentration: float,
) -> bool:
    if portfolio_value <= 0:
        return True
    return abs(exposure) / portfolio_value >= max_concentration


def tail_risk_budget(
    *,
    liquidity_score_value: float,
    base_budget: float,
) -> float:
    return base_budget * (0.5 + 0.5 * _clamp(liquidity_score_value / 5.0))


def kelly_fraction_cap(
    *,
    edge: float,
    confidence: float,
    max_fraction: float = 0.25,
) -> float:
    raw = max(0.0, edge * confidence)
    return min(max_fraction, raw)


def fractional_kelly_decay(
    *,
    current_fraction: float,
    drift_score: float,
    floor: float = 0.25,
) -> float:
    decay = 1.0 - _clamp(drift_score, 0.0, 1.0) * 0.5
    return max(floor * current_fraction, current_fraction * decay)


def min_ev_threshold(
    *,
    edge: float,
    fees: float,
    slippage: float,
    threshold: float = 0.01,
) -> bool:
    return fee_aware_expected_return(edge=edge, fees=fees, slippage=slippage) >= threshold


def post_trade_risk_rebalance(
    *,
    current_exposure: float,
    pnl: float,
    target_exposure: float,
) -> float:
    return target_exposure - current_exposure + 0.5 * pnl


def overnight_gap_risk_reduction(
    *,
    exposure: float,
    gap_risk_multiplier: float,
) -> float:
    return exposure * max(0.0, 1.0 - _clamp(gap_risk_multiplier, 0.0, 1.0))


def event_risk_blackout(
    *,
    minutes_to_event: float,
    blackout_window_minutes: float = 30.0,
) -> bool:
    return minutes_to_event <= blackout_window_minutes


def low_confidence_size_reduction(
    *,
    size: float,
    confidence: float,
) -> float:
    return size * (0.5 + 0.5 * _clamp(confidence))


def high_volatility_size_reduction(
    *,
    size: float,
    volatility: float,
    baseline_volatility: float = 1.0,
) -> float:
    ratio = volatility / max(1e-9, baseline_volatility)
    return size / max(1.0, ratio)


def strategy_pnl_stop(
    *,
    strategy_pnl: float,
    stop_loss: float,
) -> bool:
    return strategy_pnl <= -abs(stop_loss)


def strategy_cooldown_after_loss_streak(
    *,
    loss_streak: int,
    cooldown_threshold: int = 3,
) -> bool:
    return loss_streak >= cooldown_threshold


def portfolio_stress_test_scenarios() -> list[StressScenario]:
    return [
        StressScenario("baseline"),
        StressScenario("spread_widening", price_shock=-0.01, volatility_multiplier=1.2, liquidity_multiplier=0.7),
        StressScenario("risk_off", price_shock=-0.03, volatility_multiplier=1.5, correlation_multiplier=1.2, liquidity_multiplier=0.5),
        StressScenario("liquidity_crunch", price_shock=-0.02, volatility_multiplier=1.8, correlation_multiplier=1.3, liquidity_multiplier=0.35),
    ]


def what_if_spread_widening(
    *,
    edge: float,
    current_spread: float,
    widened_spread: float,
) -> dict[str, float]:
    delta = max(0.0, widened_spread - current_spread)
    return {
        "edge_after_widening": edge - delta,
        "spread_delta": delta,
    }


def order_overlap_checker(
    *,
    target_market: str,
    correlated_markets: Sequence[str],
    active_orders: Sequence[OrderEvent],
    correlation_threshold: float = 0.7,
    market_correlations: dict[str, float] | None = None,
) -> dict[str, Any]:
    active = []
    for order in active_orders:
        if order.market == target_market:
            active.append(order.order_id)
            continue
        if order.market in correlated_markets:
            corr = 1.0 if market_correlations is None else market_correlations.get(order.market, 1.0)
            if corr >= correlation_threshold:
                active.append(order.order_id)
    return {"blocked": bool(active), "conflicting_order_ids": active}


def risk_policy_report(
    *,
    orders: Sequence[OrderEvent],
    fills: Sequence[FillEvent],
    limits: RiskLimits,
    current_equity: float,
    peak_equity: float,
    realized_pnl: float,
) -> dict[str, Any]:
    stale = select_stale_orders(orders, max_age_seconds=60.0)
    heartbeat = heartbeat_payload(service="risk-ops", healthy=not stale, queue_lag_seconds=float(len(stale)))
    return {
        "stale_orders": len(stale),
        "heartbeat": heartbeat,
        "daily_loss_breached": daily_loss_circuit_breaker(realized_pnl=realized_pnl, threshold=limits.max_daily_loss),
        "market_loss_breached": market_loss_hard_stop(realized_pnl=realized_pnl, limit=limits.max_market_loss),
        "drawdown_breached": drawdown_limit_trigger(
            peak_equity=peak_equity,
            current_equity=current_equity,
            max_drawdown=limits.max_drawdown,
        ),
        "fill_count": len(fills),
    }


def compliance_report(
    *,
    orders: Sequence[OrderEvent],
    fills: Sequence[FillEvent],
    limits: RiskLimits,
    now: datetime | None = None,
) -> dict[str, Any]:
    total_orders = len(orders)
    total_fills = len(fills)
    open_orders = sum(1 for order in orders if order.status == "open")
    return {
        "total_orders": total_orders,
        "open_orders": open_orders,
        "fill_count": total_fills,
        "max_trade_count_per_hour_breached": trade_count_throttle(
            trades_in_hour=total_orders,
            limit=limits.max_trade_count_per_hour,
        ),
        "max_position_size_breached": any(
            concentration_cap(
                exposure=order.quantity * order.price,
                portfolio_value=max(1.0, sum(fill.quantity * fill.price for fill in fills) or 1.0),
                max_concentration=limits.max_concentration,
            )
            for order in orders
        ),
        "stale_orders": len(select_stale_orders(orders, now=now)),
    }


def calibration_confidence_breakdown(
    *,
    data_confidence: float,
    model_confidence: float,
    regime_confidence: float,
    spread_confidence: float,
) -> dict[str, float]:
    weights = {
        "data": data_confidence,
        "model": model_confidence,
        "regime": regime_confidence,
        "spread": spread_confidence,
    }
    total = sum(weights.values()) or 1.0
    return {name: value / total for name, value in weights.items()}
