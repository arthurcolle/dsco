#!/usr/bin/env python3
"""Generate dense drill-down artifacts for weather-overlay strategy simulations."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path
from typing import Any, Iterable, Sequence

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np
from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from quant_strategy_sim import HORIZONS_MINUTES, STRATEGIES, WEATHER_FACTORS  # noqa: E402


CORE_PLOTS = [
    "executive_decision_board.png",
    "strategy_metric_lattice.png",
    "tail_risk_lattice.png",
    "utility_score_heatmap.png",
    "risk_reward_quadrant.png",
    "weather_beta_matrix.png",
    "weather_correlation_matrix.png",
    "weather_hedge_map.png",
    "family_contribution_dashboard.png",
    "market_weather_crosswalk.png",
]

CONTACT_SHEETS = [
    "decision_contact_sheet.png",
    "weather_contact_sheet.png",
    "market_contact_sheet.png",
    "strategy_profiles_contact_sheet.png",
    "ultra_detail_contact_sheet.png",
]

COLORS = {
    "bg": "#101111",
    "panel": "#151817",
    "ink": "#f4f1e8",
    "muted": "#b9c5bf",
    "grid": "#2f3a36",
    "cyan": "#36d1c4",
    "gold": "#f5c84b",
    "coral": "#ff6f61",
    "green": "#8fd17f",
    "violet": "#b38cff",
    "orange": "#ff9f68",
}


def write_ultra_detail(
    result: dict[str, Any],
    *,
    out_dir: str | Path,
    bundle: dict[str, Any] | None = None,
) -> dict[str, Any]:
    out = Path(out_dir)
    detail_dir = out / "ultra_detail"
    detail_dir.mkdir(parents=True, exist_ok=True)
    _style()

    plot_files = _plot_filenames()
    decision_rows = build_decision_score_rows(result)
    horizon_playbook = build_horizon_playbook_rows(result, decision_rows)
    risk_flags = build_risk_flag_rows(result)
    hedge_pairs = build_weather_hedge_pair_rows(result)
    family_rows = build_market_family_breakdown_rows(result)
    monitor_rows = build_weather_monitor_rows(result)
    plot_executive_decision_board(result, horizon_playbook, decision_rows, risk_flags, monitor_rows, detail_dir)
    plot_strategy_metric_lattice(result, detail_dir)
    plot_tail_risk_lattice(result, detail_dir)
    plot_utility_score_heatmap(decision_rows, detail_dir)
    plot_risk_reward_quadrant(result, decision_rows, detail_dir)
    plot_weather_matrix(result, detail_dir, metric="beta", filename="weather_beta_matrix.png")
    plot_weather_matrix(result, detail_dir, metric="correlation", filename="weather_correlation_matrix.png")
    plot_weather_hedge_map(hedge_pairs, detail_dir)
    plot_family_contribution_dashboard(result, family_rows, detail_dir)
    plot_market_weather_crosswalk(result, bundle, detail_dir)
    for horizon in HORIZONS_MINUTES:
        plot_market_contribution_horizon(result, horizon, detail_dir)
    for strategy in STRATEGIES:
        plot_strategy_profile(result, strategy, detail_dir)

    strategy_aggregate = build_strategy_aggregate_rows(result)
    best_by_horizon = build_best_by_horizon_rows(result)
    top_weather = sorted(result["weather_exposure_rows"], key=lambda row: abs(row["beta"]), reverse=True)[:40]
    _write_csv(detail_dir / "strategy_aggregate.csv", strategy_aggregate)
    _write_csv(detail_dir / "best_by_horizon.csv", best_by_horizon)
    _write_csv(detail_dir / "horizon_playbook.csv", horizon_playbook)
    _write_csv(detail_dir / "decision_utility_scores.csv", decision_rows)
    _write_csv(detail_dir / "risk_flags.csv", risk_flags)
    _write_csv(detail_dir / "weather_hedge_pairs.csv", hedge_pairs)
    _write_csv(detail_dir / "market_family_breakdown.csv", family_rows)
    _write_csv(detail_dir / "weather_monitor_list.csv", monitor_rows)
    _write_csv(detail_dir / "top_weather_exposures.csv", top_weather)
    _write_csv(detail_dir / "all_strategy_horizon_rows.csv", result["summary_rows"])

    make_contact_sheet(detail_dir, plot_files)
    write_html_index(result, detail_dir, plot_files)
    write_markdown_report(
        result,
        out,
        plot_files,
        strategy_aggregate,
        best_by_horizon,
        top_weather,
        horizon_playbook,
        risk_flags,
        hedge_pairs,
        family_rows,
        monitor_rows,
        decision_rows,
    )
    manifest = update_manifest(out, plot_files)
    return manifest


def _style() -> None:
    plt.rcParams.update(
        {
            "figure.facecolor": COLORS["bg"],
            "axes.facecolor": COLORS["panel"],
            "savefig.facecolor": COLORS["bg"],
            "text.color": COLORS["ink"],
            "axes.labelcolor": "#d7ddd7",
            "xtick.color": "#aab8b2",
            "ytick.color": "#aab8b2",
            "axes.edgecolor": "#4b5a56",
            "grid.color": COLORS["grid"],
            "font.family": "DejaVu Sans",
            "font.size": 9,
            "axes.titleweight": "bold",
        }
    )


def _plot_filenames() -> list[str]:
    files = [*CORE_PLOTS]
    files.extend(f"market_contribution_{horizon}m.png" for horizon in HORIZONS_MINUTES)
    files.extend(f"strategy_profile_{strategy}.png" for strategy in STRATEGIES)
    return files


def _save(fig: plt.Figure, out: Path, filename: str) -> None:
    fig.savefig(out / filename, dpi=190, bbox_inches="tight", pad_inches=0.2)
    plt.close(fig)


def _matrix(rows: Sequence[dict[str, Any]], metric: str) -> np.ndarray:
    indexed = {(row["strategy"], row["horizon_minutes"]): row for row in rows}
    return np.array(
        [[indexed[(strategy, horizon)][metric] for horizon in HORIZONS_MINUTES] for strategy in STRATEGIES],
        dtype=np.float64,
    )


def _market_matrix(result: dict[str, Any], horizon: int) -> tuple[list[str], np.ndarray]:
    markets = [case["market"] for case in result["cases"]]
    indexed = {
        (row["strategy"], row["market"]): row["mean_contribution"]
        for row in result["market_rows"]
        if row["horizon_minutes"] == horizon
    }
    data = np.array(
        [[indexed.get((strategy, market), 0.0) for market in markets] for strategy in STRATEGIES],
        dtype=np.float64,
    )
    return markets, data


def _short_strategy(strategy: str) -> str:
    return strategy.replace("_", "\n")


def _label_strategy(strategy: str) -> str:
    return strategy.replace("_", " ")


def _short_market(market: str) -> str:
    value = market.replace("KX", "")
    parts = value.split("-")
    if len(parts) >= 3:
        return f"{parts[0]} {parts[-1]}"
    return value[:18]


def _fmt_dollars(value: float) -> str:
    return f"${float(value):.2f}"


def _fmt_pct(value: float) -> str:
    return f"{100.0 * float(value):.1f}%"


def _heatmap(
    ax: plt.Axes,
    data: np.ndarray,
    *,
    title: str,
    xlabels: Sequence[str],
    ylabels: Sequence[str],
    cmap: str = "RdYlGn",
    centered: bool = True,
    annotate: bool = True,
) -> Any:
    if centered:
        vmax = max(abs(float(np.nanmin(data))), abs(float(np.nanmax(data))), 1e-9)
        image = ax.imshow(data, cmap=cmap, vmin=-vmax, vmax=vmax, aspect="auto")
    else:
        image = ax.imshow(data, cmap=cmap, aspect="auto")
    ax.set_title(title)
    ax.set_xticks(range(len(xlabels)), xlabels, rotation=0)
    ax.set_yticks(range(len(ylabels)), ylabels)
    if annotate:
        for i in range(data.shape[0]):
            for j in range(data.shape[1]):
                value = data[i, j]
                label = f"{value:.2f}" if abs(value) >= 1 else f"{value:.3f}"
                rgba = image.cmap(image.norm(value))
                luminance = 0.2126 * rgba[0] + 0.7152 * rgba[1] + 0.0722 * rgba[2]
                text_color = COLORS["bg"] if luminance > 0.58 else COLORS["ink"]
                ax.text(j, i, label, ha="center", va="center", fontsize=6.5, color=text_color)
    return image


def plot_strategy_metric_lattice(result: dict[str, Any], out: Path) -> None:
    metrics = [
        ("mean_pnl", "Mean PnL", "RdYlGn", True),
        ("sharpe", "Mean / Std", "PRGn", True),
        ("loss_probability", "Loss Probability", "magma_r", False),
        ("p05_pnl", "P05 PnL", "RdYlGn", True),
        ("p95_pnl", "P95 PnL", "viridis", False),
        ("expected_shortfall_5", "Expected Shortfall 5%", "RdYlGn", True),
        ("avg_active_positions", "Avg Active Positions", "cividis", False),
        ("std_pnl", "PnL Std Dev", "plasma", False),
    ]
    fig, axes = plt.subplots(4, 2, figsize=(18, 24), constrained_layout=True)
    fig.suptitle("Ultra-Detailed Strategy Metric Lattice", fontsize=23, weight="bold")
    for ax, (metric, title, cmap, centered) in zip(axes.ravel(), metrics):
        data = _matrix(result["summary_rows"], metric)
        im = _heatmap(
            ax,
            data,
            title=title,
            xlabels=[f"{h}m" for h in HORIZONS_MINUTES],
            ylabels=[_short_strategy(strategy) for strategy in STRATEGIES],
            cmap=cmap,
            centered=centered,
        )
        fig.colorbar(im, ax=ax, shrink=0.82)
    _save(fig, out, "strategy_metric_lattice.png")


def plot_tail_risk_lattice(result: dict[str, Any], out: Path) -> None:
    rows = result["summary_rows"]
    palette = ["#36d1c4", "#f5c84b", "#ff6f61", "#8fd17f", "#b38cff", "#ff9f68", "#d7ddd7"]
    fig, axes = plt.subplots(2, 2, figsize=(17, 12), constrained_layout=True)
    fig.suptitle("Tail, Dispersion, And Participation Diagnostics", fontsize=22, weight="bold")

    ax = axes[0, 0]
    for idx, strategy in enumerate(STRATEGIES):
        scoped = sorted([row for row in rows if row["strategy"] == strategy], key=lambda row: row["horizon_minutes"])
        ax.plot([row["horizon_minutes"] for row in scoped], [row["mean_pnl"] for row in scoped], color=palette[idx], marker="o", label=_label_strategy(strategy))
        ax.fill_between(
            [row["horizon_minutes"] for row in scoped],
            [row["p05_pnl"] for row in scoped],
            [row["p95_pnl"] for row in scoped],
            color=palette[idx],
            alpha=0.08,
        )
    ax.axhline(0, color=COLORS["ink"], alpha=0.35)
    ax.set_title("Mean With P05-P95 Envelope")
    ax.set_xlabel("horizon, minutes")
    ax.set_ylabel("PnL")
    ax.grid(alpha=0.35)
    ax.legend(frameon=False, labelcolor=COLORS["ink"], ncol=2, fontsize=8)

    ax = axes[0, 1]
    loss = _matrix(rows, "loss_probability")
    im = _heatmap(ax, loss, title="Loss Probability", xlabels=[f"{h}m" for h in HORIZONS_MINUTES], ylabels=[_short_strategy(s) for s in STRATEGIES], cmap="magma_r", centered=False)
    fig.colorbar(im, ax=ax, shrink=0.82)

    ax = axes[1, 0]
    es = _matrix(rows, "expected_shortfall_5")
    im = _heatmap(ax, es, title="Expected Shortfall 5%", xlabels=[f"{h}m" for h in HORIZONS_MINUTES], ylabels=[_short_strategy(s) for s in STRATEGIES], cmap="RdYlGn", centered=True)
    fig.colorbar(im, ax=ax, shrink=0.82)

    ax = axes[1, 1]
    active = _matrix(rows, "avg_active_positions")
    im = _heatmap(ax, active, title="Average Active Positions", xlabels=[f"{h}m" for h in HORIZONS_MINUTES], ylabels=[_short_strategy(s) for s in STRATEGIES], cmap="cividis", centered=False)
    fig.colorbar(im, ax=ax, shrink=0.82)
    _save(fig, out, "tail_risk_lattice.png")


def plot_executive_decision_board(
    result: dict[str, Any],
    horizon_playbook: Sequence[dict[str, Any]],
    decision_rows: Sequence[dict[str, Any]],
    risk_flags: Sequence[dict[str, Any]],
    monitor_rows: Sequence[dict[str, Any]],
    out: Path,
) -> None:
    fig = plt.figure(figsize=(18, 12), constrained_layout=True)
    gs = fig.add_gridspec(3, 2, height_ratios=[0.95, 1.05, 1.0])
    fig.suptitle("Executive Decision Board: Weather-Overlay Strategy Simulation", fontsize=24, weight="bold")
    fig.text(
        0.02,
        0.955,
        f"{result['assumptions']['paths_per_horizon']:,} paths per horizon | {result['assumptions']['strategy_evaluations']:,} strategy evaluations | synthetic research artifact",
        color=COLORS["muted"],
        fontsize=10,
    )

    ax = fig.add_subplot(gs[0, :])
    ax.axis("off")
    playbook_cells = [
        [
            f"{row['horizon_minutes']}m",
            row["return_leader"].replace("_", " "),
            _fmt_dollars(row["return_leader_mean_pnl"]),
            _fmt_pct(row["return_leader_loss_probability"]),
            row["conservative_leader"].replace("_", " "),
            row["tail_leader"].replace("_", " "),
        ]
        for row in horizon_playbook
    ]
    table = ax.table(
        cellText=playbook_cells,
        colLabels=["Horizon", "Return Leader", "Mean", "Loss", "Conservative", "Tail Leader"],
        loc="center",
        cellLoc="center",
    )
    table.auto_set_font_size(False)
    table.set_fontsize(10)
    table.scale(1, 1.9)
    for (row_idx, col_idx), cell in table.get_celld().items():
        cell.set_edgecolor("#42524c")
        if row_idx == 0:
            cell.set_facecolor("#22302c")
            cell.get_text().set_color(COLORS["ink"])
            cell.get_text().set_weight("bold")
        else:
            cell.set_facecolor("#151817")
            cell.get_text().set_color(COLORS["ink"])
    ax.set_title("Horizon Playbook", pad=12)

    ax = fig.add_subplot(gs[1, 0])
    utility_leaders = sorted(decision_rows, key=lambda row: row["conservative_utility"], reverse=True)[:10]
    labels = [f"{row['strategy'].replace('_', ' ')} {row['horizon_minutes']}m" for row in utility_leaders][::-1]
    values = [row["conservative_utility"] for row in utility_leaders][::-1]
    ax.barh(labels, values, color=[COLORS["cyan"] if value >= 0 else COLORS["coral"] for value in values], alpha=0.88)
    ax.axvline(0, color=COLORS["ink"], alpha=0.35)
    ax.set_title("Top Conservative Utility Scores")
    ax.set_xlabel("utility score")
    ax.grid(axis="x", alpha=0.35)

    ax = fig.add_subplot(gs[1, 1])
    flag_counts: dict[str, int] = {}
    for row in risk_flags:
        for flag in row["flags"].split(","):
            flag_counts[flag] = flag_counts.get(flag, 0) + 1
    flag_items = sorted(flag_counts.items(), key=lambda item: item[1])
    ax.barh([item[0].replace("_", " ") for item in flag_items], [item[1] for item in flag_items], color=COLORS["orange"], alpha=0.86)
    ax.set_title("Risk Flag Frequency")
    ax.set_xlabel("flagged rows")
    ax.grid(axis="x", alpha=0.35)

    ax = fig.add_subplot(gs[2, 0])
    top_monitor = monitor_rows[:10]
    ax.axis("off")
    monitor_cells = [
        [
            f"{row['horizon_minutes']}m",
            row["strategy"].replace("_", " "),
            row["factor"].replace("_", " "),
            f"{row['beta']:.2f}",
            "rises" if row["beta"] > 0 else "falls",
        ]
        for row in top_monitor
    ]
    monitor_table = ax.table(
        cellText=monitor_cells,
        colLabels=["Horizon", "Strategy", "Factor", "Beta", "Helps When"],
        loc="center",
        cellLoc="center",
    )
    monitor_table.auto_set_font_size(False)
    monitor_table.set_fontsize(8)
    monitor_table.scale(1, 1.45)
    for (row_idx, _), cell in monitor_table.get_celld().items():
        cell.set_edgecolor("#42524c")
        cell.set_facecolor("#22302c" if row_idx == 0 else "#151817")
        cell.get_text().set_color(COLORS["ink"])
    ax.set_title("Weather Monitors To Check First", pad=12)

    ax = fig.add_subplot(gs[2, 1])
    mean = _matrix(result["summary_rows"], "mean_pnl")
    best_by_horizon = np.max(mean, axis=0)
    regret = mean - best_by_horizon
    im = _heatmap(
        ax,
        regret,
        title="Regret Versus Best Mean Strategy At Same Horizon",
        xlabels=[f"{h}m" for h in HORIZONS_MINUTES],
        ylabels=[_short_strategy(strategy) for strategy in STRATEGIES],
        cmap="magma",
        centered=False,
        annotate=True,
    )
    fig.colorbar(im, ax=ax, shrink=0.82, label="mean PnL gap")
    _save(fig, out, "executive_decision_board.png")


def plot_utility_score_heatmap(decision_rows: Sequence[dict[str, Any]], out: Path) -> None:
    data = np.array(
        [
            [
                next(row["conservative_utility"] for row in decision_rows if row["strategy"] == strategy and row["horizon_minutes"] == horizon)
                for horizon in HORIZONS_MINUTES
            ]
            for strategy in STRATEGIES
        ],
        dtype=np.float64,
    )
    fig, ax = plt.subplots(figsize=(14.5, 8.5))
    im = _heatmap(
        ax,
        data,
        title="Conservative Utility Score: Mean Minus Tail, Volatility, And Loss Penalties",
        xlabels=[f"{h}m" for h in HORIZONS_MINUTES],
        ylabels=[_short_strategy(strategy) for strategy in STRATEGIES],
        cmap="RdYlGn",
        centered=True,
    )
    ax.text(
        -0.45,
        len(STRATEGIES) + 0.35,
        "Score = mean PnL - 0.35*std - 0.75*tail loss - 4*loss probability. Higher is better for defensive screening.",
        color=COLORS["muted"],
        fontsize=9,
    )
    fig.colorbar(im, ax=ax, shrink=0.82, label="conservative utility")
    _save(fig, out, "utility_score_heatmap.png")


def plot_risk_reward_quadrant(result: dict[str, Any], decision_rows: Sequence[dict[str, Any]], out: Path) -> None:
    utility = {(row["strategy"], row["horizon_minutes"]): row for row in decision_rows}
    rows = result["summary_rows"]
    fig, ax = plt.subplots(figsize=(14.2, 8.4))
    colors = [utility[(row["strategy"], row["horizon_minutes"])]["conservative_utility"] for row in rows]
    sizes = [80 + 260 * row["avg_active_positions"] / max(1.0, len(result["cases"])) for row in rows]
    scatter = ax.scatter(
        [row["expected_shortfall_5"] for row in rows],
        [row["mean_pnl"] for row in rows],
        c=colors,
        s=sizes,
        cmap="RdYlGn",
        edgecolor=COLORS["ink"],
        linewidth=0.45,
        alpha=0.9,
    )
    for row in sorted(rows, key=lambda item: item["mean_pnl"], reverse=True)[:8]:
        ax.text(
            row["expected_shortfall_5"] + 0.04,
            row["mean_pnl"] + 0.06,
            f"{row['strategy'].replace('_', ' ')} {row['horizon_minutes']}m",
            fontsize=8,
        )
    ax.axhline(0, color=COLORS["ink"], alpha=0.35)
    ax.axvline(0, color=COLORS["ink"], alpha=0.35)
    ax.set_title("Risk/Reward Quadrant With Conservative Utility Coloring")
    ax.set_xlabel("expected shortfall 5 percent")
    ax.set_ylabel("mean PnL")
    ax.grid(alpha=0.35)
    fig.colorbar(scatter, ax=ax, shrink=0.82, label="conservative utility")
    _save(fig, out, "risk_reward_quadrant.png")


def plot_weather_matrix(result: dict[str, Any], out: Path, *, metric: str, filename: str) -> None:
    rows = result["weather_exposure_rows"]
    fig, axes = plt.subplots(2, 2, figsize=(18, 13), constrained_layout=True)
    fig.suptitle(f"Weather Overlay {metric.title()} Matrix", fontsize=22, weight="bold")
    for ax, factor in zip(axes.ravel(), WEATHER_FACTORS):
        data = np.array(
            [
                [
                    next(
                        row[metric]
                        for row in rows
                        if row["strategy"] == strategy
                        and row["horizon_minutes"] == horizon
                        and row["factor"] == factor
                    )
                    for horizon in HORIZONS_MINUTES
                ]
                for strategy in STRATEGIES
            ],
            dtype=np.float64,
        )
        im = _heatmap(
            ax,
            data,
            title=factor.replace("_", " ").title(),
            xlabels=[f"{h}m" for h in HORIZONS_MINUTES],
            ylabels=[_short_strategy(strategy) for strategy in STRATEGIES],
            cmap="coolwarm",
            centered=True,
        )
        fig.colorbar(im, ax=ax, shrink=0.78)
    _save(fig, out, filename)


def plot_weather_hedge_map(hedge_pairs: Sequence[dict[str, Any]], out: Path) -> None:
    top = list(hedge_pairs[:24])
    labels = [
        f"{row['factor'].replace('_', ' ')} {row['horizon_minutes']}m\n{row['long_beta_strategy']} vs {row['short_beta_strategy']}"
        for row in top
    ]
    scores = [row["hedge_score"] for row in top]
    fig, ax = plt.subplots(figsize=(14.5, 11.5))
    y = np.arange(len(top))
    ax.barh(y, scores, color=COLORS["violet"], alpha=0.84)
    for idx, row in enumerate(top):
        ax.text(
            row["hedge_score"] + 0.01,
            idx,
            f"+{row['positive_beta']:.2f} / {row['negative_beta']:.2f}",
            va="center",
            fontsize=8,
            color=COLORS["muted"],
        )
    ax.set_yticks(y, labels)
    ax.invert_yaxis()
    ax.set_title("Weather Hedge Pair Map: Opposite-Signed Factor Exposures")
    ax.set_xlabel("hedge score: min(abs(beta pair))")
    ax.grid(axis="x", alpha=0.35)
    _save(fig, out, "weather_hedge_map.png")


def plot_family_contribution_dashboard(result: dict[str, Any], family_rows: Sequence[dict[str, Any]], out: Path) -> None:
    best = build_best_by_horizon_rows(result)
    families = sorted({row["family"] for row in family_rows})
    indexed = {
        (row["horizon_minutes"], row["strategy"], row["family"]): row["mean_contribution"]
        for row in family_rows
    }
    fig, ax = plt.subplots(figsize=(14.5, 8.5))
    x = np.arange(len(best))
    pos_bottom = np.zeros(len(best))
    neg_bottom = np.zeros(len(best))
    palette = {
        "high_temp": COLORS["coral"],
        "low_temp": COLORS["cyan"],
        "rain": COLORS["green"],
        "other": COLORS["violet"],
    }
    for family in families:
        values = np.array([indexed.get((row["horizon_minutes"], row["strategy"], family), 0.0) for row in best], dtype=np.float64)
        positive = np.where(values > 0, values, 0.0)
        negative = np.where(values < 0, values, 0.0)
        ax.bar(x, positive, bottom=pos_bottom, color=palette.get(family, COLORS["gold"]), label=family.replace("_", " "), alpha=0.86)
        ax.bar(x, negative, bottom=neg_bottom, color=palette.get(family, COLORS["gold"]), alpha=0.86)
        pos_bottom += positive
        neg_bottom += negative
    ax.axhline(0, color=COLORS["ink"], alpha=0.35)
    ax.set_xticks(x, [f"{row['horizon_minutes']}m\n{row['strategy'].replace('_', ' ')}" for row in best])
    ax.set_title("Family Contribution Stack For Best Strategy At Each Horizon")
    ax.set_xlabel("best row by horizon")
    ax.set_ylabel("mean PnL contribution")
    ax.grid(axis="y", alpha=0.35)
    ax.legend(frameon=False, labelcolor=COLORS["ink"], ncol=len(families))
    _save(fig, out, "family_contribution_dashboard.png")


def plot_market_weather_crosswalk(result: dict[str, Any], bundle: dict[str, Any] | None, out: Path) -> None:
    best = max(result["summary_rows"], key=lambda row: row["mean_pnl"])
    contributions = {
        row["market"]: row["mean_contribution"]
        for row in result["market_rows"]
        if row["strategy"] == best["strategy"] and row["horizon_minutes"] == best["horizon_minutes"]
    }
    cases = {case["market"]: case for case in result["cases"]}
    if bundle:
        sensitivity_rows = {row["market"]: row for row in bundle.get("reports", {}).get("weather_sensitivity", {}).get("rows", [])}
    else:
        sensitivity_rows = {}

    markets = list(cases)
    pin = np.array([sensitivity_rows.get(market, {}).get("pin_risk_2f", 0.0) for market in markets])
    delta = np.array([abs(sensitivity_rows.get(market, {}).get("market_delta_value", 0.0)) for market in markets])
    contrib = np.array([contributions.get(market, 0.0) for market in markets])
    confidence = np.array([cases[market]["confidence"] for market in markets])
    colors = [COLORS["cyan"] if value >= 0 else COLORS["coral"] for value in contrib]

    fig, ax = plt.subplots(figsize=(13.5, 8.2))
    ax.scatter(pin, contrib, s=130 + 170 * delta + 90 * confidence, c=colors, edgecolor=COLORS["ink"], linewidth=0.6, alpha=0.88)
    for market, x, y in zip(markets, pin, contrib):
        ax.text(x + 0.003, y + 0.03, _short_market(market), fontsize=8)
    ax.axhline(0, color=COLORS["ink"], alpha=0.35)
    ax.set_title(f"Weather Pin Risk Crosswalk For Best Row: {_label_strategy(best['strategy'])} {best['horizon_minutes']}m")
    ax.set_xlabel("2F pin risk")
    ax.set_ylabel("mean PnL contribution")
    ax.xaxis.set_major_formatter(lambda val, _: _fmt_pct(val))
    ax.grid(alpha=0.35)
    _save(fig, out, "market_weather_crosswalk.png")


def plot_market_contribution_horizon(result: dict[str, Any], horizon: int, out: Path) -> None:
    markets, data = _market_matrix(result, horizon)
    fig, ax = plt.subplots(figsize=(15.5, 9.0))
    im = _heatmap(
        ax,
        data,
        title=f"Market Contribution Heatmap At {horizon}m",
        xlabels=[_short_market(market) for market in markets],
        ylabels=[_short_strategy(strategy) for strategy in STRATEGIES],
        cmap="RdYlGn",
        centered=True,
        annotate=True,
    )
    ax.set_xlabel("market")
    ax.set_ylabel("strategy")
    fig.colorbar(im, ax=ax, shrink=0.82, label="mean contribution")
    _save(fig, out, f"market_contribution_{horizon}m.png")


def plot_strategy_profile(result: dict[str, Any], strategy: str, out: Path) -> None:
    scoped = sorted([row for row in result["summary_rows"] if row["strategy"] == strategy], key=lambda row: row["horizon_minutes"])
    horizons = [row["horizon_minutes"] for row in scoped]
    fig, axes = plt.subplots(2, 2, figsize=(16, 11), constrained_layout=True)
    fig.suptitle(f"Strategy Profile: {_label_strategy(strategy)}", fontsize=22, weight="bold")

    ax = axes[0, 0]
    ax.plot(horizons, [row["mean_pnl"] for row in scoped], marker="o", color=COLORS["cyan"], label="mean")
    ax.plot(horizons, [row["p05_pnl"] for row in scoped], marker="v", color=COLORS["coral"], label="p05")
    ax.plot(horizons, [row["p95_pnl"] for row in scoped], marker="^", color=COLORS["green"], label="p95")
    ax.axhline(0, color=COLORS["ink"], alpha=0.35)
    ax.set_title("PnL Path Envelope")
    ax.set_xlabel("horizon, minutes")
    ax.set_ylabel("PnL")
    ax.grid(alpha=0.35)
    ax.legend(frameon=False, labelcolor=COLORS["ink"])

    ax = axes[0, 1]
    ax.plot(horizons, [row["loss_probability"] for row in scoped], marker="o", color=COLORS["coral"], label="loss probability")
    ax2 = ax.twinx()
    ax2.plot(horizons, [row["avg_active_positions"] for row in scoped], marker="s", color=COLORS["gold"], label="active positions")
    ax.set_title("Loss And Participation")
    ax.set_xlabel("horizon, minutes")
    ax.set_ylabel("loss probability")
    ax2.set_ylabel("active positions")
    ax.yaxis.set_major_formatter(lambda val, _: _fmt_pct(val))
    ax.grid(alpha=0.35)
    ax.tick_params(colors="#aab8b2")
    ax2.tick_params(colors="#aab8b2")

    ax = axes[1, 0]
    best_horizon = max(scoped, key=lambda row: row["mean_pnl"])["horizon_minutes"]
    markets = [
        row for row in result["market_rows"]
        if row["strategy"] == strategy and row["horizon_minutes"] == best_horizon
    ]
    markets.sort(key=lambda row: row["mean_contribution"])
    values = [row["mean_contribution"] for row in markets]
    ax.barh([_short_market(row["market"]) for row in markets], values, color=[COLORS["cyan"] if v >= 0 else COLORS["coral"] for v in values], alpha=0.86)
    ax.axvline(0, color=COLORS["ink"], alpha=0.35)
    ax.set_title(f"Market Contributions At Best Horizon ({best_horizon}m)")
    ax.set_xlabel("mean contribution")
    ax.grid(axis="x", alpha=0.35)

    ax = axes[1, 1]
    for factor, color in zip(WEATHER_FACTORS, [COLORS["coral"], COLORS["cyan"], COLORS["gold"], COLORS["green"]]):
        factor_rows = sorted(
            [row for row in result["weather_exposure_rows"] if row["strategy"] == strategy and row["factor"] == factor],
            key=lambda row: row["horizon_minutes"],
        )
        ax.plot([row["horizon_minutes"] for row in factor_rows], [row["beta"] for row in factor_rows], marker="o", color=color, label=factor.replace("_", " "))
    ax.axhline(0, color=COLORS["ink"], alpha=0.35)
    ax.set_title("Weather Beta Term Structure")
    ax.set_xlabel("horizon, minutes")
    ax.set_ylabel("PnL beta")
    ax.grid(alpha=0.35)
    ax.legend(frameon=False, labelcolor=COLORS["ink"], fontsize=8)
    _save(fig, out, f"strategy_profile_{strategy}.png")


def build_strategy_aggregate_rows(result: dict[str, Any]) -> list[dict[str, Any]]:
    rows = []
    for strategy in STRATEGIES:
        scoped = [row for row in result["summary_rows"] if row["strategy"] == strategy]
        best = max(scoped, key=lambda row: row["mean_pnl"])
        best_sharpe = max(scoped, key=lambda row: row["sharpe"])
        rows.append(
            {
                "strategy": strategy,
                "avg_mean_pnl": round(float(np.mean([row["mean_pnl"] for row in scoped])), 6),
                "best_mean_horizon_minutes": best["horizon_minutes"],
                "best_mean_pnl": best["mean_pnl"],
                "best_sharpe_horizon_minutes": best_sharpe["horizon_minutes"],
                "best_sharpe": best_sharpe["sharpe"],
                "worst_expected_shortfall_5": min(row["expected_shortfall_5"] for row in scoped),
                "max_loss_probability": max(row["loss_probability"] for row in scoped),
                "avg_active_positions": round(float(np.mean([row["avg_active_positions"] for row in scoped])), 6),
            }
        )
    rows.sort(key=lambda row: row["best_mean_pnl"], reverse=True)
    return rows


def build_best_by_horizon_rows(result: dict[str, Any]) -> list[dict[str, Any]]:
    rows = []
    for horizon in HORIZONS_MINUTES:
        scoped = [row for row in result["summary_rows"] if row["horizon_minutes"] == horizon]
        rows.append(max(scoped, key=lambda row: row["mean_pnl"]))
    return rows


def build_decision_score_rows(result: dict[str, Any]) -> list[dict[str, Any]]:
    rows = []
    for row in result["summary_rows"]:
        tail_loss = max(0.0, -float(row["expected_shortfall_5"]))
        conservative_utility = (
            float(row["mean_pnl"])
            - 0.35 * float(row["std_pnl"])
            - 0.75 * tail_loss
            - 4.0 * float(row["loss_probability"])
        )
        score_per_active = conservative_utility / max(1.0, float(row["avg_active_positions"]))
        rows.append(
            {
                "horizon_minutes": row["horizon_minutes"],
                "strategy": row["strategy"],
                "mean_pnl": row["mean_pnl"],
                "std_pnl": row["std_pnl"],
                "loss_probability": row["loss_probability"],
                "expected_shortfall_5": row["expected_shortfall_5"],
                "avg_active_positions": row["avg_active_positions"],
                "conservative_utility": round(conservative_utility, 6),
                "utility_per_active_position": round(score_per_active, 6),
                "risk_bucket": _risk_bucket(row),
            }
        )
    return rows


def build_horizon_playbook_rows(result: dict[str, Any], decision_rows: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    utility = {(row["strategy"], row["horizon_minutes"]): row for row in decision_rows}
    rows = []
    for horizon in HORIZONS_MINUTES:
        scoped = [row for row in result["summary_rows"] if row["horizon_minutes"] == horizon]
        best_mean = max(scoped, key=lambda row: row["mean_pnl"])
        best_utility = max(scoped, key=lambda row: utility[(row["strategy"], row["horizon_minutes"])]["conservative_utility"])
        best_tail = max(scoped, key=lambda row: row["expected_shortfall_5"])
        if best_mean["strategy"] == best_utility["strategy"]:
            posture = "primary strategy is also conservative leader"
        else:
            posture = f"return leader differs from conservative leader: {best_mean['strategy']} vs {best_utility['strategy']}"
        rows.append(
            {
                "horizon_minutes": horizon,
                "return_leader": best_mean["strategy"],
                "return_leader_mean_pnl": best_mean["mean_pnl"],
                "return_leader_loss_probability": best_mean["loss_probability"],
                "return_leader_es5": best_mean["expected_shortfall_5"],
                "conservative_leader": best_utility["strategy"],
                "conservative_utility": utility[(best_utility["strategy"], horizon)]["conservative_utility"],
                "tail_leader": best_tail["strategy"],
                "tail_leader_es5": best_tail["expected_shortfall_5"],
                "decision_note": posture,
            }
        )
    return rows


def build_risk_flag_rows(result: dict[str, Any]) -> list[dict[str, Any]]:
    rows = []
    for row in result["summary_rows"]:
        flags = []
        if row["mean_pnl"] < 0:
            flags.append("negative_mean")
        if row["loss_probability"] >= 0.25:
            flags.append("high_loss_probability")
        if row["expected_shortfall_5"] <= -2.0:
            flags.append("left_tail")
        if row["sharpe"] < 0.5:
            flags.append("low_risk_adjusted_score")
        if row["avg_active_positions"] >= 0.85 * len(result["cases"]):
            flags.append("broad_book_exposure")
        if flags:
            rows.append(
                {
                    "horizon_minutes": row["horizon_minutes"],
                    "strategy": row["strategy"],
                    "flags": ",".join(flags),
                    "mean_pnl": row["mean_pnl"],
                    "loss_probability": row["loss_probability"],
                    "expected_shortfall_5": row["expected_shortfall_5"],
                    "sharpe": row["sharpe"],
                    "avg_active_positions": row["avg_active_positions"],
                    "suggested_action": _suggested_action(flags),
                }
            )
    return rows


def build_weather_hedge_pair_rows(result: dict[str, Any]) -> list[dict[str, Any]]:
    rows = []
    exposures = result["weather_exposure_rows"]
    for horizon in HORIZONS_MINUTES:
        for factor in WEATHER_FACTORS:
            scoped = [
                row for row in exposures
                if row["horizon_minutes"] == horizon and row["factor"] == factor
            ]
            positives = [row for row in scoped if row["beta"] > 0]
            negatives = [row for row in scoped if row["beta"] < 0]
            for pos in positives:
                for neg in negatives:
                    score = min(abs(pos["beta"]), abs(neg["beta"]))
                    rows.append(
                        {
                            "horizon_minutes": horizon,
                            "factor": factor,
                            "long_beta_strategy": pos["strategy"],
                            "short_beta_strategy": neg["strategy"],
                            "positive_beta": pos["beta"],
                            "negative_beta": neg["beta"],
                            "hedge_score": round(score, 6),
                            "interpretation": f"{pos['strategy']} benefits when {factor} rises while {neg['strategy']} is hurt",
                        }
                    )
    rows.sort(key=lambda row: row["hedge_score"], reverse=True)
    return rows


def build_market_family_breakdown_rows(result: dict[str, Any]) -> list[dict[str, Any]]:
    totals: dict[tuple[int, str, str], float] = {}
    abs_totals: dict[tuple[int, str], float] = {}
    for row in result["market_rows"]:
        key = (row["horizon_minutes"], row["strategy"], row["family"])
        parent = (row["horizon_minutes"], row["strategy"])
        contribution = float(row["mean_contribution"])
        totals[key] = totals.get(key, 0.0) + contribution
        abs_totals[parent] = abs_totals.get(parent, 0.0) + abs(contribution)
    rows = []
    for (horizon, strategy, family), contribution in sorted(totals.items()):
        denom = max(1e-9, abs_totals[(horizon, strategy)])
        rows.append(
            {
                "horizon_minutes": horizon,
                "strategy": strategy,
                "family": family,
                "mean_contribution": round(contribution, 6),
                "absolute_share": round(abs(contribution) / denom, 6),
            }
        )
    return rows


def build_weather_monitor_rows(result: dict[str, Any]) -> list[dict[str, Any]]:
    rows = []
    for row in sorted(result["weather_exposure_rows"], key=lambda item: abs(item["beta"]), reverse=True)[:30]:
        rows.append(
            {
                "horizon_minutes": row["horizon_minutes"],
                "strategy": row["strategy"],
                "factor": row["factor"],
                "beta": row["beta"],
                "correlation": row["correlation"],
                "monitor": _factor_monitor(row["factor"], row["beta"]),
                "why_it_matters": _factor_explanation(row["factor"], row["beta"]),
            }
        )
    return rows


def _risk_bucket(row: dict[str, Any]) -> str:
    if row["loss_probability"] >= 0.35 or row["expected_shortfall_5"] <= -4.0:
        return "high_tail_risk"
    if row["loss_probability"] >= 0.15 or row["expected_shortfall_5"] < 0:
        return "moderate_tail_risk"
    if row["expected_shortfall_5"] > 0 and row["loss_probability"] < 0.02:
        return "tail_resilient"
    return "balanced"


def _suggested_action(flags: Sequence[str]) -> str:
    if "negative_mean" in flags:
        return "do not deploy without a new signal filter"
    if "left_tail" in flags and "high_loss_probability" in flags:
        return "cap size and require confirmation from weather update"
    if "left_tail" in flags:
        return "limit horizon exposure or pair with opposite weather beta"
    if "high_loss_probability" in flags:
        return "use only when expected move magnitude is unusually high"
    if "broad_book_exposure" in flags:
        return "watch concentration and settlement correlation"
    return "monitor"


def _factor_monitor(factor: str, beta: float) -> str:
    direction = "rises" if beta > 0 else "falls"
    if factor == "temp_shift_f":
        return f"watch intraday temperature mean revisions; PnL improves when this factor {direction}"
    if factor == "precip_signal":
        return f"watch radar/HRRR precip probability updates; PnL improves when this factor {direction}"
    if factor == "wind_signal":
        return f"watch wind shift and boundary timing; PnL improves when this factor {direction}"
    if factor == "cloud_signal":
        return f"watch cloud cover and insolation changes; PnL improves when this factor {direction}"
    return f"watch {factor}; PnL improves when this factor {direction}"


def _factor_explanation(factor: str, beta: float) -> str:
    if factor == "temp_shift_f":
        return (
            "positive beta means warmer forecast revisions help this row"
            if beta > 0
            else "negative beta means cooler forecast revisions help this row and warmer revisions hurt"
        )
    if factor == "precip_signal":
        return (
            "positive beta means stronger precip signals help this row"
            if beta > 0
            else "negative beta means weaker precip signals help this row and stronger signals hurt"
        )
    if factor == "wind_signal":
        return (
            "positive beta means stronger wind/boundary signals help this row"
            if beta > 0
            else "negative beta means weaker wind/boundary signals help this row and stronger signals hurt"
        )
    if factor == "cloud_signal":
        return (
            "positive beta means cloudier revisions help this row"
            if beta > 0
            else "negative beta means clearer revisions help this row and cloudier revisions hurt"
        )
    return (
        f"positive beta means stronger {factor} helps this row"
        if beta > 0
        else f"negative beta means weaker {factor} helps this row and stronger values hurt"
    )


def make_contact_sheet(out: Path, plot_files: Sequence[str]) -> None:
    groups = {
        "decision_contact_sheet.png": [
            "executive_decision_board.png",
            "utility_score_heatmap.png",
            "risk_reward_quadrant.png",
            "tail_risk_lattice.png",
            "strategy_metric_lattice.png",
        ],
        "weather_contact_sheet.png": [
            "weather_beta_matrix.png",
            "weather_correlation_matrix.png",
            "weather_hedge_map.png",
            "market_weather_crosswalk.png",
        ],
        "market_contact_sheet.png": [
            "family_contribution_dashboard.png",
            *[f"market_contribution_{horizon}m.png" for horizon in HORIZONS_MINUTES],
        ],
        "strategy_profiles_contact_sheet.png": [
            *[f"strategy_profile_{strategy}.png" for strategy in STRATEGIES],
        ],
        "ultra_detail_contact_sheet.png": list(plot_files),
    }
    for filename, files in groups.items():
        _write_contact_sheet(out, filename, files)
    # Backward-compatible legacy thumbnail. Prefer the PNG sheets above.
    with Image.open(out / "ultra_detail_contact_sheet.png") as image:
        image.convert("RGB").save(out / "ultra_detail_contact_sheet.jpg", quality=88, optimize=True)


def _write_contact_sheet(out: Path, filename: str, files: Sequence[str]) -> None:
    images = [(Image.open(out / name).convert("RGB"), name) for name in files]
    thumb_w, thumb_h = 620, 380
    margin = 28
    title_h = 46
    cols = 2 if len(files) <= 8 else 3
    rows = int(np.ceil(len(images) / cols))
    sheet = Image.new(
        "RGB",
        (cols * thumb_w + (cols + 1) * margin, rows * (thumb_h + title_h) + (rows + 1) * margin),
        COLORS["bg"],
    )
    draw = ImageDraw.Draw(sheet)
    for idx, (image, name) in enumerate(images):
        image.thumbnail((thumb_w, thumb_h), Image.Resampling.LANCZOS)
        x = margin + (idx % cols) * (thumb_w + margin)
        y = margin + (idx // cols) * (thumb_h + title_h + margin)
        draw.rectangle([x - 1, y - 1, x + thumb_w + 1, y + thumb_h + title_h + 1], outline="#3d4c47", width=2)
        draw.text((x + 12, y + 14), name.removesuffix(".png").replace("_", " ").title(), fill=COLORS["ink"])
        sheet.paste(image, (x + (thumb_w - image.width) // 2, y + title_h + (thumb_h - image.height) // 2))
    sheet.save(out / filename, optimize=True)


def write_html_index(result: dict[str, Any], out: Path, plot_files: Sequence[str]) -> None:
    sheet_links = "\n".join(
        f'<a href="{name}">{name.removesuffix(".png").replace("_", " ").title()}</a>'
        for name in CONTACT_SHEETS
    )
    figures = "\n".join(
        f'<figure><img src="{name}" alt="{name.removesuffix(".png").replace("_", " ")}"><figcaption>{name.removesuffix(".png").replace("_", " ").title()}</figcaption></figure>'
        for name in plot_files
    )
    html = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Ultra Detailed Strategy Simulation</title>
<style>
:root{{color-scheme:dark;--bg:{COLORS['bg']};--ink:{COLORS['ink']};--muted:{COLORS['muted']};--line:#33423d}}
*{{box-sizing:border-box}}
body{{margin:0;background:var(--bg);color:var(--ink);font-family:Inter,Arial,sans-serif}}
main{{max-width:1700px;margin:0 auto;padding:28px}}
h1,p{{margin:0}}
h1{{font-size:clamp(32px,5vw,68px);line-height:1.02;max-width:1100px}}
p{{margin-top:12px;color:var(--muted);line-height:1.5;max-width:880px}}
.links{{display:flex;flex-wrap:wrap;gap:10px;margin-top:18px}}
.links a{{color:var(--ink);text-decoration:none;border:1px solid var(--line);border-radius:8px;padding:9px 11px;background:#151817}}
.grid{{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px;margin-top:28px}}
figure{{margin:0;border:1px solid var(--line);border-radius:8px;overflow:hidden;background:#151817}}
figure:nth-child(1),figure:nth-child(2){{grid-column:1/-1}}
img{{width:100%;height:auto;display:block}}
figcaption{{padding:10px 12px;color:var(--muted);font-size:14px;border-top:1px solid var(--line)}}
@media(max-width:900px){{main{{padding:18px}}.grid{{grid-template-columns:1fr}}figure:nth-child(1),figure:nth-child(2){{grid-column:auto}}}}
</style>
</head>
<body><main>
<h1>Ultra detailed strategy simulation</h1>
<p>{result['assumptions']['paths_per_horizon']:,} paths per horizon, {result['assumptions']['strategy_evaluations']:,} strategy evaluations, dense horizon/strategy/market/weather drill-downs.</p>
<div class="links">
{sheet_links}
</div>
<div class="grid">
{figures}
</div>
</main></body></html>
"""
    (out / "index.html").write_text(html, encoding="utf-8")


def write_markdown_report(
    result: dict[str, Any],
    out: Path,
    plot_files: Sequence[str],
    strategy_aggregate: Sequence[dict[str, Any]],
    best_by_horizon: Sequence[dict[str, Any]],
    top_weather: Sequence[dict[str, Any]],
    horizon_playbook: Sequence[dict[str, Any]],
    risk_flags: Sequence[dict[str, Any]],
    hedge_pairs: Sequence[dict[str, Any]],
    family_rows: Sequence[dict[str, Any]],
    monitor_rows: Sequence[dict[str, Any]],
    decision_rows: Sequence[dict[str, Any]],
) -> None:
    plot_links = "\n".join(f"- [{name.removesuffix('.png').replace('_', ' ').title()}](ultra_detail/{name})" for name in plot_files)
    utility_leaders = sorted(decision_rows, key=lambda row: row["conservative_utility"], reverse=True)[:12]
    family_leaders = sorted(family_rows, key=lambda row: abs(row["mean_contribution"]), reverse=True)[:18]
    text = f"""# Ultra Detailed Weather-Overlay Strategy Simulation

As of: `{result['as_of']}`

Synthetic sample unless regenerated with live or user-supplied cases. This is a research artifact for strategy testing; it is not live market data and not trading advice.

## Run Scale

- Paths per horizon: `{result['assumptions']['paths_per_horizon']:,}`
- Horizons: `{', '.join(str(h) + 'm' for h in result['assumptions']['horizons_minutes'])}`
- Strategies: `{', '.join(result['assumptions']['strategies'])}`
- Total path-horizon evaluations: `{result['assumptions']['total_path_horizon_evaluations']:,}`
- Strategy evaluations: `{result['assumptions']['strategy_evaluations']:,}`
- Runtime recorded by simulator: `{result['elapsed_seconds']:.3f}s`

## Decision Playbook

Use this as a workflow triage table, not an execution instruction. `Return leader` maximizes mean PnL, `conservative leader` maximizes the penalty-adjusted utility score, and `tail leader` maximizes ES5.

{_markdown_table(
    ["Horizon", "Return Leader", "Mean", "Loss", "ES5", "Conservative Leader", "Utility", "Tail Leader", "Tail ES5", "Decision Note"],
    [
        [
            f"{row['horizon_minutes']}m",
            row["return_leader"],
            _fmt_dollars(row["return_leader_mean_pnl"]),
            _fmt_pct(row["return_leader_loss_probability"]),
            _fmt_dollars(row["return_leader_es5"]),
            row["conservative_leader"],
            row["conservative_utility"],
            row["tail_leader"],
            _fmt_dollars(row["tail_leader_es5"]),
            row["decision_note"],
        ]
        for row in horizon_playbook
    ],
)}

## Conservative Utility Leaders

Formula: `mean PnL - 0.35*std - 0.75*tail_loss - 4*loss_probability`. It is intentionally defensive and should surface strategies that survive the left tail instead of only maximizing average PnL.

{_markdown_table(
    ["Strategy", "Horizon", "Utility", "Mean", "Loss", "ES5", "Active", "Risk Bucket"],
    [
        [
            row["strategy"],
            f"{row['horizon_minutes']}m",
            row["conservative_utility"],
            _fmt_dollars(row["mean_pnl"]),
            _fmt_pct(row["loss_probability"]),
            _fmt_dollars(row["expected_shortfall_5"]),
            row["avg_active_positions"],
            row["risk_bucket"],
        ]
        for row in utility_leaders
    ],
)}

## Risk Flags

These rows are the places to cap size, require confirmation, or skip unless a fresh catalyst improves the setup.

{_markdown_table(
    ["Horizon", "Strategy", "Flags", "Mean", "Loss", "ES5", "Sharpe", "Action"],
    [
        [
            f"{row['horizon_minutes']}m",
            row["strategy"],
            row["flags"],
            _fmt_dollars(row["mean_pnl"]),
            _fmt_pct(row["loss_probability"]),
            _fmt_dollars(row["expected_shortfall_5"]),
            row["sharpe"],
            row["suggested_action"],
        ]
        for row in risk_flags[:25]
    ],
)}

## Weather Monitor List

This converts the largest simulated weather betas into operational checks for forecast updates.

{_markdown_table(
    ["Strategy", "Horizon", "Factor", "Beta", "Corr", "Monitor", "Why"],
    [
        [
            row["strategy"],
            f"{row['horizon_minutes']}m",
            row["factor"],
            row["beta"],
            row["correlation"],
            row["monitor"],
            row["why_it_matters"],
        ]
        for row in monitor_rows[:18]
    ],
)}

## Hedge Pair Ideas

These are opposite-signed weather beta pairs. They are useful for scenario hedging and for finding strategies that offset the same weather update risk.

{_markdown_table(
    ["Horizon", "Factor", "Positive Strategy", "Negative Strategy", "Positive Beta", "Negative Beta", "Score"],
    [
        [
            f"{row['horizon_minutes']}m",
            row["factor"],
            row["long_beta_strategy"],
            row["short_beta_strategy"],
            row["positive_beta"],
            row["negative_beta"],
            row["hedge_score"],
        ]
        for row in hedge_pairs[:20]
    ],
)}

## Strategy Aggregates

{_markdown_table(
    ["Strategy", "Avg Mean", "Best Horizon", "Best Mean", "Best Sharpe Horizon", "Best Sharpe", "Worst ES5", "Max Loss", "Avg Active"],
    [
        [
            row["strategy"],
            _fmt_dollars(row["avg_mean_pnl"]),
            f"{row['best_mean_horizon_minutes']}m",
            _fmt_dollars(row["best_mean_pnl"]),
            f"{row['best_sharpe_horizon_minutes']}m",
            row["best_sharpe"],
            _fmt_dollars(row["worst_expected_shortfall_5"]),
            _fmt_pct(row["max_loss_probability"]),
            row["avg_active_positions"],
        ]
        for row in strategy_aggregate
    ],
)}

## Best Strategy By Horizon

{_markdown_table(
    ["Horizon", "Strategy", "Mean", "P05", "P95", "Loss", "Sharpe", "ES5", "Active"],
    [
        [
            f"{row['horizon_minutes']}m",
            row["strategy"],
            _fmt_dollars(row["mean_pnl"]),
            _fmt_dollars(row["p05_pnl"]),
            _fmt_dollars(row["p95_pnl"]),
            _fmt_pct(row["loss_probability"]),
            row["sharpe"],
            _fmt_dollars(row["expected_shortfall_5"]),
            row["avg_active_positions"],
        ]
        for row in best_by_horizon
    ],
)}

## Family Contribution Leaders

This shows which contract families drive the biggest modeled PnL contribution, regardless of sign.

{_markdown_table(
    ["Horizon", "Strategy", "Family", "Contribution", "Abs Share"],
    [
        [
            f"{row['horizon_minutes']}m",
            row["strategy"],
            row["family"],
            _fmt_dollars(row["mean_contribution"]),
            _fmt_pct(row["absolute_share"]),
        ]
        for row in family_leaders
    ],
)}

## Largest Weather Exposures

{_markdown_table(
    ["Strategy", "Horizon", "Factor", "Beta", "Correlation"],
    [
        [
            row["strategy"],
            f"{row['horizon_minutes']}m",
            row["factor"],
            row["beta"],
            row["correlation"],
        ]
        for row in top_weather[:25]
    ],
)}

## All Strategy-Horizon Rows

{_markdown_table(
    ["Horizon", "Strategy", "Mean", "Std", "P05", "P50", "P95", "Loss", "Sharpe", "ES5", "Active"],
    [
        [
            f"{row['horizon_minutes']}m",
            row["strategy"],
            _fmt_dollars(row["mean_pnl"]),
            _fmt_dollars(row["std_pnl"]),
            _fmt_dollars(row["p05_pnl"]),
            _fmt_dollars(row["p50_pnl"]),
            _fmt_dollars(row["p95_pnl"]),
            _fmt_pct(row["loss_probability"]),
            row["sharpe"],
            _fmt_dollars(row["expected_shortfall_5"]),
            row["avg_active_positions"],
        ]
        for row in sorted(result["summary_rows"], key=lambda item: (item["horizon_minutes"], item["strategy"]))
    ],
)}

## Plot Index

- [Ultra-detail HTML gallery](ultra_detail/index.html)
- [Decision contact sheet](ultra_detail/decision_contact_sheet.png)
- [Weather contact sheet](ultra_detail/weather_contact_sheet.png)
- [Market contact sheet](ultra_detail/market_contact_sheet.png)
- [Strategy profiles contact sheet](ultra_detail/strategy_profiles_contact_sheet.png)
- [Combined contact sheet](ultra_detail/ultra_detail_contact_sheet.png)
{plot_links}
"""
    (out / "ultra_detail_report.md").write_text(text, encoding="utf-8")


def update_manifest(out: Path, plot_files: Sequence[str]) -> dict[str, Any]:
    manifest_path = out / "strategy_simulation_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
    created = set(manifest.get("created", []))
    created.add("ultra_detail_report.md")
    created.add("ultra_detail/index.html")
    for name in CONTACT_SHEETS:
        created.add(f"ultra_detail/{name}")
    created.add("ultra_detail/ultra_detail_contact_sheet.jpg")
    for name in plot_files:
        created.add(f"ultra_detail/{name}")
    for name in [
        "strategy_aggregate.csv",
        "best_by_horizon.csv",
        "horizon_playbook.csv",
        "decision_utility_scores.csv",
        "risk_flags.csv",
        "weather_hedge_pairs.csv",
        "market_family_breakdown.csv",
        "weather_monitor_list.csv",
        "top_weather_exposures.csv",
        "all_strategy_horizon_rows.csv",
    ]:
        created.add(f"ultra_detail/{name}")
    manifest["created"] = sorted(created)
    manifest["ultra_detail_artifacts"] = sorted(item for item in created if item.startswith("ultra_detail/") or item == "ultra_detail_report.md")
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    _update_parent_index(out)
    _update_parent_manifest(out, manifest)
    return manifest


def _update_parent_index(out: Path) -> None:
    parent = out.parent
    if parent.name != "quant_speculation":
        return
    index_path = parent / "index.md"
    if not index_path.exists():
        return
    text = index_path.read_text(encoding="utf-8")
    section = (
        "\n\n## Strategy Simulation\n\n"
        "- [Strategy simulation report](strategy_simulation/strategy_simulation_report.md)\n"
        "- [Strategy simulation plot report](strategy_simulation/strategy_simulation_plot_report.md)\n"
        "- [Strategy simulation plot gallery](strategy_simulation/plots/index.html)\n"
        "- [Ultra-detail simulation report](strategy_simulation/ultra_detail_report.md)\n"
        "- [Ultra-detail plot gallery](strategy_simulation/ultra_detail/index.html)\n"
    )
    marker = "\n## Strategy Simulation\n"
    if marker in text:
        text = text[: text.index(marker)].rstrip() + section
    else:
        text = text.rstrip() + section
    index_path.write_text(text + "\n", encoding="utf-8")


def _update_parent_manifest(out: Path, sim_manifest: dict[str, Any]) -> None:
    parent_manifest = out.parent / "manifest.json"
    if out.parent.name != "quant_speculation" or not parent_manifest.exists():
        return
    parent = json.loads(parent_manifest.read_text(encoding="utf-8"))
    created = set(parent.get("created", []))
    for item in sim_manifest.get("created", []):
        created.add(f"{out.name}/{item}")
    parent["created"] = sorted(created)
    parent["strategy_simulation"] = {
        "paths_per_horizon": sim_manifest.get("paths_per_horizon"),
        "strategy_evaluations": sim_manifest.get("strategy_evaluations"),
        "elapsed_seconds": sim_manifest.get("elapsed_seconds"),
        "report_dir": out.name,
        "ultra_detail": True,
    }
    parent_manifest.write_text(json.dumps(parent, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _write_csv(path: Path, rows: Sequence[dict[str, Any]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames = sorted({key for row in rows for key in row})
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def _markdown_table(headers: Sequence[str], rows: Iterable[Sequence[Any]]) -> str:
    header = "| " + " | ".join(headers) + " |"
    sep = "| " + " | ".join("---" for _ in headers) + " |"
    body = ["| " + " | ".join(str(value).replace("|", "/") for value in row) + " |" for row in rows]
    return "\n".join([header, sep, *body])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", default="artifacts/reports/quant_speculation/strategy_simulation/strategy_simulation.json")
    parser.add_argument("--bundle", default="artifacts/reports/quant_speculation/bundle.json")
    parser.add_argument("--out-dir", default="artifacts/reports/quant_speculation/strategy_simulation")
    args = parser.parse_args()

    result = json.loads(Path(args.result).read_text(encoding="utf-8"))
    bundle_path = Path(args.bundle)
    bundle = json.loads(bundle_path.read_text(encoding="utf-8")) if bundle_path.exists() else None
    manifest = write_ultra_detail(result, out_dir=args.out_dir, bundle=bundle)
    print(json.dumps({"out_dir": args.out_dir, "ultra_plot_count": len(_plot_filenames()), **manifest}, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
