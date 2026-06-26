#!/usr/bin/env bash
# Build/dev-loop sanity checks.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

missing=0
required=(
  scripts/dev_fast.sh
  scripts/changed_tests.sh
  scripts/gen_compile_commands.py
  scripts/build_report.py
  scripts/build_cache_doctor.sh
  scripts/fast_touch.sh
  scripts/build_time_trace.sh
  scripts/gen_ninja.py
)
for f in "${required[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "missing: $f"
    missing=1
  fi
done

if [[ ! -d build ]]; then
  echo "note: build/ does not exist yet"
fi

if command -v git >/dev/null 2>&1; then
  echo "branch: $(git branch --show-current 2>/dev/null || echo unknown)"
  echo "dirty files: $(git status --short 2>/dev/null | wc -l | tr -d ' ')"
fi

if [[ $missing -ne 0 ]]; then
  exit 1
fi

echo "build cache doctor: ok"
