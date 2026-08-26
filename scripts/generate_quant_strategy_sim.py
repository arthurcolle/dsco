#!/usr/bin/env python3
"""Run high-volume strategy simulations and generate weather-overlay plots."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Sequence

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np
from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from quant_strategy_sim import (  # noqa: E402
    HORIZONS_MINUTES,
    STRATEGIES,
    WEATHER_FACTORS,
    load_sim_cases,
    run_strategy_simulation,
    write_strategy_simulation_outputs,
)


PLOT_FILES = [
    "strategy_mean_heatmap.png",
    "strategy_sharpe_heatmap.png",
    "strategy_frontier.png",
    "weather_overlay_betas.png",
    "horizon_decay_curves.png",
    "strategy_regret_heatmap.png",
    "market_contribution_stack.png",
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
}


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
            "font.size": 10,
            "axes.titleweight": "bold",
        }
    )


def write_strategy_simulation_plots(result: dict[str, Any], out_dir: str | Path) -> dict[str, Any]:
    _style()
    out = Path(out_dir)
    plot_dir = out / "plots"
    plot_dir.mkdir(parents=True, exist_ok=True)

    plot_strategy_mean_heatmap(result, plot_dir)
    plot_strategy_sharpe_heatmap(result, plot_dir)
    plot_strategy_frontier(result, plot_dir)
    plot_weather_overlay_betas(result, plot_dir)
    plot_horizon_decay_curves(result, plot_dir)
    plot_strategy_regret_heatmap(result, plot_dir)
    plot_market_contribution_stack(result, plot_dir)
    make_contact_sheet(plot_dir)
    write_html_index(result, plot_dir)
    write_markdown_plot_report(result, out)
    update_parent_index(out)
    manifest = update_sim_manifest(out)
    return manifest


def _save(fig: plt.Figure, plot_dir: Path, filename: str) -> None:
    fig.savefig(plot_dir / filename, dpi=170, bbox_inches="tight", pad_inches=0.18)
    plt.close(fig)


def _matrix(rows: Sequence[dict[str, Any]], metric: str) -> np.ndarray:
    indexed = {(row["strategy"], row["horizon_minutes"]): row for row in rows}
    return np.array(
        [[indexed[(strategy, horizon)][metric] for horizon in HORIZONS_MINUTES] for strategy in STRATEGIES],
        dtype=np.float64,
    )


def _short_strategy(strategy: str) -> str:
    return strategy.replace("_", "\n")


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
    data: np.ndarray,
    *,
    title: str,
    cbar_label: str,
    filename: str,
    plot_dir: Path,
    cmap: str = "RdYlGn",
    center: float | None = 0.0,
) -> None:
    fig, ax = plt.subplots(figsize=(12.8, 7.8))
    if center is None:
        im = ax.imshow(data, cmap=cmap)
    else:
        vmax = max(abs(float(np.nanmin(data))), abs(float(np.nanmax(data))), 1e-9)
        im = ax.imshow(data, cmap=cmap, vmin=-vmax, vmax=vmax)
    ax.set_title(title)
    ax.set_xticks(range(len(HORIZONS_MINUTES)), [f"{h}m" for h in HORIZONS_MINUTES])
    ax.set_yticks(range(len(STRATEGIES)), [_short_strategy(strategy) for strategy in STRATEGIES])
    for i in range(data.shape[0]):
        for j in range(data.shape[1]):
            value = data[i, j]
            label = f"{value:.2f}" if abs(value) >= 1 else f"{value:.3f}"
            ax.text(j, i, label, ha="center", va="center", color=COLORS["ink"], fontsize=8)
    ax.set_xlabel("forecast horizon")
    ax.set_ylabel("strategy")
    fig.colorbar(im, ax=ax, shrink=0.84, label=cbar_label)
    _save(fig, plot_dir, filename)


def plot_strategy_mean_heatmap(result: dict[str, Any], plot_dir: Path) -> None:
    data = _matrix(result["summary_rows"], "mean_pnl")
    _heatmap(
        data,
        title="Mean PnL By Strategy And Forecast Horizon",
        cbar_label="mean PnL",
        filename="strategy_mean_heatmap.png",
        plot_dir=plot_dir,
    )


def plot_strategy_sharpe_heatmap(result: dict[str, Any], plot_dir: Path) -> None:
    data = _matrix(result["summary_rows"], "sharpe")
    _heatmap(
        data,
        title="Risk-Adjusted Score By Strategy And Horizon",
        cbar_label="mean / std",
        filename="strategy_sharpe_heatmap.png",
        plot_dir=plot_dir,
        cmap="PRGn",
    )


def plot_strategy_frontier(result: dict[str, Any], plot_dir: Path) -> None:
    rows = result["summary_rows"]
    horizon_palette = dict(zip(HORIZONS_MINUTES, ["#36d1c4", "#8fd17f", "#f5c84b", "#ff9f68", "#ff6f61", "#b38cff"]))
    fig, ax = plt.subplots(figsize=(13.2, 7.6))
    for strategy in STRATEGIES:
        scoped = [row for row in rows if row["strategy"] == strategy]
        x = [row["expected_shortfall_5"] for row in scoped]
        y = [row["mean_pnl"] for row in scoped]
        sizes = [70 + 420 * row["loss_probability"] for row in scoped]
        colors = [horizon_palette[row["horizon_minutes"]] for row in scoped]
        ax.scatter(x, y, s=sizes, c=colors, edgecolor=COLORS["ink"], linewidth=0.45, alpha=0.88, label=strategy.replace("_", " "))
        ax.plot(x, y, color=COLORS["grid"], alpha=0.55, linewidth=1)
    best = max(rows, key=lambda row: row["mean_pnl"])
    ax.scatter([best["expected_shortfall_5"]], [best["mean_pnl"]], s=240, facecolors="none", edgecolors=COLORS["gold"], linewidth=2.4)
    ax.text(best["expected_shortfall_5"], best["mean_pnl"] + 0.18, f"best mean: {best['strategy']} {best['horizon_minutes']}m", color=COLORS["gold"], ha="center")
    ax.axhline(0, color=COLORS["ink"], alpha=0.35)
    ax.set_title("Strategy Frontier: Mean PnL Vs Left Tail")
    ax.set_xlabel("expected shortfall 5 percent")
    ax.set_ylabel("mean PnL")
    ax.grid(alpha=0.35)
    ax.legend(frameon=False, labelcolor=COLORS["ink"], ncol=2, fontsize=8)
    _save(fig, plot_dir, "strategy_frontier.png")


def plot_weather_overlay_betas(result: dict[str, Any], plot_dir: Path) -> None:
    rows = result["weather_exposure_rows"]
    fig, axes = plt.subplots(2, 2, figsize=(15.5, 9.5), sharex=True)
    factor_palette = {
        "temp_shift_f": COLORS["coral"],
        "precip_signal": COLORS["cyan"],
        "wind_signal": COLORS["gold"],
        "cloud_signal": COLORS["green"],
    }
    for ax, factor in zip(axes.ravel(), WEATHER_FACTORS):
        data = np.array(
            [
                [row["beta"] for row in rows if row["strategy"] == strategy and row["factor"] == factor and row["horizon_minutes"] == horizon][0]
                for strategy in STRATEGIES
                for horizon in HORIZONS_MINUTES
            ],
            dtype=np.float64,
        ).reshape(len(STRATEGIES), len(HORIZONS_MINUTES))
        y = np.arange(len(STRATEGIES))
        offsets = np.linspace(-0.24, 0.24, len(HORIZONS_MINUTES))
        for h_idx, horizon in enumerate(HORIZONS_MINUTES):
            ax.scatter(data[:, h_idx], y + offsets[h_idx], s=42, color=factor_palette[factor], alpha=0.45 + h_idx / 12, label=f"{horizon}m" if factor == WEATHER_FACTORS[0] else "")
        ax.axvline(0, color=COLORS["ink"], alpha=0.35)
        ax.set_title(factor.replace("_", " ").title())
        ax.set_yticks(y, [strategy.replace("_", " ") for strategy in STRATEGIES])
        ax.grid(axis="x", alpha=0.32)
    axes[0, 0].legend(frameon=False, labelcolor=COLORS["ink"], ncol=3, fontsize=8)
    fig.suptitle("Weather Overlay PnL Betas By Strategy", fontsize=19, weight="bold")
    fig.supxlabel("PnL beta per factor unit")
    _save(fig, plot_dir, "weather_overlay_betas.png")


def plot_horizon_decay_curves(result: dict[str, Any], plot_dir: Path) -> None:
    rows = result["summary_rows"]
    fig, ax = plt.subplots(figsize=(13.2, 7.6))
    palette = ["#36d1c4", "#f5c84b", "#ff6f61", "#8fd17f", "#b38cff", "#ff9f68", "#d7ddd7"]
    for idx, strategy in enumerate(STRATEGIES):
        scoped = sorted([row for row in rows if row["strategy"] == strategy], key=lambda row: row["horizon_minutes"])
        ax.plot(
            [row["horizon_minutes"] for row in scoped],
            [row["mean_pnl"] for row in scoped],
            marker="o",
            linewidth=2.2,
            color=palette[idx % len(palette)],
            label=strategy.replace("_", " "),
        )
    ax.axhline(0, color=COLORS["ink"], alpha=0.35)
    ax.set_title("Horizon Decay And Weather Signal Maturation")
    ax.set_xlabel("forecast horizon, minutes")
    ax.set_ylabel("mean PnL")
    ax.grid(alpha=0.35)
    ax.legend(frameon=False, labelcolor=COLORS["ink"], ncol=2)
    _save(fig, plot_dir, "horizon_decay_curves.png")


def plot_strategy_regret_heatmap(result: dict[str, Any], plot_dir: Path) -> None:
    mean = _matrix(result["summary_rows"], "mean_pnl")
    best_by_horizon = np.max(mean, axis=0)
    regret = mean - best_by_horizon
    _heatmap(
        regret,
        title="Strategy Regret Vs Best Strategy At Same Horizon",
        cbar_label="mean PnL gap",
        filename="strategy_regret_heatmap.png",
        plot_dir=plot_dir,
        cmap="magma",
        center=None,
    )


def plot_market_contribution_stack(result: dict[str, Any], plot_dir: Path) -> None:
    best = max(result["summary_rows"], key=lambda row: row["mean_pnl"])
    rows = [
        row for row in result["market_rows"]
        if row["strategy"] == best["strategy"] and row["horizon_minutes"] == best["horizon_minutes"]
    ]
    rows.sort(key=lambda row: row["mean_contribution"])
    fig, ax = plt.subplots(figsize=(13.2, 7.6))
    values = [row["mean_contribution"] for row in rows]
    colors = [COLORS["cyan"] if value >= 0 else COLORS["coral"] for value in values]
    ax.barh([_short_market(row["market"]) for row in rows], values, color=colors, alpha=0.88)
    ax.axvline(0, color=COLORS["ink"], alpha=0.4)
    for idx, value in enumerate(values):
        ax.text(value + (0.03 if value >= 0 else -0.03), idx, _fmt_dollars(value), va="center", ha="left" if value >= 0 else "right")
    ax.set_title(f"Market Contribution Stack: {best['strategy'].replace('_', ' ')} at {best['horizon_minutes']}m")
    ax.set_xlabel("mean PnL contribution")
    ax.grid(axis="x", alpha=0.35)
    _save(fig, plot_dir, "market_contribution_stack.png")


def make_contact_sheet(plot_dir: Path) -> None:
    images = [Image.open(plot_dir / name).convert("RGB") for name in PLOT_FILES]
    thumb_w, thumb_h = 560, 330
    margin = 26
    title_h = 48
    rows = 4
    sheet = Image.new("RGB", (2 * thumb_w + 3 * margin, rows * (thumb_h + title_h) + (rows + 1) * margin), COLORS["bg"])
    draw = ImageDraw.Draw(sheet)
    for idx, (img, name) in enumerate(zip(images, PLOT_FILES)):
        img.thumbnail((thumb_w, thumb_h), Image.Resampling.LANCZOS)
        x = margin + (idx % 2) * (thumb_w + margin)
        y = margin + (idx // 2) * (thumb_h + title_h + margin)
        draw.rectangle([x - 1, y - 1, x + thumb_w + 1, y + thumb_h + title_h + 1], outline="#3d4c47", width=2)
        draw.text((x + 14, y + 15), name.removesuffix(".png").replace("_", " ").title(), fill=COLORS["ink"])
        sheet.paste(img, (x + (thumb_w - img.width) // 2, y + title_h + (thumb_h - img.height) // 2))
    sheet.save(plot_dir / "simulation_contact_sheet.jpg", quality=88, optimize=True)


def write_html_index(result: dict[str, Any], plot_dir: Path) -> None:
    figures = "\n".join(
        f'<figure><img src="{name}" alt="{name.removesuffix(".png").replace("_", " ")}"><figcaption>{name.removesuffix(".png").replace("_", " ").title()}</figcaption></figure>'
        for name in PLOT_FILES
    )
    html = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Strategy Simulation Plots</title>
<style>
:root{{color-scheme:dark;--bg:{COLORS['bg']};--ink:{COLORS['ink']};--muted:{COLORS['muted']};--line:#33423d}}
*{{box-sizing:border-box}}
body{{margin:0;background:var(--bg);color:var(--ink);font-family:Inter,Arial,sans-serif}}
main{{max-width:1500px;margin:0 auto;padding:28px}}
h1,p{{margin:0}}
h1{{font-size:clamp(32px,5vw,64px);line-height:1.02;max-width:980px}}
p{{margin-top:12px;color:var(--muted);line-height:1.5;max-width:820px}}
.grid{{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px;margin-top:28px}}
figure{{margin:0;border:1px solid var(--line);border-radius:8px;overflow:hidden;background:#151817}}
figure:first-child{{grid-column:1/-1}}
img{{width:100%;height:auto;display:block}}
figcaption{{padding:10px 12px;color:var(--muted);font-size:14px;border-top:1px solid var(--line)}}
@media(max-width:900px){{main{{padding:18px}}.grid{{grid-template-columns:1fr}}figure:first-child{{grid-column:auto}}}}
</style>
</head>
<body><main>
<h1>Strategy simulation plots</h1>
<p>{result['assumptions']['paths_per_horizon']:,} paths per horizon across {len(result['assumptions']['strategies'])} strategies and {len(result['assumptions']['horizons_minutes'])} forecast horizons. Synthetic sample unless regenerated with live or user-supplied cases.</p>
<div class="grid">
{figures}
</div>
</main></body></html>
"""
    (plot_dir / "index.html").write_text(html, encoding="utf-8")


def write_markdown_plot_report(result: dict[str, Any], out: Path) -> None:
    best_mean = max(result["summary_rows"], key=lambda row: row["mean_pnl"])
    best_sharpe = max(result["summary_rows"], key=lambda row: row["sharpe"])
    most_weather_sensitive = max(result["weather_exposure_rows"], key=lambda row: abs(row["beta"]))
    text = f"""# Strategy Simulation Plot Report

As of: `{result['as_of']}`

Synthetic sample unless regenerated with live or user-supplied cases. This is a research artifact for strategy testing; it is not live market data and not trading advice.

## Key Readouts

- Paths per horizon: `{result['assumptions']['paths_per_horizon']:,}` across `{len(result['assumptions']['horizons_minutes'])}` horizons and `{len(result['assumptions']['strategies'])}` strategy policies.
- Strategy evaluations: `{result['assumptions']['strategy_evaluations']:,}`.
- Best mean PnL: `{best_mean['strategy']}` at `{best_mean['horizon_minutes']}m` with `{_fmt_dollars(best_mean['mean_pnl'])}`.
- Best Sharpe-style score: `{best_sharpe['strategy']}` at `{best_sharpe['horizon_minutes']}m` with `{best_sharpe['sharpe']}`.
- Strongest weather beta: `{most_weather_sensitive['strategy']}` on `{most_weather_sensitive['factor']}` at `{most_weather_sensitive['horizon_minutes']}m` with beta `{most_weather_sensitive['beta']}`.

## Plot Gallery

![Strategy mean heatmap](plots/strategy_mean_heatmap.png)

![Strategy Sharpe heatmap](plots/strategy_sharpe_heatmap.png)

![Strategy frontier](plots/strategy_frontier.png)

![Weather overlay betas](plots/weather_overlay_betas.png)

![Horizon decay curves](plots/horizon_decay_curves.png)

![Strategy regret heatmap](plots/strategy_regret_heatmap.png)

![Market contribution stack](plots/market_contribution_stack.png)
"""
    (out / "strategy_simulation_plot_report.md").write_text(text, encoding="utf-8")


def update_parent_index(out: Path) -> None:
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
    )
    marker = "\n## Strategy Simulation\n"
    if marker in text:
        text = text[: text.index(marker)].rstrip() + section
    else:
        text = text.rstrip() + section
    index_path.write_text(text + "\n", encoding="utf-8")


def update_sim_manifest(out: Path) -> dict[str, Any]:
    manifest_path = out / "strategy_simulation_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
    created = set(manifest.get("created", []))
    created.add("strategy_simulation_plot_report.md")
    created.add("plots/index.html")
    created.add("plots/simulation_contact_sheet.jpg")
    for name in PLOT_FILES:
        created.add(f"plots/{name}")
    manifest["created"] = sorted(created)
    manifest["plot_artifacts"] = sorted(item for item in created if item.startswith("plots/") or item == "strategy_simulation_plot_report.md")
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    parent_manifest = out.parent / "manifest.json"
    if out.parent.name == "quant_speculation" and parent_manifest.exists():
        parent = json.loads(parent_manifest.read_text(encoding="utf-8"))
        parent_created = set(parent.get("created", []))
        for item in manifest["created"]:
            parent_created.add(f"{out.name}/{item}")
        parent["created"] = sorted(parent_created)
        parent["strategy_simulation"] = {
            "paths_per_horizon": manifest.get("paths_per_horizon"),
            "strategy_evaluations": manifest.get("strategy_evaluations"),
            "elapsed_seconds": manifest.get("elapsed_seconds"),
            "report_dir": out.name,
        }
        parent_manifest.write_text(json.dumps(parent, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", default="artifacts/reports/quant_speculation/cases.json", help="Case JSON input")
    parser.add_argument("--out-dir", default="artifacts/reports/quant_speculation/strategy_simulation")
    parser.add_argument("--as-of", default="2026-04-10T12:00:00Z")
    parser.add_argument("--paths-per-horizon", type=int, default=10_000_000)
    parser.add_argument("--chunk-size", type=int, default=250_000)
    parser.add_argument("--quantity", type=float, default=25.0)
    parser.add_argument("--fee-rate", type=float, default=0.0125)
    parser.add_argument("--seed", type=int, default=20260410)
    parser.add_argument("--no-plots", action="store_true")
    args = parser.parse_args()

    cases = load_sim_cases(args.input if args.input else None)
    result = run_strategy_simulation(
        cases,
        as_of=args.as_of,
        paths_per_horizon=args.paths_per_horizon,
        chunk_size=args.chunk_size,
        quantity=args.quantity,
        fee_rate=args.fee_rate,
        seed=args.seed,
    )
    manifest = write_strategy_simulation_outputs(result, args.out_dir)
    if not args.no_plots:
        manifest = write_strategy_simulation_plots(result, args.out_dir)
    print(json.dumps({"out_dir": args.out_dir, **manifest}, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
