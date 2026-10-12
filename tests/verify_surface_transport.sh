#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
transport_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/dsco-surface-transport.XXXXXX")
trap 'rm -rf "$transport_test_dir"' EXIT HUP INT TERM
cd "$repo"
case "${1:-}" in
    --sanitize) set -- -fsanitize=address,undefined -fno-omit-frame-pointer -g ;;
    "") set -- ;;
    *) echo "usage: $0 [--sanitize]" >&2; exit 2 ;;
esac
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror \
    "$@" \
    -Iinclude tests/test_surface_transport.c src/process_capture.c src/tool_content.c \
    src/json_util.c src/json_fast.c -lpthread -lm -o "$transport_test_dir/test_surface_transport"
"$transport_test_dir/test_surface_transport"
