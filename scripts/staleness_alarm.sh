#!/usr/bin/env bash
# T12 (SOTA_FRAMES_2026-09-02.md, tripwire quartet #4): fire when any
# harness-parity roadmap doc has gone stale — i.e. nobody has touched a plan
# doc in DSCO_STALENESS_DAYS (default 45) days while its status board entry
# still claims active/in-progress. A silent roadmap is a lying roadmap.
#
# Usage:
#   scripts/staleness_alarm.sh              # human report, exit 0 if clean
#   scripts/staleness_alarm.sh --check      # exit 1 if any doc is stale (CI)
set -euo pipefail
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLAN_DIR="$ROOT/.workspace/harness-parity"
STALE_DAYS="${DSCO_STALENESS_DAYS:-45}"
CHECK=0
[[ "${1:-}" == "--check" ]] && CHECK=1

if [[ ! -d "$PLAN_DIR" ]]; then
  echo "staleness_alarm: no plan dir at $PLAN_DIR — nothing to check" >&2
  exit 0
fi

now_epoch=$(date +%s)
stale_count=0
declare -a stale_list=()

while IFS= read -r -d '' f; do
  base="$(basename "$f")"
  # Only plan-number-prefixed docs count as roadmap plans (NN_NAME.md /
  # NNL_NAME.md), not strategy/addendum/index docs.
  [[ "$base" =~ ^[0-9]{2}[A-Za-z]?_ ]] || continue

  mtime_epoch=$(stat -f "%m" "$f" 2>/dev/null || stat -c "%Y" "$f" 2>/dev/null)
  age_days=$(( (now_epoch - mtime_epoch) / 86400 ))

  if (( age_days > STALE_DAYS )); then
    stale_count=$((stale_count + 1))
    stale_list+=("$base — ${age_days}d untouched (limit ${STALE_DAYS}d)")
  fi
done < <(find "$PLAN_DIR" -maxdepth 1 -name "*.md" -print0)

if (( stale_count > 0 )); then
  echo "STALENESS ALARM: $stale_count roadmap doc(s) exceed ${STALE_DAYS}-day freshness limit:"
  for line in "${stale_list[@]}"; do
    echo "  - $line"
  done
  echo
  echo "Action: verify the plan's status vs 00_MASTER.md; either update the doc with observed"
  echo "state or explicitly mark it dormant. A stale plan doc silently becomes a false claim."
  (( CHECK == 1 )) && exit 1
  exit 0
fi

echo "staleness_alarm: OK — all $(find "$PLAN_DIR" -maxdepth 1 -name '[0-9]*.md' | wc -l | tr -d ' ') roadmap plan docs touched within ${STALE_DAYS} days"
exit 0
