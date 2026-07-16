#!/bin/bash
# bake_data.sh — thin wrapper around the encrypting baker (scripts/bake_data.py).
# Kept for the historical Makefile interface: bake_data.sh <data> <out_c> <out_h>.
# The baker encrypts every data/ blob at rest so the shipped binary leaks no
# baked config to strings/grep; src/embedded_data.c decrypts lazily at runtime.
set -euo pipefail

DATA_DIR="${1:-data}"
OUT_C_DIR="${2:-src/generated}"
OUT_H_DIR="${3:-include}"
KEY_FILE="${4:-build/.embed_key}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$SCRIPT_DIR/bake_data.py" "$DATA_DIR" "$OUT_C_DIR" "$OUT_H_DIR" "$KEY_FILE"
