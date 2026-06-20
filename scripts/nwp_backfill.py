#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from nwp_pipeline import backfill_hourly_observations


def _parse_dt(value: str) -> datetime:
    if len(value) == 10:
        return datetime.strptime(value, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    return datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone(timezone.utc)


def main() -> None:
    parser = argparse.ArgumentParser(description="Backfill hourly NWS observations")
    parser.add_argument("city_key", help="City key, e.g. nyc or chi")
    parser.add_argument("--start", required=True, help="Start datetime or YYYY-MM-DD")
    parser.add_argument("--end", required=True, help="End datetime or YYYY-MM-DD")
    parser.add_argument("--dry-run", action="store_true", help="Preview writes without storing them")
    args = parser.parse_args()

    start = _parse_dt(args.start)
    end = _parse_dt(args.end)
    preview = backfill_hourly_observations(args.city_key, start, end, dry_run=args.dry_run)
    print(f"Backfill rows: {len(preview)}")


if __name__ == "__main__":
    main()
