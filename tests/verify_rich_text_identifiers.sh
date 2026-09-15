#!/bin/sh
# Pure parser regression; no graphical surfaces or terminal input.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
dir=$(mktemp -d "${TMPDIR:-/tmp}/dsco-rich-identifiers.XXXXXX")
trap 'rm -rf "$dir"' EXIT HUP INT TERM
cd "$repo"
case "${1:-}" in
  --sanitize) set -- -fsanitize=address,undefined -fno-omit-frame-pointer -g ;;
  "") set -- ;;
  *) echo "usage: $0 [--sanitize]" >&2; exit 2 ;;
esac
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror "$@" -Iinclude \
  tests/test_rich_text_identifiers.c src/rich_text.c -o "$dir/test"
"$dir/test"
