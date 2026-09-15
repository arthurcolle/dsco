#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
desktop_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/dsco-desktop-test.XXXXXX")
trap 'rm -rf "$desktop_test_dir"' EXIT HUP INT TERM
cd "$repo"
cc=${CC:-cc}
if [ "$(uname -s)" = Darwin ]; then
    "$cc" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -Iinclude \
        tests/test_desktop_macos.c src/desktop_macos.c src/json_fast.c src/json_util.c \
        -framework ApplicationServices -o "$desktop_test_dir/desktop_macos"
    "$desktop_test_dir/desktop_macos"
fi
"$cc" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -DDSCO_DESKTOP_NO_MACOS \
    -Wall -Wextra -Werror -Iinclude tests/test_desktop_macos.c src/desktop_macos.c \
    src/json_fast.c src/json_util.c -lpthread -lm -o "$desktop_test_dir/desktop_portable"
"$desktop_test_dir/desktop_portable"
