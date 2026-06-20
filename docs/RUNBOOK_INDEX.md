# DSCO Runbook Index

This file is the canonical index for the lightweight web dashboard runbooks exposed through `/api/docs/runbooks`.

## Weather freshness

- Purpose: detect stale observation or model snapshots before they reach the dashboard.
- Signals: `freshness.status`, `freshness.age_minutes`, `stale_count`, and `source_lineage`.
- First checks:
  - Refresh the weather dashboard.
  - Compare `current_time` against the freshness SLA.
  - Confirm the station mapping and fallback source order.

## Trading data outage

- Purpose: diagnose missing market or portfolio data.
- Signals: `/api/trading/status`, `market_state.no_market_data`, and the portfolio export endpoints.
- First checks:
  - Verify provider credentials are configured.
  - Confirm dry-run mode or live mode matches the operator intent.
  - Inspect the trading dashboard badges for stale/offline states.

## UI regression

- Purpose: catch broken dashboard panels, loading states, or export hooks.
- Signals: `/api/dashboard/meta`, `/api/metrics`, weather/trading export routes, and the browser console.
- First checks:
  - Confirm JSON and CSV exports return payloads.
  - Verify that loading skeletons render before fetch completion.
  - Check for broken metric acronyms or inaccessible labels.
