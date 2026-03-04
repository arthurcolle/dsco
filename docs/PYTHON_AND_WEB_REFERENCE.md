# Python and Web Asset Reference

This document covers all non-C runtime assets in the repository.

## Python Scripts

### `scripts/analyst_swarm.py` (9019 LOC)

Purpose:

- Large multi-agent equity analysis engine producing institutional-style synthesized reports.

CLI:

```bash
python3 scripts/analyst_swarm.py <TICKER> [--agents ...] [--depth quick|standard|deep] [--no-color]
```

Inputs and env:

- `ALPHA_VANTAGE_API_KEY` (primary market data enrichment)
- `FRED_API_KEY` (macro overlays, optional but used)
- Local cache under `~/.dsco/cache`

Outputs:

- Rich terminal report by specialist agent domains
- Synthesis stage with recommendation and conviction metrics

### `scripts/equity_kb.py` (2479 LOC)

Purpose:

- Full-universe equity knowledge base backed by SQLite + embeddings.

CLI subcommands:

- `build`
- `update`
- `search`
- `report`
- `screen`
- `export`
- `stats`
- `sources`

Key options:

- `--ticker`, `--limit`, `--workers`, `--stale-days`, `--deep-research`, etc.

Env:

- `DSCO_EQUITY_DB` (DB path override)
- `JINA_API_KEY`
- `PARALLEL_API_KEY`
- `ALPHA_VANTAGE_API_KEY`
- `FRED_API_KEY` (for macro augmentors)

Storage:

- Creates and maintains rich schema: companies, annual_financials, computed_metrics, notes, embeddings, jobs, source health, AV cache.

### `scripts/valuation.py` (1336 LOC)

Purpose:

- Full valuation and quality workflow for a single ticker with multi-model fair value analysis.

CLI:

```bash
python3 scripts/valuation.py <TICKER> [--json] [--no-color]
```

Env:

- `ALPHA_VANTAGE_API_KEY` required for fetch layer.

Output:

- Human-readable or JSON valuation report (DCF + quality + risk overlays).

### `scripts/freight_api.py` (652 LOC)

Purpose:

- Local freight data API server with realistic random-walk simulation.

Run:

```bash
python3 scripts/freight_api.py
```

Default bind:

- `127.0.0.1:8422`

Main endpoints:

- `/api/health`
- `/api/rates`
- `/api/rates/<route_id>`
- `/api/indices`
- `/api/container/<index>`
- `/api/chokepoints`
- `/api/news`
- `/api/derivatives`

Architecture:

- In-memory simulator + cached responses
- Background tick thread updates route rates
- HTTPServer + JSON responses

### `scripts/freight_quant_tools.py` (2197 LOC)

Purpose:

- Freight quant tool registry and execution facade across market/vessel/cargo/macro/geopolitical/fundamental/execution domains.

Notes:

- Contains extensive structured `ToolSpec` catalog
- Mix of implemented data fetchers and placeholders requiring vendor credentials

Env:

- `FRED_API_KEY`, `BALTIC_API_KEY`, `CME_API_KEY`, `KPLER_API_KEY`, `VORTEXA_API_KEY`, `SPIRE_AIS_KEY`, `GDELT_API_KEY`

Typical output:

- Registry summaries, provider-stack diagnostics, market structure and intelligence views.

### `scripts/hormuz_ffa.py` (883 LOC)

Purpose:

- Crisis-focused freight/energy intelligence report for Hormuz scenarios.

Run:

```bash
python3 scripts/hormuz_ffa.py
```

Env:

- `FRED_API_KEY` optional (uses fallback crisis constants when absent)

Output:

- Terminal report with energy, tanker rates, scenario stress, and catalyst timeline.

### `valuation.py` (root, 440 LOC)

Purpose:

- Lightweight standalone stock valuation report script.

Run:

```bash
python3 valuation.py <TICKER>
```

Notes:

- Simpler/faster than `scripts/valuation.py`; oriented to quick snapshot valuation.

## Web Assets

### `www/freight.html` (2135 LOC)

Purpose:

- Interactive “DSCO Freight Terminal” dashboard UI.

Tech stack:

- Vanilla HTML/CSS/JS
- Leaflet for map rendering
- Chart.js for charting

Design:

- Dark trading-floor visual system with custom CSS variables and responsive panels.

Expected data source:

- Local freight API (`scripts/freight_api.py`) and/or timeline static serving through `baseline` HTTP server.

### `www/freight_intelligence_report.html` (4015 LOC)

Purpose:

- Rich static freight intelligence report page.

Tech:

- Pure HTML/CSS
- Designed print/readability-first with section navigation and KPI components.

## Integration with C Runtime

The C timeline server (`baseline.c`) can serve web pages via:

- `/freight` -> `www/freight.html`
- `/www/<file>` -> static pass-through

This allows a single `dsco --timeline-server` process to host both timeline/observability views and freight UI assets.
