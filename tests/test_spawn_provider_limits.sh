#!/bin/sh
# Compile the current handler, not a copied implementation; no providers needed.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/dsco-spawn-limits.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
awk '/^static bool tool_spawn_provider\(/ {copy=1} copy {print} copy && /^}/ {exit}' \
    "$root/src/tools.c" > "$tmp/spawn_provider_under_test.h"
test -s "$tmp/spawn_provider_under_test.h"
${CC:-cc} -std=c11 -D_DARWIN_C_SOURCE -D_POSIX_C_SOURCE=200809L \
    -I"$root/include" -I"$tmp" "$root/tests/test_spawn_provider_limits.c" \
    "$root/src/json_util.c" "$root/src/env_config.c" -lm -o "$tmp/test"
"$tmp/test"
