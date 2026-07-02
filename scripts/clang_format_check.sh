#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found" >&2
  exit 1
fi

FILES=()
while IFS= read -r file; do
  FILES+=("$file")
done < <(git ls-files '*.c' '*.h' |
  grep -vE '^(vendor|gsl)/' |
  grep -vE '^include/(dist_logo|tool_embeddings)\.h$')
if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "no C/C++ files found"
  exit 0
fi

clang-format --dry-run --Werror "${FILES[@]}"
echo "clang-format check passed"
