#!/bin/sh
# Owned mock transport only; never opens a buffer, terminal, or Kitty window.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
dir=$(mktemp -d "${TMPDIR:-/tmp}/dsco-kitty-transport.XXXXXX")
trap 'rm -rf "$dir"' EXIT HUP INT TERM
cd "$repo"
case "${1:-}" in
  --sanitize) set -- -fsanitize=address,undefined -fno-omit-frame-pointer -g ;;
  "") set -- ;;
  *) echo "usage: $0 [--sanitize]" >&2; exit 2 ;;
esac
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror "$@" \
  -Iinclude tests/test_kitty_remote_transport.c src/json_util.c src/json_fast.c \
  -lpthread -lm -o "$dir/test"
"$dir/test"
