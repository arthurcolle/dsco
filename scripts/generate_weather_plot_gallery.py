#!/usr/bin/env python
"""Generate local weather calibration plot artifacts.

The plots are synthetic-but-realistic diagnostics intended for quick visual
review of calibration, fusion, and station-health workflows. The script owns
the full gallery so it can be run repeatedly from cron, launchd, or a watch
loop without leaving stale static plots behind.
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import time
from datetime import datetime, timezone
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.collections import LineCollection
from PIL import Image, ImageDraw, ImageStat


DEFAULT_OUT_DIR = Path("artifacts/plots")
OUT_DIR = DEFAULT_OUT_DIR
RNG = np.random.default_rng(42)

CORE_FILES = [
    "weather_calibration_cockpit.svg",
    "emos_crps_convergence.svg",
    "pit_histogram_reliability.svg",
    "station_coverage_heatmap.svg",
    "fusion_weights_dumbbell.svg",
]

PLOT_FILES = CORE_FILES

NOVEL_FILES = [
    "novel_ensemble_ridgeline.png",
    "novel_crps_loss_landscape.png",
    "novel_pit_phase_orbit.png",
    "novel_station_network.png",
    "novel_regime_fingerprint.png",
    "novel_drift_river.png",
]

ALL_PLOT_FILES = CORE_FILES + NOVEL_FILES
GALLERY_FILES = ["novel_contact_sheet.jpg", "index.html", "manifest.json"]


def _style() -> None:
    plt.rcParams.update(
        {
            "figure.facecolor": "#101111",
            "axes.facecolor": "#101111",
            "savefig.facecolor": "#101111",
            "text.color": "#f4f1e8",
            "axes.labelcolor": "#d7ddd7",
            "xtick.color": "#aab8b2",
            "ytick.color": "#aab8b2",
            "axes.edgecolor": "#4b5a56",
            "grid.color": "#26312e",
            "font.family": "DejaVu Sans",
            "font.size": 10,
            "axes.titleweight": "bold",
            "axes.titlesize": 12,
            "figure.titleweight": "bold",
        }
    )


def _save(fig: plt.Figure, filename: str, *, dpi: int = 170) -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT_DIR / filename, dpi=dpi, bbox_inches="tight", pad_inches=0.2)
    plt.close(fig)


def _normal_pdf(x: np.ndarray, mu: float, sigma: float) -> np.ndarray:
    z = (x - mu) / sigma
    return np.exp(-0.5 * z * z) / (sigma * math.sqrt(2.0 * math.pi))


def _smooth(values: np.ndarray, kernel_size: int = 5) -> np.ndarray:
    kernel = np.ones(kernel_size) / kernel_size
    padded = np.pad(values, (kernel_size // 2, kernel_size // 2), mode="edge")
    return np.convolve(padded, kernel, mode="valid")


def _heatmap(ax: plt.Axes, data: np.ndarray, x_labels: list[str], y_labels: list[str], title: str) -> None:
    im = ax.imshow(data, cmap="viridis", aspect="auto", vmin=np.nanmin(data), vmax=np.nanmax(data))
    ax.set_title(title)
    ax.set_xticks(range(len(x_labels)))
    ax.set_xticklabels(x_labels, rotation=35, ha="right")
    ax.set_yticks(range(len(y_labels)))
    ax.set_yticklabels(y_labels)
    ax.tick_params(length=0)
    for spine in ax.spines.values():
        spine.set_visible(False)
    cbar = ax.figure.colorbar(im, ax=ax, shrink=0.76, pad=0.018)
    cbar.outline.set_edgecolor("#4b5a56")


def plot_weather_calibration_cockpit(run_id: str) -> None:
    days = np.arange(42)
    raw_crps = 3.45 - 0.020 * days + 0.13 * np.sin(days / 4.2) + RNG.normal(0, 0.035, len(days))
    emos_crps = 2.82 - 0.030 * days + 0.07 * np.cos(days / 5.0) + RNG.normal(0, 0.026, len(days))
    pit_bins = np.linspace(0.05, 0.95, 10)
    pit_density = 1.0 + 0.20 * np.sin(np.linspace(0, 2 * math.pi, 10)) + RNG.normal(0, 0.05, 10)
    station_health = np.clip(0.72 + RNG.normal(0, 0.12, (5, 8)), 0.35, 0.99)
    weights = np.clip(
        np.vstack(
            [
                0.36 + 0.12 * np.sin(days / 7.5),
                0.25 + 0.08 * np.cos(days / 8.8),
                0.21 + 0.07 * np.sin(days / 5.5 + 1.0),
                0.18 + 0.05 * np.cos(days / 10.5 + 2.2),
            ]
        )
        + RNG.normal(0, 0.018, (4, len(days))),
        0.03,
        None,
    )
    weights = weights / weights.sum(axis=0, keepdims=True)

    fig = plt.figure(figsize=(14.8, 10.2))
    gs = fig.add_gridspec(3, 4, hspace=0.42, wspace=0.35)
    metric_ax = fig.add_subplot(gs[0, 0])
    crps_ax = fig.add_subplot(gs[0, 1:4])
    pit_ax = fig.add_subplot(gs[1, 0:2])
    weight_ax = fig.add_subplot(gs[1, 2:4])
    coverage_ax = fig.add_subplot(gs[2, :])

    fig.suptitle("Weather Calibration Cockpit", x=0.035, y=0.985, ha="left", fontsize=22)
    fig.text(0.035, 0.944, f"fresh synthetic diagnostic run {run_id}", color="#b9c5bf", fontsize=10)

    metric_ax.axis("off")
    cards = [
        ("CRPS delta", f"{raw_crps[-1] - emos_crps[-1]:+.2f}", "#36d1c4"),
        ("PIT skew", f"{pit_density[-2] - pit_density[1]:+.2f}", "#f5c84b"),
        ("Coverage", f"{100 * station_health.mean():.1f}%", "#8fd17f"),
        ("Drift alerts", str(int(np.sum(np.abs(np.diff(weights[0])) > 0.05))), "#ff6f61"),
    ]
    for idx, (label, value, color) in enumerate(cards):
        y = 0.86 - idx * 0.22
        metric_ax.text(0.02, y, label, color="#b9c5bf", fontsize=9, transform=metric_ax.transAxes)
        metric_ax.text(0.02, y - 0.11, value, color=color, fontsize=21, weight="bold", transform=metric_ax.transAxes)
        metric_ax.plot([0.02, 0.90], [y - 0.15, y - 0.15], color="#33423d", transform=metric_ax.transAxes)

    crps_ax.plot(days, raw_crps, label="raw blend", color="#ff6f61", linewidth=2.0)
    crps_ax.plot(days, emos_crps, label="EMOS", color="#36d1c4", linewidth=2.4)
    crps_ax.fill_between(days, emos_crps, raw_crps, where=raw_crps > emos_crps, color="#36d1c4", alpha=0.16)
    crps_ax.set_title("Rolling CRPS Compression")
    crps_ax.set_xlabel("verification day")
    crps_ax.set_ylabel("CRPS")
    crps_ax.legend(frameon=False, labelcolor="#f4f1e8")
    crps_ax.grid(alpha=0.45)
    crps_ax.spines[["top", "right"]].set_visible(False)

    pit_ax.bar(pit_bins, pit_density, width=0.075, color="#f5c84b", edgecolor="#101111", alpha=0.85)
    pit_ax.axhline(1.0, color="#f4f1e8", linestyle="--", linewidth=1.2, alpha=0.55)
    pit_ax.set_title("PIT Reliability Shape")
    pit_ax.set_xlabel("probability integral transform")
    pit_ax.set_ylabel("density ratio")
    pit_ax.grid(axis="y", alpha=0.35)
    pit_ax.spines[["top", "right"]].set_visible(False)

    weight_ax.stackplot(days, weights, colors=["#36d1c4", "#f5c84b", "#ff6f61", "#8fd17f"], labels=["HRRR", "GFS", "NAM", "NBM"], alpha=0.9)
    weight_ax.set_title("Fusion Weight Mix")
    weight_ax.set_xlabel("verification day")
    weight_ax.set_ylabel("normalized weight")
    weight_ax.set_ylim(0, 1)
    weight_ax.legend(frameon=False, ncol=4, loc="upper left", labelcolor="#f4f1e8")
    weight_ax.grid(axis="x", alpha=0.35)
    weight_ax.spines[["top", "right"]].set_visible(False)

    _heatmap(
        coverage_ax,
        station_health,
        ["KATL", "KAUS", "KDAL", "KDEN", "KMDW", "KMSY", "KPHX", "KSEA"],
        ["obs", "METAR", "ASOS", "NWP", "settle"],
        "Station Coverage Heatmap",
    )
    _save(fig, "weather_calibration_cockpit.svg")


def plot_emos_crps_convergence() -> None:
    epochs = np.arange(1, 81)
    baseline = 3.25 - 0.004 * epochs + 0.05 * np.sin(epochs / 8.0)
    blend = 2.95 - 0.009 * epochs + 0.06 * np.cos(epochs / 9.0) + RNG.normal(0, 0.012, len(epochs))
    emos = 2.84 - 0.016 * np.log1p(epochs) - 0.0045 * epochs + RNG.normal(0, 0.010, len(epochs))
    holdout = _smooth(emos + 0.09 + RNG.normal(0, 0.025, len(epochs)), 7)
    early_stop = int(np.argmin(holdout) + 1)

    fig, ax = plt.subplots(figsize=(12.4, 6.7))
    ax.plot(epochs, baseline, color="#8fd17f", linewidth=1.9, label="raw station climatology")
    ax.plot(epochs, blend, color="#f5c84b", linewidth=2.0, label="fusion blend")
    ax.plot(epochs, emos, color="#36d1c4", linewidth=2.5, label="EMOS train")
    ax.plot(epochs, holdout, color="#ff6f61", linewidth=2.2, label="EMOS holdout")
    ax.axvline(early_stop, color="#ffffff", linestyle="--", alpha=0.48)
    ax.scatter([early_stop], [holdout[early_stop - 1]], s=150, color="#ff6f61", edgecolor="#101111", zorder=5)
    ax.text(early_stop + 1.5, holdout[early_stop - 1] + 0.03, "best holdout", color="#ff6f61", weight="bold")
    ax.set_title("EMOS CRPS Convergence")
    ax.set_xlabel("optimization epoch")
    ax.set_ylabel("mean CRPS")
    ax.legend(frameon=False, labelcolor="#f4f1e8")
    ax.grid(alpha=0.42)
    ax.spines[["top", "right"]].set_visible(False)
    _save(fig, "emos_crps_convergence.svg")


def plot_pit_histogram_reliability() -> None:
    n = 1400
    calibrated = np.clip(RNG.beta(1.05, 1.04, n), 0.001, 0.999)
    warm_tail = np.clip(RNG.beta(1.8, 1.25, n // 4), 0.001, 0.999)
    pit = np.concatenate([calibrated, warm_tail])
    bins = np.linspace(0, 1, 21)
    hist, edges = np.histogram(pit, bins=bins, density=True)
    centers = 0.5 * (edges[:-1] + edges[1:])

    fig, ax = plt.subplots(figsize=(11.6, 6.8))
    ax.bar(centers, hist, width=0.044, color="#36d1c4", edgecolor="#101111", alpha=0.86)
    ax.plot(centers, _smooth(hist, 5), color="#f5c84b", linewidth=2.6, label="smoothed density")
    ax.axhline(1.0, color="#f4f1e8", linestyle="--", linewidth=1.2, alpha=0.6, label="ideal uniform")
    ax.fill_between(centers, 1.0, hist, where=hist > 1.0, color="#ff6f61", alpha=0.20)
    ax.set_title("PIT Histogram Reliability")
    ax.set_xlabel("PIT bin")
    ax.set_ylabel("density")
    ax.legend(frameon=False, labelcolor="#f4f1e8")
    ax.grid(axis="y", alpha=0.35)
    ax.spines[["top", "right"]].set_visible(False)
    _save(fig, "pit_histogram_reliability.svg")


def plot_station_coverage_heatmap() -> None:
    stations = ["KATL", "KAUS", "KDAL", "KDEN", "KMDW", "KMSY", "KOKC", "KPHX", "KSEA", "KSFO"]
    feeds = ["CLI", "ASOS", "METAR", "NWP", "settle", "quotes"]
    base = np.linspace(0.92, 0.72, len(feeds))[:, None] + RNG.normal(0, 0.055, (len(feeds), len(stations)))
    base[:, [2, 7]] += 0.035
    base[:, [4, 8]] -= 0.055
    data = np.clip(base, 0.48, 0.995)

    fig, ax = plt.subplots(figsize=(12.4, 6.4))
    _heatmap(ax, data, stations, feeds, "Station Coverage Heatmap")
    for y in range(data.shape[0]):
        for x in range(data.shape[1]):
            ax.text(x, y, f"{100 * data[y, x]:.0f}", ha="center", va="center", color="#101111" if data[y, x] > 0.77 else "#f4f1e8", fontsize=8)
    _save(fig, "station_coverage_heatmap.svg")


def plot_fusion_weights_dumbbell() -> None:
    stations = ["KATL", "KAUS", "KDAL", "KDEN", "KMDW", "KMSY", "KOKC", "KPHX", "KSEA", "KSFO"]
    old = np.clip(0.22 + 0.20 * RNG.random(len(stations)), 0.05, 0.60)
    new = np.clip(old + RNG.normal(0.06, 0.08, len(stations)), 0.04, 0.72)
    order = np.argsort(new - old)
    stations = [stations[i] for i in order]
    old = old[order]
    new = new[order]
    y = np.arange(len(stations))

    fig, ax = plt.subplots(figsize=(10.8, 7.0))
    for idx in range(len(stations)):
        color = "#36d1c4" if new[idx] >= old[idx] else "#ff6f61"
        ax.plot([old[idx], new[idx]], [y[idx], y[idx]], color=color, linewidth=3.8, alpha=0.78)
    ax.scatter(old, y, s=110, color="#7e8c86", edgecolor="#101111", label="previous")
    ax.scatter(new, y, s=140, color="#f5c84b", edgecolor="#101111", label="current")
    ax.set_yticks(y)
    ax.set_yticklabels(stations)
    ax.set_xlim(0, 0.8)
    ax.set_title("Fusion Weights Dumbbell")
    ax.set_xlabel("HRRR/NBM high-temperature blend weight")
    ax.legend(frameon=False, labelcolor="#f4f1e8")
    ax.grid(axis="x", alpha=0.35)
    ax.spines[["top", "right"]].set_visible(False)
    _save(fig, "fusion_weights_dumbbell.svg")


def plot_ensemble_ridgeline() -> None:
    x = np.linspace(52, 91, 260)
    leads = np.arange(0, 84, 6)
    colors = ["#36d1c4", "#f5c84b", "#ff6f61", "#8fd17f", "#b38cff", "#f28e2b"]

    fig, ax = plt.subplots(figsize=(12.8, 7.2))
    ax.set_title("Ensemble Plume Ridgeline")
    ax.text(52.1, len(leads) + 0.35, "Forecast high distribution by lead hour", color="#b9c5bf")

    observed = 75.8 + RNG.normal(0, 0.45)
    for i, lead in enumerate(leads):
        center = 62 + 0.18 * lead + 2.0 * math.sin(lead / 15.0) + RNG.normal(0, 0.20)
        spread = 2.4 + 0.055 * lead + RNG.normal(0, 0.04)
        secondary = center + 6.0 * math.sin((lead + 10) / 28.0)
        pdf = 0.74 * _normal_pdf(x, center, spread) + 0.26 * _normal_pdf(x, secondary, spread * 0.72)
        pdf = pdf / pdf.max() * 0.86
        y0 = i
        c = colors[i % len(colors)]
        ax.fill_between(x, y0, y0 + pdf, color=c, alpha=0.42, linewidth=0)
        ax.plot(x, y0 + pdf, color=c, linewidth=1.8)
        q10 = np.interp(0.10, np.cumsum(pdf) / np.sum(pdf), x)
        q90 = np.interp(0.90, np.cumsum(pdf) / np.sum(pdf), x)
        ax.plot([q10, q90], [y0 + 0.05, y0 + 0.05], color="#f4f1e8", alpha=0.62, linewidth=2.2)

    ax.axvline(observed, color="#ffffff", linewidth=2.4, alpha=0.9)
    ax.text(observed + 0.35, len(leads) - 0.2, "observed", rotation=90, va="top", color="#ffffff")
    ax.set_yticks(range(len(leads)))
    ax.set_yticklabels([f"+{h:02d}h" for h in leads])
    ax.set_xlabel("temperature F")
    ax.set_ylabel("lead hour")
    ax.grid(axis="x", alpha=0.55)
    ax.spines[["top", "right"]].set_visible(False)
    _save(fig, "novel_ensemble_ridgeline.png")


def plot_crps_loss_landscape() -> None:
    a = np.linspace(-8, 8, 160)
    b = np.linspace(0.72, 1.28, 150)
    aa, bb = np.meshgrid(a, b)
    bowl = 1.65 + 0.020 * (aa + 1.7) ** 2 + 16.0 * (bb - 1.035) ** 2
    ripple = 0.15 * np.sin(aa * 0.9) * np.cos((bb - 1.0) * 19)
    penalty = 0.18 * np.maximum(0, np.abs(aa) - 5) ** 1.5
    z = bowl + ripple + penalty
    path_t = np.linspace(0, 1, 18)
    path_a = -6.6 + 5.1 * (1 - np.exp(-3.4 * path_t)) + 0.22 * np.sin(path_t * 18)
    path_b = 1.24 - 0.205 * (1 - np.exp(-4.0 * path_t)) + 0.012 * np.cos(path_t * 16)

    fig, ax = plt.subplots(figsize=(11.8, 7.2))
    im = ax.contourf(aa, bb, z, levels=28, cmap="magma")
    cs = ax.contour(aa, bb, z, levels=10, colors="#f4f1e8", linewidths=0.45, alpha=0.35)
    ax.clabel(cs, inline=True, fontsize=7, fmt="%.1f")
    ax.plot(path_a, path_b, color="#30d5c8", linewidth=2.8, marker="o", markersize=4.2)
    ax.scatter([-1.7], [1.035], s=190, color="#f5c84b", edgecolor="#101111", linewidth=1.5, zorder=5)
    ax.text(-1.25, 1.047, "best basin", color="#f5c84b", weight="bold")
    ax.set_title("CRPS Loss Landscape")
    ax.set_xlabel("intercept a")
    ax.set_ylabel("slope b")
    cbar = fig.colorbar(im, ax=ax, shrink=0.84, pad=0.02)
    cbar.set_label("mean CRPS")
    ax.spines[["top", "right"]].set_visible(False)
    _save(fig, "novel_crps_loss_landscape.png")


def plot_pit_phase_orbit() -> None:
    n = 220
    t = np.linspace(0, 8 * math.pi, n)
    base = 0.5 + 0.38 * np.sin(t) * np.cos(t / 3)
    pit = np.clip(base + RNG.normal(0, 0.06, n), 0.01, 0.99)
    x = pit[:-1]
    y = pit[1:]
    pts = np.array([x, y]).T.reshape(-1, 1, 2)
    segs = np.concatenate([pts[:-1], pts[1:]], axis=1)

    fig, ax = plt.subplots(figsize=(8.4, 8.4))
    lc = LineCollection(segs, cmap="viridis", norm=plt.Normalize(0, len(segs)))
    lc.set_array(np.arange(len(segs)))
    lc.set_linewidth(1.25)
    lc.set_alpha(0.55)
    ax.add_collection(lc)
    sc = ax.scatter(x, y, c=np.arange(len(x)), cmap="plasma", s=24, edgecolor="#101111", linewidth=0.25)
    ax.plot([0, 1], [0, 1], color="#f4f1e8", linewidth=1.2, alpha=0.45)
    for val in [0.2, 0.4, 0.6, 0.8]:
        ax.axhline(val, color="#33423d", linewidth=0.8)
        ax.axvline(val, color="#33423d", linewidth=0.8)
    ax.scatter([0.5], [0.5], s=420, facecolors="none", edgecolors="#f5c84b", linewidth=2.2)
    ax.set_xlim(0, 1)
    ax.set_ylim(0, 1)
    ax.set_aspect("equal", adjustable="box")
    ax.set_title("PIT Phase Orbit")
    ax.set_xlabel("PIT day t")
    ax.set_ylabel("PIT day t+1")
    cbar = fig.colorbar(sc, ax=ax, shrink=0.74, pad=0.02)
    cbar.set_label("verification order")
    ax.spines[["top", "right"]].set_visible(False)
    _save(fig, "novel_pit_phase_orbit.png")


def plot_station_network() -> None:
    n = 34
    theta = RNG.uniform(0, 2 * math.pi, n)
    radius = np.sqrt(RNG.uniform(0.05, 1.0, n))
    lon = -73.9 + 3.1 * radius * np.cos(theta) + RNG.normal(0, 0.08, n)
    lat = 41.2 + 1.8 * radius * np.sin(theta) + RNG.normal(0, 0.05, n)
    health = np.clip(0.58 + 0.32 * np.sin(np.linspace(0, 4.2, n)) + RNG.normal(0, 0.10, n), 0.1, 0.98)
    obs_gap = np.clip(1.0 - health + RNG.normal(0, 0.08, n), 0, 1)

    fig, ax = plt.subplots(figsize=(11.4, 7.2))
    ax.set_title("Station Health Network")
    ax.text(lon.min(), lat.max() + 0.16, "Node size: missingness risk. Edge color: reliability contrast.", color="#b9c5bf")

    points = np.column_stack([lon, lat])
    d = np.sqrt(((points[:, None, :] - points[None, :, :]) ** 2).sum(axis=2))
    for i in range(n):
        neighbors = np.argsort(d[i])[1:4]
        for j in neighbors:
            if i < j:
                contrast = abs(health[i] - health[j])
                color = "#ff6f61" if contrast > 0.27 else "#36d1c4"
                alpha = min(0.92, 0.22 + contrast)
                ax.plot([lon[i], lon[j]], [lat[i], lat[j]], color=color, alpha=alpha, linewidth=0.8 + 3.0 * contrast)

    sizes = 90 + 520 * obs_gap
    sc = ax.scatter(lon, lat, c=health, s=sizes, cmap="RdYlGn", vmin=0, vmax=1, edgecolor="#f4f1e8", linewidth=0.75, zorder=3)
    for idx in np.argsort(obs_gap)[-5:]:
        ax.text(lon[idx] + 0.04, lat[idx] + 0.035, f"S{idx:02d}", fontsize=8, color="#f4f1e8")
    ax.set_xlabel("longitude")
    ax.set_ylabel("latitude")
    ax.grid(alpha=0.38)
    cbar = fig.colorbar(sc, ax=ax, shrink=0.82, pad=0.02)
    cbar.set_label("coverage health")
    ax.spines[["top", "right"]].set_visible(False)
    _save(fig, "novel_station_network.png")


def plot_regime_fingerprint() -> None:
    labels = ["bias", "spread", "tail", "lag", "blend", "coverage"]
    regimes = {
        "Heat Dome": [0.78, 0.44, 0.91, 0.35, 0.62, 0.70],
        "Marine Surge": [0.38, 0.74, 0.32, 0.83, 0.49, 0.58],
        "Frontal Passage": [0.58, 0.88, 0.67, 0.74, 0.80, 0.45],
        "Nocturnal Inversion": [0.84, 0.40, 0.54, 0.61, 0.36, 0.76],
    }
    colors = ["#ff6f61", "#36d1c4", "#f5c84b", "#8fd17f"]
    angles = np.linspace(0, 2 * math.pi, len(labels), endpoint=False)
    angles = np.concatenate([angles, angles[:1]])

    fig, axes = plt.subplots(2, 2, figsize=(10.6, 8.6), subplot_kw={"projection": "polar"})
    fig.suptitle("Regime Fingerprints", y=0.98, weight="bold", color="#f4f1e8")
    for ax, (name, values), color in zip(axes.flat, regimes.items(), colors):
        jittered = np.clip(np.array(values) + RNG.normal(0, 0.035, len(values)), 0.05, 0.98)
        vals = np.concatenate([jittered, jittered[:1]])
        ax.plot(angles, vals, color=color, linewidth=2.4)
        ax.fill(angles, vals, color=color, alpha=0.26)
        ax.set_title(name, y=1.10, color="#f4f1e8")
        ax.set_xticks(angles[:-1])
        ax.set_xticklabels(labels, color="#cfd8d2", fontsize=8)
        ax.set_yticks([0.25, 0.5, 0.75])
        ax.set_yticklabels([".25", ".50", ".75"], color="#97a7a1", fontsize=7)
        ax.set_ylim(0, 1)
        ax.grid(color="#394641", alpha=0.8)
        ax.spines["polar"].set_color("#586a63")
    _save(fig, "novel_regime_fingerprint.png")


def plot_drift_river() -> None:
    days = np.arange(45)
    hrrr = 0.30 + 0.10 * np.sin(days / 5.0) + 0.04 * RNG.normal(size=len(days))
    gfs = 0.22 + 0.08 * np.cos(days / 6.0 + 1.0) + 0.03 * RNG.normal(size=len(days))
    nam = 0.18 + 0.08 * np.sin(days / 7.5 + 2.0) + 0.035 * RNG.normal(size=len(days))
    nbm = 0.16 + 0.05 * np.cos(days / 9.0) + 0.025 * RNG.normal(size=len(days))
    blend = np.vstack([hrrr, gfs, nam, nbm])
    blend = np.clip(blend, 0.03, None)
    blend = blend / blend.sum(axis=0, keepdims=True)
    drift = 0.18 + 0.03 * days + 0.45 * np.maximum(0, np.sin((days - 22) / 5.0))

    fig, ax1 = plt.subplots(figsize=(12.8, 7.0))
    colors = ["#36d1c4", "#f5c84b", "#ff6f61", "#8fd17f"]
    ax1.stackplot(days, blend, labels=["HRRR", "GFS", "NAM", "NBM"], colors=colors, alpha=0.88)
    ax1.set_title("Fusion Weight River With Drift Alerts")
    ax1.set_xlabel("verification day")
    ax1.set_ylabel("normalized model weight")
    ax1.set_ylim(0, 1)
    ax1.legend(loc="upper left", ncol=4, frameon=False, labelcolor="#f4f1e8")
    ax1.grid(axis="x", alpha=0.35)

    ax2 = ax1.twinx()
    ax2.plot(days, drift, color="#ffffff", linewidth=2.5)
    ax2.fill_between(days, 0.8, drift, where=drift > 0.8, color="#ff6f61", alpha=0.20)
    ax2.axhline(0.8, color="#ffffff", alpha=0.35, linestyle="--", linewidth=1.1)
    for xval, yval in zip(days[drift > 0.8], drift[drift > 0.8]):
        ax2.scatter([xval], [yval], s=46, color="#ff6f61", edgecolor="#101111", linewidth=0.6, zorder=4)
    ax2.set_ylabel("drift score")
    ax2.set_ylim(0, max(1.25, float(drift.max() + 0.2)))
    ax2.tick_params(colors="#d7ddd7")
    ax1.spines[["top"]].set_visible(False)
    ax2.spines[["top"]].set_visible(False)
    _save(fig, "novel_drift_river.png")


def make_contact_sheet() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    images = [Image.open(OUT_DIR / name).convert("RGB") for name in NOVEL_FILES]
    thumb_w, thumb_h = 620, 385
    margin = 28
    title_h = 58
    sheet = Image.new("RGB", (2 * thumb_w + 3 * margin, 3 * (thumb_h + title_h) + 4 * margin), "#101111")
    draw = ImageDraw.Draw(sheet)
    for idx, (img, name) in enumerate(zip(images, NOVEL_FILES)):
        img.thumbnail((thumb_w, thumb_h), Image.Resampling.LANCZOS)
        x = margin + (idx % 2) * (thumb_w + margin)
        y = margin + (idx // 2) * (thumb_h + title_h + margin)
        draw.rectangle([x - 1, y - 1, x + thumb_w + 1, y + thumb_h + title_h + 1], outline="#3d4c47", width=2)
        sheet.paste(img, (x + (thumb_w - img.width) // 2, y + title_h + (thumb_h - img.height) // 2))
        draw.text((x + 14, y + 17), _display_title(name), fill="#f4f1e8")
    sheet.save(OUT_DIR / "novel_contact_sheet.jpg", quality=88, optimize=True)


def _display_title(name: str) -> str:
    stem = name
    for prefix in ("novel_",):
        stem = stem.removeprefix(prefix)
    stem = stem.removesuffix(".png").removesuffix(".jpg").removesuffix(".svg")
    words = []
    for word in stem.split("_"):
        upper = word.upper()
        words.append(upper if upper in {"CRPS", "PIT", "EMOS", "HRRR", "GFS", "NAM", "NBM"} else word.title())
    return " ".join(words)


def write_gallery(*, seed: int, run_id: str, generated_at: str, archive_path: str | None) -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    all_existing = [name for name in CORE_FILES if (OUT_DIR / name).exists()]
    novel_existing = [name for name in NOVEL_FILES if (OUT_DIR / name).exists()]
    html = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Weather Calibration Plots</title>
<style>
:root{{color-scheme:dark;--bg:#101111;--ink:#f4f1e8;--muted:#b9c5bf;--line:#33423d;--cyan:#36d1c4;--gold:#f5c84b;--coral:#ff6f61;--green:#8fd17f}}
*{{box-sizing:border-box}}
body{{margin:0;background:var(--bg);color:var(--ink);font-family:Inter,Arial,sans-serif}}
main{{max-width:1500px;margin:0 auto;padding:28px}}
h1,h2,p{{margin:0}}
h1{{font-size:clamp(32px,5vw,68px);line-height:1.02;max-width:980px}}
h2{{font-size:22px;margin-top:34px;margin-bottom:14px}}
p{{color:var(--muted);font-size:16px;line-height:1.5;max-width:880px;margin-top:12px}}
.hero{{display:grid;grid-template-columns:minmax(0,1fr);gap:18px;margin-bottom:20px}}
.accent{{width:120px;height:6px;background:linear-gradient(90deg,var(--cyan),var(--gold),var(--coral),var(--green));border-radius:6px}}
.meta{{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px}}
.pill{{border:1px solid var(--line);border-radius:8px;padding:7px 10px;color:var(--muted);font-size:13px;background:#151817}}
.grid{{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px}}
.wide{{grid-column:1/-1}}
figure{{margin:0;border:1px solid var(--line);border-radius:8px;overflow:hidden;background:#151817}}
img{{width:100%;height:auto;display:block}}
figcaption{{padding:10px 12px;color:var(--muted);font-size:14px;border-top:1px solid var(--line)}}
@media(max-width:900px){{main{{padding:18px}}.grid{{grid-template-columns:1fr}}}}
</style>
</head>
<body><main>
<section class="hero">
<div class="accent"></div>
<h1>Weather calibration image gallery</h1>
<p>Fresh calibration cockpit visuals plus experimental diagnostics for model fusion, PIT behavior, station coverage, and drift monitoring.</p>
<div class="meta">
<span class="pill">run {run_id}</span>
<span class="pill">seed {seed}</span>
<span class="pill">generated {generated_at}</span>
</div>
</section>
"""
    if "weather_calibration_cockpit.svg" in all_existing:
        html += '<figure class="wide"><img src="weather_calibration_cockpit.svg" alt="Weather calibration cockpit"><figcaption>Weather calibration cockpit</figcaption></figure>\n'
    html += "<h2>Novel diagnostics</h2>\n<div class=\"grid\">\n"
    for name in novel_existing:
        title = _display_title(name)
        html += f'<figure><img src="{name}" alt="{title}"><figcaption>{title}</figcaption></figure>\n'
    html += "</div>\n<h2>Core diagnostics</h2>\n<div class=\"grid\">\n"
    for name in all_existing:
        if name == "weather_calibration_cockpit.svg":
            continue
        title = _display_title(name)
        html += f'<figure><img src="{name}" alt="{title}"><figcaption>{title}</figcaption></figure>\n'
    html += "</div>\n</main></body></html>\n"
    (OUT_DIR / "index.html").write_text(html, encoding="utf-8")

    created = sorted({*all_existing, *novel_existing, *GALLERY_FILES})
    manifest = {
        "generated_at": generated_at,
        "run_id": run_id,
        "seed": seed,
        "created": created,
        "provenance": "synthetic_showcase",
        "note": "Generated demo diagnostics for calibration and ingest workflows; not a claim of live forecast verification.",
        "core_set": all_existing,
        "novel_set": novel_existing,
        "contact_sheet": "novel_contact_sheet.jpg",
        "archive_path": archive_path,
        "frequent_generation": {
            "single_refresh": "scripts/refresh_weather_plot_gallery.sh",
            "loop_example": "scripts/refresh_weather_plot_gallery.sh --loop --interval-seconds 900",
        },
    }
    (OUT_DIR / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def validate_outputs() -> dict[str, dict[str, object]]:
    report: dict[str, dict[str, object]] = {}
    required = ALL_PLOT_FILES + GALLERY_FILES
    for name in required:
        path = OUT_DIR / name
        if not path.exists():
            raise FileNotFoundError(f"missing plot artifact: {path}")
        size = path.stat().st_size
        min_size = 2_000 if path.suffix == ".svg" else 20_000
        if name in {"index.html", "manifest.json"}:
            min_size = 500
        if size < min_size:
            raise ValueError(f"plot artifact is unexpectedly small: {path} ({size} bytes)")
        info: dict[str, object] = {"bytes": size}
        if path.suffix.lower() in {".png", ".jpg", ".jpeg"}:
            with Image.open(path) as img:
                width, height = img.size
                stat = ImageStat.Stat(img.convert("RGB"))
            if width < 800 or height < 600:
                raise ValueError(f"raster plot is too small: {path} ({width}x{height})")
            max_stddev = max(stat.stddev)
            if max_stddev < 8.0:
                raise ValueError(f"raster plot appears blank or nearly blank: {path} (stddev {max_stddev:.2f})")
            info.update({"width": width, "height": height, "max_channel_stddev": round(max_stddev, 2)})
        report[name] = info
    return report


def archive_outputs(archive_root: Path, run_id: str) -> Path:
    archive_dir = archive_root / run_id
    archive_dir.mkdir(parents=True, exist_ok=True)
    for name in ALL_PLOT_FILES + GALLERY_FILES:
        shutil.copy2(OUT_DIR / name, archive_dir / name)
    return archive_dir


def generate_once(*, out_dir: Path, seed: int, archive_root: Path | None) -> dict[str, object]:
    global OUT_DIR, RNG

    OUT_DIR = out_dir
    RNG = np.random.default_rng(seed)
    generated_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
    run_id = datetime.now().strftime("%Y%m%d-%H%M%S")
    _style()
    plot_weather_calibration_cockpit(run_id)
    plot_emos_crps_convergence()
    plot_pit_histogram_reliability()
    plot_station_coverage_heatmap()
    plot_fusion_weights_dumbbell()
    plot_ensemble_ridgeline()
    plot_crps_loss_landscape()
    plot_pit_phase_orbit()
    plot_station_network()
    plot_regime_fingerprint()
    plot_drift_river()
    make_contact_sheet()

    archive_path = str(archive_root / run_id) if archive_root else None
    write_gallery(seed=seed, run_id=run_id, generated_at=generated_at, archive_path=archive_path)
    quality = validate_outputs()
    if archive_root:
        archive_path = str(archive_outputs(archive_root, run_id))
        write_gallery(seed=seed, run_id=run_id, generated_at=generated_at, archive_path=archive_path)
        quality = validate_outputs()
        shutil.copy2(OUT_DIR / "manifest.json", Path(archive_path) / "manifest.json")
        shutil.copy2(OUT_DIR / "index.html", Path(archive_path) / "index.html")
    return {
        "out_dir": str(OUT_DIR),
        "run_id": run_id,
        "seed": seed,
        "archive_path": archive_path,
        "created": ALL_PLOT_FILES + GALLERY_FILES,
        "quality": quality,
    }


def _seed_from_clock() -> int:
    return int(time.time_ns() % (2**32 - 1))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", default=str(DEFAULT_OUT_DIR), help="Directory to write the current gallery into.")
    parser.add_argument("--seed", type=int, default=42, help="Deterministic random seed for reproducible visuals.")
    parser.add_argument("--fresh", action="store_true", help="Use a clock-derived seed so each run produces a fresh variant.")
    parser.add_argument("--archive-root", default="", help="Optional directory for timestamped copies of every run.")
    parser.add_argument("--loop", action="store_true", help="Regenerate on a fixed interval until interrupted.")
    parser.add_argument("--interval-seconds", type=float, default=900.0, help="Delay between looped generations.")
    parser.add_argument("--max-runs", type=int, default=0, help="Stop after this many loop iterations; 0 means unlimited.")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    out_dir = Path(args.out_dir)
    archive_root = Path(args.archive_root) if args.archive_root else None
    runs = 0
    summaries = []

    while True:
        seed = _seed_from_clock() if args.fresh else args.seed + runs
        summary = generate_once(out_dir=out_dir, seed=seed, archive_root=archive_root)
        print(json.dumps(summary, indent=2))
        summaries.append(summary)
        runs += 1
        if not args.loop or (args.max_runs and runs >= args.max_runs):
            break
        time.sleep(max(args.interval_seconds, 1.0))

    if len(summaries) > 1:
        print(json.dumps({"completed_runs": len(summaries), "last_run": summaries[-1]["run_id"]}, indent=2))


if __name__ == "__main__":
    main()
