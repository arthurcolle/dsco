#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

python_bin="${PYTHON_BIN:-python}"
interval="${PLOT_REFRESH_INTERVAL_SECONDS:-900}"
archive_root="${PLOT_ARCHIVE_ROOT:-artifacts/plot_runs}"

exec "$python_bin" scripts/generate_weather_plot_gallery.py \
  --fresh \
  --archive-root "$archive_root" \
  --interval-seconds "$interval" \
  "$@"
