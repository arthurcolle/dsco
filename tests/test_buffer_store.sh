#!/bin/sh
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_tmp=$(mktemp -d /tmp/dsco-buffer-build-XXXXXX)
trap 'rm -f "$task_tmp/test"; rm -rf "$task_tmp/test.dSYM"; rmdir "$task_tmp"' EXIT HUP INT TERM
cd "$task_root"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    ${BUFFER_TEST_CFLAGS:-} -Iinclude tests/test_buffer_store.c \
    src/buffer_store.c src/json_util.c src/crypto.c vendor/yyjson.c \
    -lsqlite3 -lpthread -o "$task_tmp/test"
"$task_tmp/test"
