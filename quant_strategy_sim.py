"""High-volume strategy simulations with weather overlay factors."""

from __future__ import annotations

import csv
import json
import math
import time
from pathlib import Path
from typing import Any, Sequence

import numpy as np

from quant_spec_reports import (
    SpeculativeMarketCase,
    build_quant_spec_bundle,
    load_cases,
    sample_speculative_cases,
)


HORIZONS_MINUTES = [15, 30, 60, 120, 180, 360]
WEATHER_FACTORS = ["temp_shift_f", "precip_signal", "wind_signal", "cloud_signal"]
STRATEGIES = [
    "edge_static",
    "posterior_blend",
    "weather_momentum",
    "threshold_pin",
    "liquidity_reversion",
    "correlation_weighted",
    "ensemble_vote",
]


def run_strategy_simulation(
    cases: Sequence[SpeculativeMarketCase],
    *,
    as_of: str = "2026-04-10T12:00:00Z",
    paths_per_horizon: int = 10_000_000,
    chunk_size: int = 250_000,
    quantity: float = 25.0,
    fee_rate: float = 0.0125,
    seed: int = 20260410,
) -> dict[str, Any]:
    """Run chunked strategy simulations across all forecast horizons.

    `paths_per_horizon` paths are evaluated at each horizon, and every strategy
    is scored on the same simulated weather/price paths.
    """
    if not cases:
        raise ValueError("at least one case is required")
    paths_per_horizon = max(1, int(paths_per_horizon))
    chunk_size = max(1, int(chunk_size))
    started = time.perf_counter()

    base = _prepare_base_inputs(cases, as_of=as_of, quantity=quantity, fee_rate=fee_rate)
    n_h = len(HORIZONS_MINUTES)
    n_s = len(STRATEGIES)
    n_c = len(cases)
    n_f = len(WEATHER_FACTORS)
    bins = np.linspace(-320.0, 320.0, 641)

    sums = np.zeros((n_h, n_s), dtype=np.float64)
    sums_sq = np.zeros((n_h, n_s), dtype=np.float64)
    losses = np.zeros((n_h, n_s), dtype=np.int64)
    active_counts = np.zeros((n_h, n_s), dtype=np.float64)
    hist = np.zeros((n_h, n_s, len(bins) - 1), dtype=np.int64)
    market_sums = np.zeros((n_h, n_s, n_c), dtype=np.float64)
    factor_sums = np.zeros((n_h, n_f), dtype=np.float64)
    factor_sums_sq = np.zeros((n_h, n_f), dtype=np.float64)
    factor_pnl_sums = np.zeros((n_h, n_s, n_f), dtype=np.float64)

    rng = np.random.default_rng(seed)
    for h_idx, horizon in enumerate(HORIZONS_MINUTES):
        done = 0
        while done < paths_per_horizon:
            n = min(chunk_size, paths_per_horizon - done)
            overlay = _weather_overlay(rng, n, horizon)
            signal_shift = _signal_shift(overlay, base)
            exit_mid = _exit_midpoint(rng, n, horizon, signal_shift, base)

            factor_sums[h_idx] += overlay.sum(axis=0)
            factor_sums_sq[h_idx] += np.square(overlay).sum(axis=0)

            for s_idx, strategy in enumerate(STRATEGIES):
                direction, units = _strategy_positions(strategy, signal_shift, base, quantity)
                pnl_cases = _position_pnl(direction, units, exit_mid, base, fee_rate)
                portfolio = pnl_cases.sum(axis=1)

                sums[h_idx, s_idx] += portfolio.sum()
                sums_sq[h_idx, s_idx] += np.square(portfolio).sum()
                losses[h_idx, s_idx] += int(np.count_nonzero(portfolio < 0.0))
                active_counts[h_idx, s_idx] += _active_position_count(direction, units)
                market_sums[h_idx, s_idx] += pnl_cases.sum(axis=0)
                factor_pnl_sums[h_idx, s_idx] += overlay.T @ portfolio
                hist[h_idx, s_idx] += _hist_counts(portfolio, bins)
            done += n

    summary_rows = _summary_rows(sums, sums_sq, losses, active_counts, hist, bins, paths_per_horizon)
    market_rows = _market_rows(market_sums, paths_per_horizon, base)
    overlay_rows = _overlay_summary_rows(factor_sums, factor_sums_sq, paths_per_horizon)
    exposure_rows = _overlay_exposure_rows(factor_sums, factor_sums_sq, factor_pnl_sums, sums, sums_sq, paths_per_horizon)
    ranking_rows = _ranking_rows(summary_rows)
    elapsed = time.perf_counter() - started

    return {
        "report": "strategy_weather_overlay_simulation",
        "as_of": base["bundle"]["as_of"],
        "assumptions": {
            "data_status": "synthetic_sample unless cases input was regenerated",
            "paths_per_horizon": paths_per_horizon,
            "total_path_horizon_evaluations": paths_per_horizon * len(HORIZONS_MINUTES),
            "strategy_evaluations": paths_per_horizon * len(HORIZONS_MINUTES) * len(STRATEGIES),
            "chunk_size": chunk_size,
            "seed": seed,
            "quantity": quantity,
            "fee_rate": fee_rate,
            "horizons_minutes": HORIZONS_MINUTES,
            "strategies": STRATEGIES,
            "weather_factors": WEATHER_FACTORS,
            "pnl_unit": "mark-to-model dollars per strategy portfolio",
        },
        "elapsed_seconds": round(elapsed, 3),
        "cases": base["cases"],
        "summary_rows": summary_rows,
        "market_rows": market_rows,
        "weather_overlay_rows": overlay_rows,
        "weather_exposure_rows": exposure_rows,
        "ranking_rows": ranking_rows,
    }


def write_strategy_simulation_outputs(result: dict[str, Any], out_dir: str | Path) -> dict[str, Any]:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    created = []

    json_path = out / "strategy_simulation.json"
    json_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    created.append(json_path.name)

    for key, filename in [
        ("summary_rows", "strategy_horizon_summary.csv"),
        ("market_rows", "strategy_market_contribution.csv"),
        ("weather_overlay_rows", "weather_overlay_summary.csv"),
        ("weather_exposure_rows", "weather_overlay_exposure.csv"),
        ("ranking_rows", "strategy_rankings.csv"),
    ]:
        _write_csv(out / filename, result[key])
        created.append(filename)

    report_path = out / "strategy_simulation_report.md"
    report_path.write_text(render_strategy_simulation_report(result), encoding="utf-8")
    created.append(report_path.name)

    manifest = {
        "as_of": result["as_of"],
        "created": sorted([*created, "strategy_simulation_manifest.json"]),
        "paths_per_horizon": result["assumptions"]["paths_per_horizon"],
        "total_path_horizon_evaluations": result["assumptions"]["total_path_horizon_evaluations"],
        "strategy_evaluations": result["assumptions"]["strategy_evaluations"],
        "elapsed_seconds": result["elapsed_seconds"],
        "note": "High-volume synthetic strategy simulation with weather overlays. Not live market data and not trading advice.",
    }
    (out / "strategy_simulation_manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def render_strategy_simulation_report(result: dict[str, Any]) -> str:
    best_by_mean = max(result["summary_rows"], key=lambda row: row["mean_pnl"])
    best_by_es = max(result["summary_rows"], key=lambda row: row["expected_shortfall_5"])
    top_rows = sorted(result["ranking_rows"], key=lambda row: row["mean_pnl"], reverse=True)[:10]
    top_table = _markdown_table(
        ["Rank", "Strategy", "Horizon", "Mean", "P05", "Loss", "Sharpe", "ES5"],
        [
            [
                idx + 1,
                row["strategy"],
                f"{row['horizon_minutes']}m",
                _fmt_dollars(row["mean_pnl"]),
                _fmt_dollars(row["p05_pnl"]),
                _fmt_pct(row["loss_probability"]),
                row["sharpe"],
                _fmt_dollars(row["expected_shortfall_5"]),
            ]
            for idx, row in enumerate(top_rows)
        ],
    )
    return (
        "# Weather-Overlay Strategy Simulation\n\n"
        f"As of: `{result['as_of']}`\n\n"
        "Synthetic sample unless regenerated with live/user-supplied cases. This is a research artifact for strategy testing; it is not live market data and not trading advice.\n\n"
        "## Run Configuration\n\n"
        f"- Paths per horizon: `{result['assumptions']['paths_per_horizon']:,}`\n"
        f"- Horizons: `{', '.join(str(h) + 'm' for h in result['assumptions']['horizons_minutes'])}`\n"
        f"- Strategies tested: `{', '.join(result['assumptions']['strategies'])}`\n"
        f"- Total path-horizon evaluations: `{result['assumptions']['total_path_horizon_evaluations']:,}`\n"
        f"- Strategy evaluations: `{result['assumptions']['strategy_evaluations']:,}`\n"
        f"- Runtime: `{result['elapsed_seconds']:.3f}s`\n\n"
        "## Headline\n\n"
        f"- Best mean PnL: `{best_by_mean['strategy']}` at `{best_by_mean['horizon_minutes']}m` with `{_fmt_dollars(best_by_mean['mean_pnl'])}`.\n"
        f"- Best ES5: `{best_by_es['strategy']}` at `{best_by_es['horizon_minutes']}m` with `{_fmt_dollars(best_by_es['expected_shortfall_5'])}`.\n\n"
        "## Top Strategy-Horizon Rows\n\n"
        + top_table
        + "\n"
    )


def _prepare_base_inputs(
    cases: Sequence[SpeculativeMarketCase],
    *,
    as_of: str,
    quantity: float,
    fee_rate: float,
) -> dict[str, Any]:
    bundle = build_quant_spec_bundle(cases, as_of=as_of, quantity=quantity, fee_rate=fee_rate)
    markets = [case.market for case in cases]
    edge_by_market = {row["market"]: row for row in bundle["reports"]["edge_screener"]["rows"]}
    disagree_by_market = {row["market"]: row for row in bundle["reports"]["market_disagreement"]["rows"]}
    sensitivity_by_market = {row["market"]: row for row in bundle["reports"]["weather_sensitivity"]["rows"]}
    alloc_by_market = {row["market"]: row for row in bundle["reports"]["correlation_allocation"]["rows"]}

    fair = np.array([case.fair_prob for case in cases], dtype=np.float64)
    bid = np.array([case.bid for case in cases], dtype=np.float64)
    ask = np.array([case.ask for case in cases], dtype=np.float64)
    mid = (bid + ask) / 2.0
    spread = ask - bid
    confidence = np.array([case.confidence for case in cases], dtype=np.float64)
    sigma = np.array([case.sigma_f for case in cases], dtype=np.float64)
    bid_depth = np.array([case.bid_depth for case in cases], dtype=np.float64)
    ask_depth = np.array([case.ask_depth for case in cases], dtype=np.float64)

    side_dir = np.array([_side_direction(edge_by_market[market]["side"]) for market in markets], dtype=np.float64)
    net_edge = np.array([edge_by_market[market]["net_edge"] for market in markets], dtype=np.float64)
    posterior = np.array([disagree_by_market[market]["posterior_prob"] for market in markets], dtype=np.float64)
    z_score = np.array([disagree_by_market[market]["z_score"] for market in markets], dtype=np.float64)
    pin_risk = np.array([sensitivity_by_market[market]["pin_risk_2f"] for market in markets], dtype=np.float64)
    delta_1f = np.array([sensitivity_by_market[market]["delta_per_1f"] for market in markets], dtype=np.float64)
    alloc_units = np.array([alloc_by_market.get(market, {}).get("allocation_contracts", 0.0) for market in markets], dtype=np.float64)

    temp_coeff = delta_1f.copy()
    precip_coeff = np.zeros(len(cases), dtype=np.float64)
    wind_coeff = np.zeros(len(cases), dtype=np.float64)
    cloud_coeff = np.zeros(len(cases), dtype=np.float64)
    families = []
    regimes = []
    for idx, case in enumerate(cases):
        family = _contract_family(case.contract)
        families.append(family)
        regimes.append(case.regime)
        if family == "rain":
            precip_coeff[idx] = 0.19
            cloud_coeff[idx] = 0.07
            wind_coeff[idx] = 0.025
        elif family == "high_temp":
            cloud_coeff[idx] = -0.035
            wind_coeff[idx] = -0.055 if case.regime == "lake_breeze" else 0.015
        elif family == "low_temp":
            cloud_coeff[idx] = -0.045
            wind_coeff[idx] = -0.035
        if case.regime == "cap_strength":
            precip_coeff[idx] -= 0.04
        if case.regime == "marine_front":
            precip_coeff[idx] += 0.05
            cloud_coeff[idx] += 0.03
        if case.regime == "warm_advection":
            temp_coeff[idx] *= 1.15

    regime_labels = sorted(set(regimes))
    regime_idx = np.array([regime_labels.index(regime) for regime in regimes], dtype=np.int64)
    return {
        "bundle": bundle,
        "cases": [case.__dict__ if hasattr(case, "__dict__") else {
            "market": case.market,
            "city": case.city,
            "contract": case.contract,
            "target_date": case.target_date,
            "fair_prob": case.fair_prob,
            "bid": case.bid,
            "ask": case.ask,
            "confidence": case.confidence,
            "regime": case.regime,
            "sigma_f": case.sigma_f,
            "volume": case.volume,
            "open_interest": case.open_interest,
            "bid_depth": case.bid_depth,
            "ask_depth": case.ask_depth,
            "hours_to_settlement": case.hours_to_settlement,
            "half_life_hours": case.half_life_hours,
            "catalyst": case.catalyst,
            "notes": case.notes,
        } for case in cases],
        "markets": markets,
        "families": families,
        "fair": fair,
        "bid": bid,
        "ask": ask,
        "mid": mid,
        "spread": spread,
        "confidence": confidence,
        "sigma": sigma,
        "bid_depth": bid_depth,
        "ask_depth": ask_depth,
        "side_dir": side_dir,
        "net_edge": net_edge,
        "posterior": posterior,
        "z_score": z_score,
        "pin_risk": pin_risk,
        "delta_1f": delta_1f,
        "alloc_units": alloc_units,
        "temp_coeff": temp_coeff,
        "precip_coeff": precip_coeff,
        "wind_coeff": wind_coeff,
        "cloud_coeff": cloud_coeff,
        "regime_idx": regime_idx,
        "regime_count": len(regime_labels),
    }


def _weather_overlay(rng: np.random.Generator, n: int, horizon_minutes: int) -> np.ndarray:
    scale = math.sqrt(horizon_minutes / 360.0)
    temp = rng.normal(0.0, 2.2 * scale, n)
    precip = rng.normal(0.0, 0.70 * scale, n)
    wind = rng.normal(0.0, 0.80 * scale, n)
    cloud = rng.normal(0.0, 0.85 * scale, n)
    return np.column_stack([temp, precip, wind, cloud])


def _signal_shift(overlay: np.ndarray, base: dict[str, Any]) -> np.ndarray:
    return np.clip(
        overlay[:, [0]] * base["temp_coeff"]
        + overlay[:, [1]] * base["precip_coeff"]
        + overlay[:, [2]] * base["wind_coeff"]
        + overlay[:, [3]] * base["cloud_coeff"],
        -0.32,
        0.32,
    )


def _exit_midpoint(
    rng: np.random.Generator,
    n: int,
    horizon_minutes: int,
    signal_shift: np.ndarray,
    base: dict[str, Any],
) -> np.ndarray:
    scale = math.sqrt(horizon_minutes / 360.0)
    common = rng.normal(0.0, 0.012 * scale, (n, 1))
    regime_noise = rng.normal(0.0, 0.018 * scale, (n, base["regime_count"]))[:, base["regime_idx"]]
    idio_sigma = 0.010 + 0.070 * (1.0 - base["confidence"]) * scale + base["sigma"] / 260.0 * scale
    idio = rng.normal(0.0, 1.0, (n, len(base["markets"]))) * idio_sigma
    final_prob = np.clip(base["fair"] + signal_shift + common + regime_noise + idio, 0.01, 0.99)
    micro = rng.normal(0.0, 1.0, final_prob.shape) * (base["spread"] * 0.12 * scale)
    return np.clip(0.62 * final_prob + 0.38 * base["mid"] + micro, 0.01, 0.99)


def _strategy_positions(
    strategy: str,
    signal_shift: np.ndarray,
    base: dict[str, Any],
    quantity: float,
) -> tuple[np.ndarray, np.ndarray]:
    n = signal_shift.shape[0]
    c = signal_shift.shape[1]
    zeros = np.zeros((n, c), dtype=np.float64)
    if strategy == "edge_static":
        units = quantity * np.clip(base["net_edge"] / 0.08, 0.0, 1.35) * base["confidence"]
        units = np.where(base["net_edge"] > 0.015, units, 0.0)
        return np.broadcast_to(base["side_dir"], (n, c)), np.broadcast_to(units, (n, c))
    if strategy == "posterior_blend":
        buy_edge = base["posterior"] - base["ask"]
        sell_edge = base["bid"] - base["posterior"]
        direction = np.where((buy_edge >= sell_edge) & (buy_edge > 0.012), 1.0, np.where(sell_edge > 0.012, -1.0, 0.0))
        edge = np.maximum(buy_edge, sell_edge)
        units = quantity * np.clip(edge / 0.075, 0.0, 1.25) * base["confidence"]
        units = np.where(direction != 0.0, units, 0.0)
        return np.broadcast_to(direction, (n, c)), np.broadcast_to(units, (n, c))
    if strategy == "weather_momentum":
        direction = np.where(signal_shift > 0.012, 1.0, np.where(signal_shift < -0.012, -1.0, 0.0))
        units = quantity * np.clip(np.abs(signal_shift) / 0.070, 0.0, 1.20) * base["confidence"]
        return direction, np.where(direction != 0.0, units, 0.0)
    if strategy == "threshold_pin":
        eligible = (base["pin_risk"] > 0.08).astype(np.float64)
        direction = np.where(signal_shift > 0.006, 1.0, np.where(signal_shift < -0.006, -1.0, 0.0))
        units = quantity * np.clip(base["pin_risk"] * np.abs(signal_shift) / 0.018, 0.0, 1.10) * eligible
        return direction, np.where(direction != 0.0, units, 0.0)
    if strategy == "liquidity_reversion":
        fair_gap = base["fair"] - base["mid"]
        direction = np.where(fair_gap > 0.035, 1.0, np.where(fair_gap < -0.035, -1.0, 0.0))
        spread_scale = np.clip(base["spread"] / 0.04, 0.35, 1.50)
        units = quantity * np.clip(np.abs(fair_gap) / 0.10, 0.0, 1.10) * spread_scale
        return np.broadcast_to(direction, (n, c)), np.broadcast_to(units, (n, c))
    if strategy == "correlation_weighted":
        direction = base["side_dir"]
        units = np.where(base["alloc_units"] > 0.0, base["alloc_units"], 0.0)
        return np.broadcast_to(direction, (n, c)), np.broadcast_to(units, (n, c))
    if strategy == "ensemble_vote":
        buy_edge = base["posterior"] - base["ask"]
        sell_edge = base["bid"] - base["posterior"]
        post_dir = np.where((buy_edge >= sell_edge) & (buy_edge > 0.0), 1.0, np.where(sell_edge > 0.0, -1.0, 0.0))
        edge_prior = base["side_dir"] * np.clip(base["net_edge"] / 0.08, 0.0, 1.0)
        post_prior = post_dir * np.clip(np.maximum(buy_edge, sell_edge) / 0.07, 0.0, 1.0)
        vote = 0.42 * edge_prior + 0.34 * post_prior + 0.24 * np.clip(signal_shift / 0.055, -1.0, 1.0)
        direction = np.where(vote > 0.25, 1.0, np.where(vote < -0.25, -1.0, 0.0))
        units = quantity * np.clip(np.abs(vote), 0.0, 1.25) * base["confidence"]
        return direction, np.where(direction != 0.0, units, 0.0)
    return zeros, zeros


def _position_pnl(
    direction: np.ndarray,
    units: np.ndarray,
    exit_mid: np.ndarray,
    base: dict[str, Any],
    fee_rate: float,
) -> np.ndarray:
    entry = np.where(direction >= 0.0, base["ask"], base["bid"])
    depth = np.where(direction >= 0.0, base["ask_depth"], base["bid_depth"])
    fee = fee_rate * entry * (1.0 - entry)
    impact = np.minimum(0.05, units / np.maximum(1.0, depth) * np.maximum(base["spread"], 0.01))
    slippage = 0.5 * base["spread"] + impact
    pnl = units * direction * (exit_mid - entry) - units * (fee + slippage)
    return np.where((units > 0.0) & (direction != 0.0), pnl, 0.0)


def _active_position_count(direction: np.ndarray, units: np.ndarray) -> float:
    if direction.ndim == 1:
        return float(np.count_nonzero((units > 0.0) & (direction != 0.0)))
    return float(np.count_nonzero((units > 0.0) & (direction != 0.0)))


def _hist_counts(values: np.ndarray, bins: np.ndarray) -> np.ndarray:
    idx = np.clip(np.searchsorted(bins, values, side="right") - 1, 0, len(bins) - 2)
    return np.bincount(idx, minlength=len(bins) - 1)


def _summary_rows(
    sums: np.ndarray,
    sums_sq: np.ndarray,
    losses: np.ndarray,
    active_counts: np.ndarray,
    hist: np.ndarray,
    bins: np.ndarray,
    n: int,
) -> list[dict[str, Any]]:
    rows = []
    for h_idx, horizon in enumerate(HORIZONS_MINUTES):
        for s_idx, strategy in enumerate(STRATEGIES):
            mean = sums[h_idx, s_idx] / n
            variance = max(0.0, sums_sq[h_idx, s_idx] / n - mean * mean)
            std = math.sqrt(variance)
            hcounts = hist[h_idx, s_idx]
            p05 = _hist_percentile(hcounts, bins, 0.05)
            p50 = _hist_percentile(hcounts, bins, 0.50)
            p95 = _hist_percentile(hcounts, bins, 0.95)
            es5 = _hist_expected_shortfall(hcounts, bins, 0.05)
            rows.append(
                {
                    "horizon_minutes": horizon,
                    "strategy": strategy,
                    "paths": n,
                    "mean_pnl": round(mean, 6),
                    "std_pnl": round(std, 6),
                    "sharpe": round(mean / std, 6) if std else 0.0,
                    "loss_probability": round(float(losses[h_idx, s_idx]) / n, 6),
                    "p05_pnl": round(p05, 6),
                    "p50_pnl": round(p50, 6),
                    "p95_pnl": round(p95, 6),
                    "expected_shortfall_5": round(es5, 6),
                    "avg_active_positions": round(active_counts[h_idx, s_idx] / n, 6),
                }
            )
    return rows


def _market_rows(market_sums: np.ndarray, n: int, base: dict[str, Any]) -> list[dict[str, Any]]:
    rows = []
    for h_idx, horizon in enumerate(HORIZONS_MINUTES):
        for s_idx, strategy in enumerate(STRATEGIES):
            for c_idx, market in enumerate(base["markets"]):
                rows.append(
                    {
                        "horizon_minutes": horizon,
                        "strategy": strategy,
                        "market": market,
                        "family": base["families"][c_idx],
                        "mean_contribution": round(float(market_sums[h_idx, s_idx, c_idx]) / n, 6),
                    }
                )
    return rows


def _overlay_summary_rows(factor_sums: np.ndarray, factor_sums_sq: np.ndarray, n: int) -> list[dict[str, Any]]:
    rows = []
    for h_idx, horizon in enumerate(HORIZONS_MINUTES):
        for f_idx, factor in enumerate(WEATHER_FACTORS):
            mean = factor_sums[h_idx, f_idx] / n
            variance = max(0.0, factor_sums_sq[h_idx, f_idx] / n - mean * mean)
            rows.append(
                {
                    "horizon_minutes": horizon,
                    "factor": factor,
                    "mean": round(float(mean), 6),
                    "std": round(math.sqrt(variance), 6),
                }
            )
    return rows


def _overlay_exposure_rows(
    factor_sums: np.ndarray,
    factor_sums_sq: np.ndarray,
    factor_pnl_sums: np.ndarray,
    pnl_sums: np.ndarray,
    pnl_sums_sq: np.ndarray,
    n: int,
) -> list[dict[str, Any]]:
    rows = []
    for h_idx, horizon in enumerate(HORIZONS_MINUTES):
        for s_idx, strategy in enumerate(STRATEGIES):
            pnl_mean = pnl_sums[h_idx, s_idx] / n
            pnl_var = max(0.0, pnl_sums_sq[h_idx, s_idx] / n - pnl_mean * pnl_mean)
            pnl_std = math.sqrt(pnl_var)
            for f_idx, factor in enumerate(WEATHER_FACTORS):
                x_mean = factor_sums[h_idx, f_idx] / n
                x_var = max(0.0, factor_sums_sq[h_idx, f_idx] / n - x_mean * x_mean)
                x_std = math.sqrt(x_var)
                cov = factor_pnl_sums[h_idx, s_idx, f_idx] / n - pnl_mean * x_mean
                beta = cov / x_var if x_var else 0.0
                corr = cov / (x_std * pnl_std) if x_std and pnl_std else 0.0
                rows.append(
                    {
                        "horizon_minutes": horizon,
                        "strategy": strategy,
                        "factor": factor,
                        "beta": round(float(beta), 6),
                        "correlation": round(float(corr), 6),
                    }
                )
    return rows


def _ranking_rows(summary_rows: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    rows = []
    for horizon in HORIZONS_MINUTES:
        scoped = [row for row in summary_rows if row["horizon_minutes"] == horizon]
        scoped.sort(key=lambda row: row["mean_pnl"], reverse=True)
        for rank, row in enumerate(scoped, 1):
            rows.append({**row, "rank_by_mean": rank})
    return rows


def _hist_percentile(counts: np.ndarray, bins: np.ndarray, q: float) -> float:
    total = int(counts.sum())
    if total <= 0:
        return 0.0
    target = q * total
    cumulative = np.cumsum(counts)
    idx = int(np.searchsorted(cumulative, target, side="left"))
    idx = min(max(idx, 0), len(counts) - 1)
    prev = cumulative[idx - 1] if idx > 0 else 0
    count = counts[idx]
    frac = 0.0 if count == 0 else (target - prev) / count
    return float(bins[idx] + frac * (bins[idx + 1] - bins[idx]))


def _hist_expected_shortfall(counts: np.ndarray, bins: np.ndarray, q: float) -> float:
    threshold = _hist_percentile(counts, bins, q)
    mids = (bins[:-1] + bins[1:]) / 2.0
    mask = mids <= threshold
    tail_counts = counts[mask]
    if int(tail_counts.sum()) <= 0:
        return threshold
    return float((mids[mask] * tail_counts).sum() / tail_counts.sum())


def _write_csv(path: Path, rows: Sequence[dict[str, Any]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames = sorted({key for row in rows for key in row})
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def _markdown_table(headers: Sequence[str], rows: Sequence[Sequence[Any]]) -> str:
    header = "| " + " | ".join(headers) + " |"
    sep = "| " + " | ".join("---" for _ in headers) + " |"
    body = ["| " + " | ".join(str(value).replace("|", "/") for value in row) + " |" for row in rows]
    return "\n".join([header, sep, *body])


def _fmt_pct(value: float) -> str:
    return f"{100.0 * float(value):.1f}%"


def _fmt_dollars(value: float) -> str:
    return f"${float(value):.2f}"


def _side_direction(side: str) -> float:
    if side == "BUY_YES":
        return 1.0
    if side == "SELL_YES":
        return -1.0
    return 0.0


def _contract_family(contract: str) -> str:
    lower = contract.lower()
    if "rain" in lower:
        return "rain"
    if "low" in lower:
        return "low_temp"
    if "high" in lower:
        return "high_temp"
    return "other"


def load_sim_cases(path: str | Path | None) -> list[SpeculativeMarketCase]:
    return load_cases(path) if path else sample_speculative_cases()
