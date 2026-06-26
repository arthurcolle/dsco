#!/usr/bin/env bash
# Ultra-fast local development wrapper for dsco-cli.
# Uses a separate BUILD_DIR so fast/dev objects do not poison release/test caches.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ACTION="${1:-build}"
shift || true

JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
FAST_BUILD_DIR="${FAST_BUILD_DIR:-build/fast}"
COMMON=(BUILD_DIR="$FAST_BUILD_DIR" DSCO_ARCH="${DSCO_ARCH:-native}" -j"$JOBS")

case "$ACTION" in
  build)
    exec make "${COMMON[@]}" dsco "$@"
    ;;
  test)
    exec make "${COMMON[@]}" test "$@"
    ;;
  quick)
    exec make "${COMMON[@]}" dsco test_stateful_atoms test_plan_cache "$@"
    ;;
  syntax)
    mkdir -p "$FAST_BUILD_DIR/syntax"
    exec make "${COMMON[@]}" -n dsco >/dev/null
    ;;
  changed)
    exec ./scripts/changed_tests.sh "$@"
    ;;
  bench)
    exec make "${COMMON[@]}" bench-startup "$@"
    ;;
  doctor)
    ./scripts/build_cache_doctor.sh
    echo "fast build dir: $FAST_BUILD_DIR"
    echo "jobs: $JOBS"
    ;;
  clean)
    exec rm -rf "$FAST_BUILD_DIR"
    ;;
  *)
    echo "usage: $0 {build|test|quick|syntax|changed|bench|doctor|clean} [make args...]" >&2
    exit 2
    ;;
esac
