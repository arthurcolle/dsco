#!/usr/bin/env python3
"""Generate a simple compile_commands.json from Makefile source lists.
This is intentionally dependency-light and good enough for clangd/clang-tidy.
"""
from __future__ import annotations
import json
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
mk = (ROOT / "Makefile").read_text()

def make_var(name: str) -> str:
    m = re.search(rf"^{name}\s*=\s*(.*?)(?=\n[A-Za-z0-9_]+\s*[?:+]?=|\n\S|\Z)", mk, re.M | re.S)
    if not m:
        return ""
    s = m.group(1).replace("\\\n", " ")
    return re.sub(r"\s+", " ", s).strip()

src_names = [x for x in make_var("SRC_NAMES").split() if x.endswith((".c", ".m"))]
test_names = [x for x in make_var("TEST_SRC_NAMES").split() if x.endswith(".c")]
std = os.environ.get("DSCO_STD", "c2y")
arch = os.environ.get("DSCO_ARCH", "native")
common = ["cc", "-Wall", "-Wextra", f"-std={std}", "-D_POSIX_C_SOURCE=200809L", "-Iinclude", f"-march={arch}", f"-mtune={arch}"]
entries = []
for rel in [*("src/" + s for s in src_names), *("tests/" + s for s in test_names)]:
    p = ROOT / rel
    if not p.exists():
        continue
    entries.append({
        "directory": str(ROOT),
        "file": str(p),
        "arguments": [*common, "-c", str(p), "-o", f"build/compile-commands/{p.stem}.o"],
    })

out = ROOT / "compile_commands.json"
out.write_text(json.dumps(entries, indent=2) + "\n")
print(f"wrote {out} ({len(entries)} entries)")
