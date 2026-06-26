#!/usr/bin/env bash
# Build with clang time-trace enabled into an isolated tree.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
if ! ${CC:-cc} --version 2>/dev/null | grep -qi clang; then
  echo "build_time_trace requires clang-compatible CC" >&2
  exit 1
fi
exec make BUILD_DIR=build/time-trace CFLAGS="${CFLAGS:-} -ftime-trace" dsco "$@"
