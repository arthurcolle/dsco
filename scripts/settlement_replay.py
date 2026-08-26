#!/usr/bin/env python3
"""
Replay yesterday (or chosen date) forecasts versus settled CLI observations.

Uses model forecasts from nwp_forecasts.db with cutoff-hour filtering,
then runs calibration to compare raw/weighted/calibrated estimates.
"""

from __future__ import annotations

import argparse
import sqlite3
from datetime import datetime, timedelta, timezone
from pathlib import Path

from calibration import CALIB_DB, _collect_model_forecasts_from_nwp, calibrate_city
from nwp_pipeline import KALSHI_CITIES


def _fetch_obs(city_key: str, date_s: str):
    con = sqlite3.connect(str(CALIB_DB))
    row = con.execute(
        """
        SELECT obs_high_f, obs_low_f
        FROM verification
        WHERE city=? AND date=? AND model='cli'
        LIMIT 1
        """,
        (city_key, date_s),
    ).fetchone()
    con.close()
    if not row:
        return None, None
    return row[0], row[1]


def main() -> None:
    p = argparse.ArgumentParser(description="Replay model forecasts vs settlement")
    p.add_argument("--date", help="Settlement date YYYY-MM-DD (default: yesterday UTC)")
    p.add_argument("--cutoff-hour", type=int, default=23, help="Forecast init cutoff hour UTC")
    args = p.parse_args()

    target = (
        datetime.strptime(args.date, "%Y-%m-%d").replace(tzinfo=timezone.utc)
        if args.date
        else (datetime.now(timezone.utc) - timedelta(days=1)).replace(hour=0, minute=0, second=0, microsecond=0)
    )
    date_s = target.strftime("%Y-%m-%d")

    if not Path(CALIB_DB).exists():
        raise SystemExit(f"Calibration DB not found: {CALIB_DB}")

    rows = []
    for city_key, meta in sorted(KALSHI_CITIES.items()):
        obs_high, obs_low = _fetch_obs(city_key, date_s)
        if obs_high is None:
            continue

        model_map = _collect_model_forecasts_from_nwp(city_key, date_s, cutoff_hour_utc=args.cutoff_hour)
        if not model_map:
            continue

        model_highs = {m: v["high_f"] for m, v in model_map.items() if v.get("high_f") is not None}
        if not model_highs:
            continue

        calib = calibrate_city(city_key, model_highs, target)
        weighted = float(calib.get("weighted_mean", sum(model_highs.values()) / max(len(model_highs), 1)))
        cal_mean = float(calib.get("calib_mean", weighted))

        rows.append(
            {
                "city": city_key,
                "name": meta[0],
                "obs": float(obs_high),
                "n": len(model_highs),
                "raw_min": min(model_highs.values()),
                "raw_max": max(model_highs.values()),
                "weighted": weighted,
                "cal": cal_mean,
                "err_w": weighted - float(obs_high),
                "err_c": cal_mean - float(obs_high),
            }
        )

    if not rows:
        raise SystemExit(f"No replay rows for {date_s}")

    rows.sort(key=lambda r: abs(r["err_c"]), reverse=True)

    print(f"Settlement Replay — {date_s} (cutoff <= {args.cutoff_hour:02d}Z)")
    print("NYC uses Central Park-adjusted station mapping in this replay.")
    print("")
    print(f"{'City':<6} {'Obs':>5} {'N':>3} {'RawRange':>11} {'Weighted':>9} {'Calib':>7} {'ErrW':>7} {'ErrC':>7}")
    print("-" * 68)
    mae_w = mae_c = 0.0
    for r in rows:
        mae_w += abs(r["err_w"])
        mae_c += abs(r["err_c"])
        rr = f"{r['raw_min']:.0f}-{r['raw_max']:.0f}"
        print(
            f"{r['city']:<6} {r['obs']:>5.1f} {r['n']:>3d} {rr:>11} {r['weighted']:>9.1f} {r['cal']:>7.1f} {r['err_w']:>+7.1f} {r['err_c']:>+7.1f}"
        )

    n = len(rows)
    print("")
    print(f"MAE weighted:   {mae_w / n:.2f}°F")
    print(f"MAE calibrated: {mae_c / n:.2f}°F")


if __name__ == "__main__":
    main()
