#!/usr/bin/env python3
"""
Forecast audit for Kukulkan calibration data.

Computes model and ensemble error metrics from verification rows where
forecast highs and observed settlement highs are both available.
"""

from __future__ import annotations

import argparse
import math
import sqlite3
import sys
from collections import defaultdict
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from calibration import CALIB_DB, calibrate_city


MODEL_WEIGHTS = {
    "hrrr": 1.5,
    "nam3": 1.3,
    "rap": 1.0,
    "nam": 0.9,
    "gfs": 0.7,
    "sref": 1.2,
}


def _metrics(values: list[float]) -> tuple[float, float, float]:
    if not values:
        return float("nan"), float("nan"), float("nan")
    mae = sum(abs(v) for v in values) / len(values)
    bias = sum(v for v in values) / len(values)
    rmse = math.sqrt(sum(v * v for v in values) / len(values))
    return mae, bias, rmse


def _group_rows(conn: sqlite3.Connection, start: str, end: str) -> dict[tuple[str, str], dict]:
    rows = conn.execute(
        """
        SELECT city, date, model, fcst_high_f, obs_high_f
        FROM verification
        WHERE model != 'cli'
          AND fcst_high_f IS NOT NULL
          AND obs_high_f IS NOT NULL
          AND date >= ? AND date <= ?
        ORDER BY date, city, model
        """,
        (start, end),
    ).fetchall()

    grouped: dict[tuple[str, str], dict] = {}
    for city, date_str, model, fcst, obs in rows:
        key = (str(city), str(date_str))
        if key not in grouped:
            grouped[key] = {"obs": float(obs), "fcst": {}}
        grouped[key]["fcst"][str(model)] = float(fcst)
    return grouped


def main() -> None:
    parser = argparse.ArgumentParser(description="Audit forecast skill from verification DB")
    parser.add_argument("--start", help="Start date YYYY-MM-DD (default: earliest model row)")
    parser.add_argument("--end", help="End date YYYY-MM-DD (default: latest model row)")
    args = parser.parse_args()

    if not Path(CALIB_DB).exists():
        raise SystemExit(f"Calibration DB not found: {CALIB_DB}")

    conn = sqlite3.connect(str(CALIB_DB))
    min_max = conn.execute(
        "SELECT MIN(date), MAX(date) FROM verification WHERE model != 'cli' AND fcst_high_f IS NOT NULL"
    ).fetchone()
    if not min_max or min_max[0] is None:
        raise SystemExit("No model verification rows found.")

    start = args.start or min_max[0]
    end = args.end or min_max[1]

    # Validate date format early.
    datetime.strptime(start, "%Y-%m-%d")
    datetime.strptime(end, "%Y-%m-%d")

    grouped = _group_rows(conn, start, end)
    conn.close()
    if not grouped:
        raise SystemExit("No verification rows in requested date range.")

    err_by_name: dict[str, list[float]] = defaultdict(list)
    err_by_city_weighted: dict[str, list[float]] = defaultdict(list)

    for (city, _date), item in grouped.items():
        date_dt = datetime.strptime(_date, "%Y-%m-%d")
        obs = item["obs"]
        fcst: dict[str, float] = item["fcst"]
        models = sorted(fcst.keys())
        vals = [fcst[m] for m in models]

        for m, v in fcst.items():
            err_by_name[f"model:{m}"].append(v - obs)

        if vals:
            ens_mean = sum(vals) / len(vals)
            err_by_name["ens:mean"].append(ens_mean - obs)

            total_w = sum(MODEL_WEIGHTS.get(m, 1.0) for m in models)
            ens_weighted = sum(fcst[m] * MODEL_WEIGHTS.get(m, 1.0) for m in models) / max(total_w, 1e-9)
            err_by_name["ens:weighted"].append(ens_weighted - obs)
            err_by_city_weighted[city].append(ens_weighted - obs)

            if len(vals) >= 4:
                s = sorted(vals)
                trimmed = s[1:-1]
                ens_trimmed = sum(trimmed) / len(trimmed)
            else:
                ens_trimmed = ens_mean
            err_by_name["ens:trimmed"].append(ens_trimmed - obs)

            try:
                r = calibrate_city(
                    city,
                    {m: fcst[m] for m in models},
                    date_dt.replace(tzinfo=None),
                )
                if "calib_mean" in r:
                    err_by_name["ens:calibrated"].append(float(r["calib_mean"]) - obs)
            except Exception:
                pass

    print(f"Forecast Audit from {start} to {end}")
    print(f"City-days: {len(grouped)}")
    print("")
    print(f"{'Predictor':<16} {'N':>5} {'MAE':>8} {'Bias':>8} {'RMSE':>8}")
    print("-" * 50)
    leaderboard = []
    for name, errs in err_by_name.items():
        mae, bias, rmse = _metrics(errs)
        leaderboard.append((mae, name, len(errs), bias, rmse))
    for mae, name, n, bias, rmse in sorted(leaderboard):
        print(f"{name:<16} {n:>5d} {mae:>8.2f} {bias:>8.2f} {rmse:>8.2f}")

    print("")
    print("Weighted Ensemble MAE by City")
    print(f"{'City':<8} {'N':>4} {'MAE':>8} {'Bias':>8}")
    print("-" * 34)
    for city, errs in sorted(err_by_city_weighted.items()):
        mae, bias, _ = _metrics(errs)
        print(f"{city:<8} {len(errs):>4d} {mae:>8.2f} {bias:>8.2f}")


if __name__ == "__main__":
    main()
