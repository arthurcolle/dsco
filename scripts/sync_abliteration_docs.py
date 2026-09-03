#!/usr/bin/env python3
"""Refresh dsco-cli's Abliteration.ai docs index through the canonical sync job."""

from __future__ import annotations

import runpy
import sys
from pathlib import Path


CLI_ROOT = Path(__file__).resolve().parents[1]
ROUTER_SYNC = CLI_ROOT.parent / "dsco-router" / "scripts" / "sync_abliteration_docs.py"


def main() -> None:
    if not ROUTER_SYNC.is_file():
        raise SystemExit(f"canonical Abliteration.ai docs sync job not found: {ROUTER_SYNC}")
    if len(sys.argv) == 1:
        sys.argv.extend(
            [
                "--json",
                str(CLI_ROOT / "data" / "abliteration_docs_index.json"),
                "--markdown",
                str(CLI_ROOT / "docs" / "ABLITERATION_AI_DOCS.md"),
            ]
        )
    runpy.run_path(str(ROUTER_SYNC), run_name="__main__")


if __name__ == "__main__":
    main()
