#!/usr/bin/env python3
"""Replay or prune raw payload snapshots captured by nwp_pipeline."""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import nwp_pipeline


def main() -> int:
    parser = argparse.ArgumentParser(description="NWP payload snapshot replay/prune utility")
    sub = parser.add_subparsers(dest="command", required=True)

    replay = sub.add_parser("replay", help="Replay one snapshot by id")
    replay.add_argument("snapshot_id", type=int)
    replay.add_argument("--apply", action="store_true", help="Allow replay adapters to write when supported")

    prune = sub.add_parser("prune", help="Prune old snapshots")
    prune.add_argument("--older-than-days", type=int, required=True)
    prune.add_argument("--source", help="Limit to one source")
    prune.add_argument("--apply", action="store_true", help="Actually delete files and DB rows")

    args = parser.parse_args()
    if args.command == "replay":
        result = nwp_pipeline.replay_payload_snapshot(args.snapshot_id, dry_run=not args.apply)
    else:
        result = nwp_pipeline.prune_payload_snapshots(
            args.older_than_days,
            source=args.source,
            dry_run=not args.apply,
        )
    print(json.dumps(result, indent=2, sort_keys=True, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
