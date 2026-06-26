#!/usr/bin/env bash
# Touch the fast build stamp/tree to force a quick rebuild without disturbing release objects.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p build/fast
printf '%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > build/fast/.fast_touch
echo "touched build/fast/.fast_touch"
