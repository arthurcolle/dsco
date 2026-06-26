#!/usr/bin/env python3
"""Emit a compact local build/source report."""
from __future__ import annotations
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def sh(cmd: list[str]) -> str:
    try:
        return subprocess.check_output(cmd, cwd=ROOT, text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return ""

def count_lines(paths):
    total = 0
    files = 0
    for p in paths:
        try:
            total += sum(1 for _ in p.open('rb'))
            files += 1
        except OSError:
            pass
    return {"files": files, "lines": total}

src = list((ROOT / "src").rglob("*.c")) + list((ROOT / "src").rglob("*.m"))
headers = list((ROOT / "include").rglob("*.h"))
tests = list((ROOT / "tests").rglob("*.c"))
report = {
    "git_hash": sh(["git", "rev-parse", "--short", "HEAD"]),
    "branch": sh(["git", "branch", "--show-current"]),
    "status_short": sh(["git", "status", "--short"]),
    "src": count_lines(src),
    "include": count_lines(headers),
    "tests": count_lines(tests),
}
out = ROOT / "build" / "build_report.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
print(f"wrote {out}")
