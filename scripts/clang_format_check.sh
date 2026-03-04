#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found" >&2
  exit 1
fi

mapfile -t FILES < <(git ls-files '*.c' '*.h')
if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "no C/C++ files found"
  exit 0
fi

clang-format --dry-run --Werror "${FILES[@]}"
echo "clang-format check passed"
