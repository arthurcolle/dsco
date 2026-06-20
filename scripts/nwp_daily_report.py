#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from nwp_pipeline import (
    KALSHI_CITIES,
    daily_missing_data_report,
    detect_duplicate_forecasts,
    validate_city_station_mappings,
)


def _parse_date(value: str | None) -> datetime:
    if not value:
        return datetime.now(timezone.utc)
    return datetime.strptime(value, "%Y-%m-%d").replace(tzinfo=timezone.utc)


def main() -> None:
    parser = argparse.ArgumentParser(description="Daily NWP ingest report")
    parser.add_argument("--date", help="Target date YYYY-MM-DD")
    args = parser.parse_args()

    target = _parse_date(args.date)
    errors = validate_city_station_mappings()

    print(f"Daily NWP report for {target.strftime('%Y-%m-%d')}")
    if errors:
        print("Station mapping issues:")
        for err in errors:
            print(f"  - {err}")
    else:
        print("Station mappings OK")

    report = daily_missing_data_report(target)
    print("")
    print(f"{'City':<16} {'Model':<8} {'Observed':>8} {'Missing':>8}")
    print("-" * 44)
    for row in report:
        if row["missing_rows"] > 0:
            print(
                f"{KALSHI_CITIES[row['city']][0]:<16} {row['model']:<8} "
                f"{row['observed_rows']:>8} {row['missing_rows']:>8}"
            )

    dupes = detect_duplicate_forecasts()
    print("")
    print(f"Duplicate forecast groups: {len(dupes)}")


if __name__ == "__main__":
    main()
