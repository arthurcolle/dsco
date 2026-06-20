#!/usr/bin/env python3
"""Generate quantitative speculation reports for weather-market research."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from quant_spec_reports import (  # noqa: E402
    build_quant_spec_bundle,
    load_cases,
    sample_speculative_cases,
    write_quant_spec_reports,
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", help="Optional JSON list of market cases, or object with a cases list")
    parser.add_argument("--out-dir", default="artifacts/reports/quant_speculation")
    parser.add_argument("--as-of", default="2026-04-10T12:00:00Z")
    parser.add_argument("--quantity", type=float, default=25.0)
    parser.add_argument("--fee-rate", type=float, default=0.0125)
    args = parser.parse_args()

    cases = load_cases(args.input) if args.input else sample_speculative_cases()
    bundle = build_quant_spec_bundle(
        cases,
        as_of=args.as_of,
        quantity=args.quantity,
        fee_rate=args.fee_rate,
    )
    manifest = write_quant_spec_reports(bundle, args.out_dir)
    print(json.dumps({"out_dir": args.out_dir, **manifest}, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
