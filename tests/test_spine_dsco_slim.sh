#!/bin/sh
set -eu
bin=${1:-./spine-dsco-slim}
"$bin" -h | grep -q 'usage: spine-dsco-slim'
out=$("$bin" -c ':cwd {}')
test -n "$out"
printf 'exit\n' | "$bin" >/dev/null
"$bin" -c '.tier' | grep -q .
"$bin" -c '@py -c "print(42)"' | grep -q 42
script=$(mktemp); trap 'rm -f "$script"' EXIT
printf '.tier\n:cwd {}\n' >"$script"
"$bin" -f "$script" >/dev/null
printf 'spine-dsco-slim smoke: ok\n'
