#!/usr/bin/env bash
set -euo pipefail

# First-class Cosmopolitan build driver for DSCO.
#
# This does not replace the Darwin-native build. It creates a portable build
# lane with its own object tree and compiler profile, so macOS frameworks,
# Homebrew dylibs, and Objective-C acceleration remain available in the normal
# target while Cosmopolitan work can advance independently.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

COSMOCC="${COSMOCC:-}"
if [[ -z "$COSMOCC" ]]; then
  COSMOCC="$ROOT/build/cosmocc/current/bin/cosmocc"
fi
if [[ ! -x "$COSMOCC" ]]; then
  COSMOCC="$("$ROOT/scripts/cosmo_bootstrap.sh")"
fi

OUT="${DSCO_COSMO_OUT:-dsco.distributed.systems}"
BUILD_DIR="${DSCO_COSMO_BUILD_DIR:-$ROOT/build/cosmo}"
MODE="${DSCO_COSMO_MODE:-normal}"
STD="${DSCO_COSMO_STD:-gnu2x}"
JOBS="${DSCO_COSMO_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

case "$MODE" in
  normal) MODE_FLAG="" ;;
  tiny) MODE_FLAG="-mtiny" ;;
  tinylinux) MODE_FLAG="-mtinylinux" ;;
  optlinux) MODE_FLAG="-moptlinux" ;;
  sysv) MODE_FLAG="-msysv" ;;
  dbg) MODE_FLAG="-mdbg" ;;
  *) echo "[cosmo] unknown DSCO_COSMO_MODE=$MODE" >&2; exit 2 ;;
esac

# Current full DSCO depends on curl/sqlite/readline and Darwin-only framework
# lanes. Cosmopolitan can support these, but only when their cosmo-built libs
# are supplied. The default portable lane currently builds the embedded lite
# front door as an APE artifact while the full-runtime path is made explicit.
# Set DSCO_COSMO_EXPERIMENTAL_FULL=1 to ask Makefile to include the complete
# native source set and whatever cosmo libraries you provide via CFLAGS/LDLIBS.
EXPERIMENTAL_FULL="${DSCO_COSMO_EXPERIMENTAL_FULL:-0}"

COMMON_CFLAGS=(
  -Wall -Wextra -O2 -std="$STD" -D_POSIX_C_SOURCE=200809L
  -DDSCO_COSMOPOLITAN=1
  -DBUILD_DATE=\\\"$(date -u +%Y-%m-%dT%H:%M:%SZ)\\\"
  -DGIT_HASH=\\\"$(git rev-parse --short HEAD 2>/dev/null || echo unknown)\\\"
  -Iinclude -Ivendor
)

# Native optimization flags like -march=native are deliberately not used here:
# the point is a portable binary with Cosmopolitan's own baseline selection.
export CC="$COSMOCC"
export DSCO_STD="$STD"
export DSCO_ARCH=""
export BUILD_DIR="$BUILD_DIR"
export STATIC_DEPS=0
export COSMO_BUILD=1
export COSMO_MODE_FLAG="$MODE_FLAG"
export COSMO_FULL="$EXPERIMENTAL_FULL"

if [[ "$EXPERIMENTAL_FULL" == "1" ]]; then
  echo "[cosmo] experimental full build requested" >&2
  echo "[cosmo] this requires cosmo-built curl/sqlite/readline/etc. via CFLAGS/LDLIBS" >&2
  make -j"$JOBS" \
    CC="$COSMOCC" \
    BUILD_DIR="$BUILD_DIR" \
    DSCO_STD="$STD" \
    BASE_CFLAGS="${COMMON_CFLAGS[*]} ${CFLAGS:-}" \
    CFLAGS="${COMMON_CFLAGS[*]} ${CFLAGS:-}" \
    LDFLAGS="${LDFLAGS:-}" \
    RELEASE_LDFLAGS="$MODE_FLAG ${RELEASE_LDFLAGS:-}" \
    LDLIBS="${LDLIBS:-} -lm" \
    "$(basename "$OUT")"
else
  echo "[cosmo] portable build lane enabled" >&2
  echo "[cosmo] output: $OUT" >&2
  make -j"$JOBS" \
    CC="$COSMOCC" \
    BUILD_DIR="$BUILD_DIR" \
    DSCO_STD="$STD" \
    COSMO_PORTABLE=1 \
    CFLAGS="${COMMON_CFLAGS[*]} ${CFLAGS:-}" \
    LDFLAGS="${LDFLAGS:-}" \
    RELEASE_LDFLAGS="$MODE_FLAG ${RELEASE_LDFLAGS:-}" \
    LDLIBS="${LDLIBS:-} -lm" \
    "$(basename "$OUT")"
fi

if [[ -x "$OUT" ]]; then
  echo "[cosmo] built $OUT" >&2
  ls -lh "$OUT" >&2 || true
else
  echo "[cosmo] build finished but $OUT is not executable" >&2
fi
