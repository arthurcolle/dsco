#!/usr/bin/env python3
"""Generate advanced plots for the quantitative speculation report bundle."""

from __future__ import annotations

import argparse
import json
import math
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


PLOT_FILES = [
    "advanced_dashboard.png",
    "monte_carlo_distribution.png",
    "correlation_allocation_matrix.png",
    "model_market_disagreement.png",
    "weather_threshold_sensitivity.png",
    "execution_risk_map.png",
    "scenario_tornado.png",
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


def load_bundle(path: str | Path) -> dict[str, Any]:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_advanced_plots(bundle: dict[str, Any], report_dir: str | Path) -> dict[str, Any]:
    _style()
    report_path = Path(report_dir)
    plot_dir = report_path / "plots"
    plot_dir.mkdir(parents=True, exist_ok=True)

    plot_dashboard(bundle, plot_dir)
    plot_monte_carlo_distribution(bundle, plot_dir)
    plot_correlation_allocation(bundle, plot_dir)
    plot_model_market_disagreement(bundle, plot_dir)
    plot_weather_threshold_sensitivity(bundle, plot_dir)
    plot_execution_risk_map(bundle, plot_dir)
    plot_scenario_tornado(bundle, plot_dir)
    make_contact_sheet(plot_dir)
    write_html_index(bundle, plot_dir)
    write_markdown_report(bundle, report_path)
    update_report_index(report_path)
    manifest = update_manifest(report_path)
    return manifest


def _save(fig: plt.Figure, plot_dir: Path, filename: str) -> None:
    fig.savefig(plot_dir / filename, dpi=170, bbox_inches="tight", pad_inches=0.18)
    plt.close(fig)


def _short(label: str) -> str:
    label = label.replace("KX", "")
    parts = label.split("-")
    if len(parts) >= 3:
        return f"{parts[0]} {parts[-1]}"
    return label[:18]


def _fmt_pct(value: float) -> str:
    return f"{100.0 * value:.1f}%"


def _fmt_dollars(value: float) -> str:
    return f"${value:.2f}"


def _fmt_plot_dollars(value: float) -> str:
    return f"USD {value:.2f}"


def _bar_colors(values: Sequence[float]) -> list[str]:
    return [COLORS["cyan"] if value >= 0 else COLORS["coral"] for value in values]


def _case_by_market(bundle: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {case["market"]: case for case in bundle.get("cases", [])}


def _normal_cdf(x: np.ndarray) -> np.ndarray:
    return 0.5 * (1.0 + np.vectorize(math.erf)(x / np.sqrt(2.0)))


def plot_dashboard(bundle: dict[str, Any], plot_dir: Path) -> None:
    reports = bundle["reports"]
    fig = plt.figure(figsize=(18, 11), constrained_layout=True)
    gs = fig.add_gridspec(2, 3)
    fig.suptitle("Advanced Quantitative Speculation Dashboard", fontsize=22, weight="bold")
    fig.text(0.01, 0.965, f"As of {bundle['as_of']} | synthetic sample unless regenerated with --input", color=COLORS["muted"])

    edge_rows = reports["edge_screener"]["rows"][:7]
    ax = fig.add_subplot(gs[0, 0])
    labels = [_short(row["market"]) for row in edge_rows][::-1]
    net_edges = [row["net_edge"] for row in edge_rows][::-1]
    ax.barh(labels, net_edges, color=_bar_colors(net_edges), alpha=0.92)
    ax.axvline(0, color=COLORS["ink"], alpha=0.45, linewidth=1)
    ax.set_title("Net Edge Rank")
    ax.set_xlabel("net edge")
    ax.xaxis.set_major_formatter(lambda x, _: _fmt_pct(x))
    ax.grid(axis="x", alpha=0.45)

    ax = fig.add_subplot(gs[0, 1])
    hist = reports["monte_carlo"].get("histogram", [])
    mids = [item["mid"] for item in hist]
    counts = [item["count"] for item in hist]
    widths = [(item["hi"] - item["lo"]) * 0.92 for item in hist]
    ax.bar(mids, counts, width=widths, color=COLORS["violet"], alpha=0.76)
    for key, color in [("p05_pnl", COLORS["coral"]), ("median_pnl", COLORS["ink"]), ("p95_pnl", COLORS["green"])]:
        ax.axvline(reports["monte_carlo"]["summary"][key], color=color, linewidth=2)
    ax.set_title("Monte Carlo PnL Distribution")
    ax.set_xlabel("portfolio PnL")
    ax.set_ylabel("iterations")
    ax.grid(axis="y", alpha=0.35)

    ax = fig.add_subplot(gs[0, 2])
    alloc = reports["correlation_allocation"]["rows"]
    x = [row["net_edge"] for row in alloc]
    y = [row["allocation_contracts"] for row in alloc]
    sizes = [80 + 38 * row["liquidity_score"] for row in alloc]
    colors = [row["corr_penalty"] for row in alloc]
    sc = ax.scatter(x, y, s=sizes, c=colors, cmap="magma", edgecolor=COLORS["ink"], linewidth=0.5, alpha=0.9)
    for row in alloc[:5]:
        ax.text(row["net_edge"] + 0.002, row["allocation_contracts"] + 0.25, _short(row["market"]), fontsize=8)
    ax.set_title("Allocation By Edge And Correlation")
    ax.set_xlabel("net edge")
    ax.set_ylabel("contracts")
    ax.xaxis.set_major_formatter(lambda val, _: _fmt_pct(val))
    fig.colorbar(sc, ax=ax, shrink=0.8, label="corr penalty")
    ax.grid(alpha=0.35)

    ax = fig.add_subplot(gs[1, 0])
    zrows = reports["market_disagreement"]["rows"]
    labels = [_short(row["market"]) for row in zrows][::-1]
    zvals = [row["z_score"] for row in zrows][::-1]
    ax.barh(labels, zvals, color=_bar_colors(zvals), alpha=0.9)
    ax.axvline(0, color=COLORS["ink"], alpha=0.45)
    ax.axvline(1.5, color=COLORS["gold"], linestyle="--", alpha=0.8)
    ax.axvline(-1.5, color=COLORS["gold"], linestyle="--", alpha=0.8)
    ax.set_title("Model-Market Disagreement Z")
    ax.set_xlabel("z-score")
    ax.grid(axis="x", alpha=0.35)

    ax = fig.add_subplot(gs[1, 1])
    sens = reports["weather_sensitivity"]["rows"]
    x = [row["pin_risk_2f"] for row in sens]
    y = [abs(row["market_delta_value"]) for row in sens]
    sc = ax.scatter(x, y, c=[row["delta_per_1f"] for row in sens], s=[90 + 500 * abs(row["delta_per_1f"]) for row in sens], cmap="coolwarm", edgecolor=COLORS["ink"], linewidth=0.5)
    for row in sens:
        if row["pin_risk_2f"] > 0.12 or abs(row["market_delta_value"]) > 2:
            ax.text(row["pin_risk_2f"] + 0.004, abs(row["market_delta_value"]) + 0.05, _short(row["market"]), fontsize=8)
    ax.set_title("Threshold Pin Risk Vs Dollar Delta")
    ax.set_xlabel("2F pin risk")
    ax.set_ylabel("absolute dollar delta")
    ax.xaxis.set_major_formatter(lambda val, _: _fmt_pct(val))
    fig.colorbar(sc, ax=ax, shrink=0.8, label="probability delta / 1F")
    ax.grid(alpha=0.35)

    ax = fig.add_subplot(gs[1, 2])
    rows = reports["scenario_ladder"]["rows"] + reports["tail_risk"]["rows"]
    names = [row["scenario"] for row in rows]
    pnl = [row.get("portfolio_expected_pnl", row.get("portfolio_pnl", 0.0)) for row in rows]
    order = np.argsort(pnl)
    ax.barh([names[i] for i in order], [pnl[i] for i in order], color=_bar_colors([pnl[i] for i in order]), alpha=0.86)
    ax.axvline(0, color=COLORS["ink"], alpha=0.5)
    ax.set_title("Scenario PnL Ladder")
    ax.set_xlabel("portfolio PnL")
    ax.grid(axis="x", alpha=0.35)

    _save(fig, plot_dir, "advanced_dashboard.png")


def plot_monte_carlo_distribution(bundle: dict[str, Any], plot_dir: Path) -> None:
    report = bundle["reports"]["monte_carlo"]
    hist = report.get("histogram", [])
    fig, ax = plt.subplots(figsize=(12.8, 7.2))
    mids = [item["mid"] for item in hist]
    counts = [item["count"] for item in hist]
    widths = [(item["hi"] - item["lo"]) * 0.92 for item in hist]
    colors = [COLORS["coral"] if mid < 0 else COLORS["cyan"] for mid in mids]
    ax.bar(mids, counts, width=widths, color=colors, alpha=0.82)
    summary = report["summary"]
    for label, key, color in [
        ("p05", "p05_pnl", COLORS["coral"]),
        ("median", "median_pnl", COLORS["ink"]),
        ("p95", "p95_pnl", COLORS["green"]),
    ]:
        value = summary[key]
        ax.axvline(value, color=color, linewidth=2.2)
        ax.text(value, max(counts) * 0.92, label, rotation=90, va="top", color=color, weight="bold")
    ax.set_title("Monte Carlo Portfolio PnL Distribution")
    ax.text(min(mids), max(counts) * 1.06, f"Mean {_fmt_plot_dollars(summary['mean_pnl'])} | loss probability {_fmt_pct(summary['loss_probability'])} | ES5 {_fmt_plot_dollars(summary['expected_shortfall_5'])}", color=COLORS["muted"])
    ax.set_xlabel("portfolio PnL")
    ax.set_ylabel("iterations")
    ax.grid(axis="y", alpha=0.38)
    ax.spines[["top", "right"]].set_visible(False)
    _save(fig, plot_dir, "monte_carlo_distribution.png")


def plot_correlation_allocation(bundle: dict[str, Any], plot_dir: Path) -> None:
    report = bundle["reports"]["correlation_allocation"]
    rows = report["rows"]
    matrix = report["correlation_matrix"]
    labels = [row["market"] for row in rows]
    data = np.array([[matrix[left][right] for right in labels] for left in labels])

    fig, (ax0, ax1) = plt.subplots(1, 2, figsize=(15.5, 7.2), gridspec_kw={"width_ratios": [1.05, 0.95]})
    im = ax0.imshow(data, cmap="coolwarm", vmin=-1, vmax=1)
    ax0.set_title("Inferred Trade Correlation Matrix")
    ax0.set_xticks(range(len(labels)), [_short(label) for label in labels], rotation=45, ha="right")
    ax0.set_yticks(range(len(labels)), [_short(label) for label in labels])
    for i in range(len(labels)):
        for j in range(len(labels)):
            ax0.text(j, i, f"{data[i, j]:.2f}", ha="center", va="center", fontsize=8, color=COLORS["ink"])
    fig.colorbar(im, ax=ax0, shrink=0.8)

    y = np.arange(len(rows))
    allocs = [row["allocation_contracts"] for row in rows]
    expected = [row["expected_pnl"] for row in rows]
    ax1.barh(y, allocs, color=COLORS["cyan"], alpha=0.85, label="allocation")
    ax1.scatter(expected, y, color=COLORS["gold"], s=80, label="expected PnL")
    ax1.set_yticks(y, [_short(row["market"]) for row in rows])
    ax1.invert_yaxis()
    ax1.set_title("Correlation-Aware Sizing")
    ax1.set_xlabel("contracts / expected PnL")
    ax1.grid(axis="x", alpha=0.35)
    ax1.legend(frameon=False, labelcolor=COLORS["ink"])
    _save(fig, plot_dir, "correlation_allocation_matrix.png")


def plot_model_market_disagreement(bundle: dict[str, Any], plot_dir: Path) -> None:
    rows = bundle["reports"]["market_disagreement"]["rows"]
    fig, ax = plt.subplots(figsize=(12.8, 7.2))
    y = np.arange(len(rows))
    for idx, row in enumerate(rows):
        ax.plot([row["market_mid"], row["model_fair"]], [idx, idx], color=COLORS["grid"], linewidth=6, solid_capstyle="round")
        ax.scatter(row["market_mid"], idx, color=COLORS["gold"], s=100, label="market mid" if idx == 0 else "")
        ax.scatter(row["model_fair"], idx, color=COLORS["cyan"], s=100, label="model fair" if idx == 0 else "")
        ax.scatter(row["posterior_prob"], idx, color=COLORS["ink"], s=46, label="posterior" if idx == 0 else "")
        ax.text(max(row["market_mid"], row["model_fair"]) + 0.015, idx, f"z={row['z_score']:.2f}", va="center", fontsize=9)
    ax.set_yticks(y, [_short(row["market"]) for row in rows])
    ax.invert_yaxis()
    ax.set_title("Model Fair Vs Market Midpoint Vs Posterior")
    ax.set_xlabel("probability")
    ax.xaxis.set_major_formatter(lambda val, _: _fmt_pct(val))
    ax.grid(axis="x", alpha=0.35)
    ax.legend(frameon=False, labelcolor=COLORS["ink"], loc="lower right")
    ax.spines[["top", "right"]].set_visible(False)
    _save(fig, plot_dir, "model_market_disagreement.png")


def plot_weather_threshold_sensitivity(bundle: dict[str, Any], plot_dir: Path) -> None:
    rows = [
        row for row in bundle["reports"]["weather_sensitivity"]["rows"]
        if row["threshold"] is not None and row["inferred_mean_f"] is not None
    ]
    fig, ax = plt.subplots(figsize=(12.8, 7.2))
    shifts = np.linspace(-5, 5, 121)
    palette = [COLORS["cyan"], COLORS["gold"], COLORS["coral"], COLORS["green"], COLORS["violet"]]
    for idx, row in enumerate(rows):
        mean = row["inferred_mean_f"]
        sigma = max(0.75, row["sigma_f"])
        threshold = row["threshold"]
        if row["family"] == "low_temp":
            probs = _normal_cdf((threshold - (mean + shifts)) / sigma)
        else:
            probs = 1.0 - _normal_cdf((threshold - (mean + shifts)) / sigma)
        ax.plot(shifts, probs, color=palette[idx % len(palette)], linewidth=2.3, label=_short(row["market"]))
        ax.scatter([0], [row["fair_prob"]], color=palette[idx % len(palette)], s=45)
    ax.axvline(0, color=COLORS["ink"], alpha=0.45)
    ax.set_title("Probability Sensitivity To Forecast Temperature Shift")
    ax.set_xlabel("forecast mean shift, F")
    ax.set_ylabel("contract probability")
    ax.yaxis.set_major_formatter(lambda val, _: _fmt_pct(val))
    ax.grid(alpha=0.35)
    ax.legend(frameon=False, labelcolor=COLORS["ink"], ncol=2)
    ax.spines[["top", "right"]].set_visible(False)
    _save(fig, plot_dir, "weather_threshold_sensitivity.png")


def plot_execution_risk_map(bundle: dict[str, Any], plot_dir: Path) -> None:
    rows = bundle["reports"]["liquidity_execution"]["rows"]
    fig, ax = plt.subplots(figsize=(12.8, 7.2))
    x = [row["spread"] for row in rows]
    y = [row["passive_fill_probability"] for row in rows]
    sizes = [80 + 28 * row["tail_risk_budget"] for row in rows]
    colors = [row["volatility_throttle"] for row in rows]
    sc = ax.scatter(x, y, s=sizes, c=colors, cmap="viridis", edgecolor=COLORS["ink"], linewidth=0.55, alpha=0.92)
    for row in rows:
        ax.text(row["spread"] + 0.002, row["passive_fill_probability"] + 0.001, _short(row["market"]), fontsize=8)
    ax.set_title("Execution Risk Map")
    ax.set_xlabel("spread")
    ax.set_ylabel("passive fill probability")
    ax.xaxis.set_major_formatter(lambda val, _: _fmt_pct(val))
    ax.yaxis.set_major_formatter(lambda val, _: _fmt_pct(val))
    ax.grid(alpha=0.35)
    fig.colorbar(sc, ax=ax, shrink=0.82, label="volatility throttle")
    ax.spines[["top", "right"]].set_visible(False)
    _save(fig, plot_dir, "execution_risk_map.png")


def plot_scenario_tornado(bundle: dict[str, Any], plot_dir: Path) -> None:
    scenario = bundle["reports"]["scenario_ladder"]["rows"]
    stress = bundle["reports"]["tail_risk"]["rows"]
    rows = [
        {"name": row["scenario"], "pnl": row["portfolio_expected_pnl"], "group": "ladder"}
        for row in scenario
    ] + [
        {"name": row["scenario"], "pnl": row["portfolio_pnl"], "group": "stress"}
        for row in stress
    ]
    rows.sort(key=lambda row: row["pnl"])
    fig, ax = plt.subplots(figsize=(12.8, 7.2))
    vals = [row["pnl"] for row in rows]
    ax.barh([row["name"] for row in rows], vals, color=_bar_colors(vals), alpha=0.88)
    ax.axvline(0, color=COLORS["ink"], alpha=0.45)
    for idx, row in enumerate(rows):
        ax.text(row["pnl"] + (0.25 if row["pnl"] >= 0 else -0.25), idx, _fmt_plot_dollars(row["pnl"]), va="center", ha="left" if row["pnl"] >= 0 else "right")
    ax.set_title("Scenario And Stress PnL Tornado")
    ax.set_xlabel("portfolio PnL")
    ax.grid(axis="x", alpha=0.35)
    ax.spines[["top", "right"]].set_visible(False)
    _save(fig, plot_dir, "scenario_tornado.png")


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
    sheet.save(plot_dir / "advanced_contact_sheet.jpg", quality=88, optimize=True)


def write_html_index(bundle: dict[str, Any], plot_dir: Path) -> None:
    figures = "\n".join(
        f'<figure><img src="{name}" alt="{name.removesuffix(".png").replace("_", " ")}"><figcaption>{name.removesuffix(".png").replace("_", " ").title()}</figcaption></figure>'
        for name in PLOT_FILES
    )
    html = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Advanced Quant Spec Plots</title>
<style>
:root{{color-scheme:dark;--bg:{COLORS['bg']};--ink:{COLORS['ink']};--muted:{COLORS['muted']};--line:#33423d}}
*{{box-sizing:border-box}}
body{{margin:0;background:var(--bg);color:var(--ink);font-family:Inter,Arial,sans-serif}}
main{{max-width:1500px;margin:0 auto;padding:28px}}
h1,p{{margin:0}}
h1{{font-size:clamp(32px,5vw,64px);line-height:1.02;max-width:980px}}
p{{margin-top:12px;color:var(--muted);line-height:1.5;max-width:760px}}
.grid{{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px;margin-top:28px}}
figure{{margin:0;border:1px solid var(--line);border-radius:8px;overflow:hidden;background:#151817}}
figure:first-child{{grid-column:1/-1}}
img{{width:100%;height:auto;display:block}}
figcaption{{padding:10px 12px;color:var(--muted);font-size:14px;border-top:1px solid var(--line)}}
@media(max-width:900px){{main{{padding:18px}}.grid{{grid-template-columns:1fr}}figure:first-child{{grid-column:auto}}}}
</style>
</head>
<body><main>
<h1>Advanced quant speculation plots</h1>
<p>Generated from bundle as of {bundle['as_of']}. Synthetic sample unless the bundle was regenerated with live or user-supplied cases.</p>
<div class="grid">
{figures}
</div>
</main></body></html>
"""
    (plot_dir / "index.html").write_text(html, encoding="utf-8")


def write_markdown_report(bundle: dict[str, Any], report_dir: Path) -> None:
    monte = bundle["reports"]["monte_carlo"]["summary"]
    allocation = bundle["reports"]["correlation_allocation"]["summary"]
    disagreement = bundle["reports"]["market_disagreement"]["summary"]
    sensitivity = bundle["reports"]["weather_sensitivity"]["summary"]
    text = f"""# Advanced Quantitative Plot Report

As of: `{bundle['as_of']}`

These plots are generated from the quantitative speculation bundle. The default data is synthetic and useful for workflow testing, model review, and analyst ideation; it is not live market data and is not trading advice.

## Key Readouts

- Monte Carlo mean PnL: `{_fmt_dollars(monte['mean_pnl'])}` with loss probability `{_fmt_pct(monte['loss_probability'])}` and ES5 `{_fmt_dollars(monte['expected_shortfall_5'])}`.
- Effective correlation-adjusted bets: `{allocation['effective_bets']}` with weighted correlation `{allocation['weighted_correlation']}`.
- Strongest model-market disagreement: `{disagreement['strongest_market']}` at z-score `{disagreement['strongest_z_score']}`.
- Highest threshold pin risk: `{sensitivity['highest_pin_market']}` at `{_fmt_pct(sensitivity['highest_pin_risk_2f'])}`.

## Plot Gallery

![Advanced dashboard](plots/advanced_dashboard.png)

![Monte Carlo distribution](plots/monte_carlo_distribution.png)

![Correlation allocation matrix](plots/correlation_allocation_matrix.png)

![Model market disagreement](plots/model_market_disagreement.png)

![Weather threshold sensitivity](plots/weather_threshold_sensitivity.png)

![Execution risk map](plots/execution_risk_map.png)

![Scenario tornado](plots/scenario_tornado.png)
"""
    (report_dir / "advanced_plot_report.md").write_text(text, encoding="utf-8")


def update_report_index(report_dir: Path) -> None:
    index_path = report_dir / "index.md"
    if not index_path.exists():
        return
    text = index_path.read_text(encoding="utf-8")
    section = (
        "\n\n## Advanced Plots\n\n"
        "- [Advanced plot report](advanced_plot_report.md)\n"
        "- [HTML plot gallery](plots/index.html)\n"
    )
    marker = "\n## Advanced Plots\n"
    if marker in text:
        text = text[: text.index(marker)].rstrip() + section
    else:
        text = text.rstrip() + section
    index_path.write_text(text + "\n", encoding="utf-8")


def update_manifest(report_dir: Path) -> dict[str, Any]:
    manifest_path = report_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
    created = set(manifest.get("created", []))
    created.add("advanced_plot_report.md")
    created.add("plots/index.html")
    created.add("plots/advanced_contact_sheet.jpg")
    for name in PLOT_FILES:
        created.add(f"plots/{name}")
    manifest["created"] = sorted(created)
    manifest["plot_artifacts"] = sorted(item for item in created if item.startswith("plots/") or item == "advanced_plot_report.md")
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", default="artifacts/reports/quant_speculation/bundle.json")
    parser.add_argument("--report-dir", default="artifacts/reports/quant_speculation")
    args = parser.parse_args()

    bundle = load_bundle(args.bundle)
    manifest = write_advanced_plots(bundle, args.report_dir)
    print(json.dumps({"report_dir": args.report_dir, "plot_count": len(PLOT_FILES), **manifest}, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
