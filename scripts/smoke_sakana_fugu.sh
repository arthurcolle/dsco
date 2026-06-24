#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ -z "${FUGU_API_KEY:-}" && -z "${SAKANA_API_KEY:-}" && -z "${FISH_API_KEY:-}" && -z "${SAKANA_TOKEN:-}" ]]; then
  echo "skip: set FUGU_API_KEY (or SAKANA_API_KEY/FISH_API_KEY/SAKANA_TOKEN)" >&2
  exit 2
fi

MODEL="${1:-fugu}"
PROMPT='Reply with exactly: FUGU_OK'

exec ./dsco --provider sakana -m "$MODEL" "$PROMPT"
