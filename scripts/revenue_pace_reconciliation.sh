#!/usr/bin/env bash
# T12 (SOTA_FRAMES_2026-09-02.md, tripwire quartet #3): monthly revenue-vs-
# pace reconciliation against the DSI $300K/1yr target (USER.md north star),
# i.e. $25K/mo pace. Reads docs/DSCO_REVENUE_PIPELINE.csv for rows whose
# `stage` column is a closed/won state and sums `estimated_value` for the
# current calendar month. Deliberately conservative: a template-only pipeline
# (no real rows) is reported as a hard alarm, not silently skipped — an
# empty pipeline is itself the most important signal this script can raise.
#
# Usage:
#   scripts/revenue_pace_reconciliation.sh            # human report, exit 0
#   scripts/revenue_pace_reconciliation.sh --check     # exit 1 if off pace
set -euo pipefail
export LC_ALL=C

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CSV="$ROOT/docs/DSCO_REVENUE_PIPELINE.csv"
ANNUAL_TARGET_USD="${DSCO_REVENUE_ANNUAL_TARGET_USD:-300000}"
MONTHLY_PACE_USD=$(( ANNUAL_TARGET_USD / 12 ))
CHECK=0
[[ "${1:-}" == "--check" ]] && CHECK=1
month_now="$(date +%Y-%m)"

if [[ ! -f "$CSV" ]]; then
  echo "REVENUE ALARM: pipeline file missing at $CSV — cannot reconcile against \$${MONTHLY_PACE_USD}/mo pace"
  (( CHECK == 1 )) && exit 1
  exit 0
fi

data_rows=$(( $(wc -l < "$CSV" | tr -d ' ') - 1 ))
if (( data_rows <= 0 )); then
  echo "REVENUE ALARM: $CSV has header only, zero pipeline rows."
  echo "Pace target: \$${MONTHLY_PACE_USD}/mo (from \$${ANNUAL_TARGET_USD}/yr north star)."
  echo "Observed: \$0 this month. Variance: -100% (\$${MONTHLY_PACE_USD} short)."
  echo "Action: this is not a formatting error — it means no revenue activity has been"
  echo "logged anywhere DSCO can observe. Either log real pipeline stages or treat the"
  echo "\$300K/1yr target as unstarted, not merely 'behind pace'."
  (( CHECK == 1 )) && exit 1
  exit 0
fi

# CSV: account,segment,buyer_name,buyer_title,contact_url,warm_intro_path,
#      observed_trigger,workflow_hypothesis,pain_baseline,authority_constraints,
#      estimated_value,stage,last_contact,next_action,next_action_date,owner,notes
closed_total=0
closed_count=0
while IFS=, read -r account _segment _buyer _title _url _intro _trigger _hyp _pain _auth value stage last_contact _next _next_date _owner _notes; do
  [[ "$account" == "account" || "$account" == "[Account]" ]] && continue
  stage_lc="$(echo "${stage:-}" | tr '[:upper:]' '[:lower:]')"
  case "$stage_lc" in
    won|closed|closed-won|closed_won)
      contact_month="${last_contact:0:7}"
      if [[ "$contact_month" == "$month_now" ]]; then
        numeric_value="$(echo "${value:-0}" | tr -dc '0-9.')"
        closed_total=$(awk -v a="$closed_total" -v b="${numeric_value:-0}" 'BEGIN{printf "%.2f", a+b}')
        closed_count=$((closed_count + 1))
      fi
      ;;
  esac
done < "$CSV"

variance_pct=$(awk -v obs="$closed_total" -v pace="$MONTHLY_PACE_USD" 'BEGIN{ if (pace==0) print 0; else printf "%.1f", ((obs-pace)/pace)*100 }')

echo "Revenue pace reconciliation — $month_now"
echo "  Target pace:   \$${MONTHLY_PACE_USD}/mo"
echo "  Observed:      \$${closed_total} (${closed_count} closed-won row(s) this month)"
echo "  Variance:      ${variance_pct}%"

if awk -v v="$variance_pct" 'BEGIN{exit !(v < -20)}'; then
  echo "  ALARM: more than 20% behind monthly pace."
  (( CHECK == 1 )) && exit 1
fi
exit 0
