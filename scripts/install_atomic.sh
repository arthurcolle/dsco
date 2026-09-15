#!/bin/sh
# Publish a complete executable without truncating a running process's inode.
set -eu
if [ "$#" -ne 2 ]; then
    echo "usage: install_atomic.sh SOURCE DESTINATION" >&2
    exit 2
fi
src=$1
dst=$2
tmp=$(mktemp "${dst}.tmp.XXXXXX")
trap 'rm -f "$tmp"' EXIT HUP INT TERM
install -m 755 "$src" "$tmp"
mv -f "$tmp" "$dst"
