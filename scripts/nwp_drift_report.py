#!/usr/bin/env python3
"""Report upstream source-drift events captured by nwp_pipeline."""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import nwp_pipeline


def main() -> int:
    parser = argparse.ArgumentParser(description="NWP source-drift report")
    parser.add_argument("--source", help="Limit to one source name")
    parser.add_argument("--limit", type=int, default=25, help="Max events to return")
    parser.add_argument("--json", action="store_true", help="Emit JSON")
    args = parser.parse_args()

    events = nwp_pipeline.source_drift_report(args.source, limit=args.limit)
    if args.json:
        print(json.dumps(events, indent=2, sort_keys=True))
        return 0

    if not events:
        print("No source-drift events")
        return 0

    print(f"{'ID':>4} {'Severity':<8} {'Source':<24} {'City':<6} {'Model':<8} Missing")
    print("-" * 82)
    for event in events:
        missing = ",".join(event["missing_fields"]) or "-"
        print(
            f"{event['id']:>4} {event['severity']:<8} {event['source']:<24} "
            f"{event.get('city') or '-':<6} {event.get('model') or '-':<8} {missing}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
