#!/usr/bin/env python3
"""Render station/model forecast coverage for active Kalshi weather cities."""

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import nwp_pipeline


def parse_dt(value: str):
    if len(value) == 10:
        return datetime.strptime(value, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    return nwp_pipeline._ensure_utc(datetime.fromisoformat(value.replace("Z", "+00:00")))


def main() -> int:
    parser = argparse.ArgumentParser(description="NWP station coverage heatmap")
    parser.add_argument("--start", required=True, help="Start UTC timestamp or YYYY-MM-DD")
    parser.add_argument("--end", required=True, help="End UTC timestamp or YYYY-MM-DD")
    parser.add_argument("--cities", nargs="+", help="City keys")
    parser.add_argument("--models", nargs="+", help="Model keys")
    parser.add_argument("--json", action="store_true", help="Emit JSON")
    args = parser.parse_args()

    heatmap = nwp_pipeline.station_coverage_heatmap(
        parse_dt(args.start),
        parse_dt(args.end),
        city_keys=args.cities,
        models=args.models,
    )
    if args.json:
        print(json.dumps(heatmap, indent=2, sort_keys=True))
    else:
        print(nwp_pipeline.render_station_coverage_heatmap(heatmap))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
