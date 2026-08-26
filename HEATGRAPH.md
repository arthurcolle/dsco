# Heatgraph Layer — Implementation Summary

## Overview
Added an in-memory ring buffer for metrics collection and export, enabling heatgraph visualization of child process behavior over time.

## Files Added
- `include/ring_buffer.h` — Fixed-capacity (3600 samples) circular buffer API
- `src/ring_buffer.c` — Ring buffer implementation with push/count/get operations

## Files Modified

### `src/supervisor.c`
- Added `#include "ring_buffer.h"`
- Added `static metric_ring_t g_metrics_ring;` global instance
- Modified `append_child_metric()` to push samples into the ring before writing JSONL
- Added export functions:
  - `supervisor_metrics_dump()` — dispatcher for format backends
  - `supervisor_metrics_count()` — returns current ring buffer size
  - `dump_json()` — exports all samples as JSON array
  - `dump_csv()` — exports as CSV with header row
  - `dump_heatgraph()` — aggregates into 10-second time buckets per child, computing avg RSS and avg memory pressure

### `include/supervisor.h`
- Added `metrics_format_t` enum (JSON, CSV, HEATGRAPH)
- Exposed `supervisor_metrics_dump()` and `supervisor_metrics_count()`

### `src/main.c`
- Added `bool arg_metrics = false;` flag
- Added `metrics` subcommand dispatch
- Parses `--format {json|csv|heatgraph}` and `--output PATH` arguments
- Defaults: JSON format, stdout output

### `Makefile`
- Added `ring_buffer.c` to `SRC_NAMES` (line 96)

## Usage

```bash
# JSON export to stdout
dsco metrics

# CSV export to file
dsco metrics --format csv --output /tmp/metrics.csv

# Heatgraph export (time-bucketed aggregates)
dsco metrics --format heatgraph --output /tmp/heatgraph.json
```

## Heatgraph Format

The heatgraph output aggregates samples into 10-second windows per child process:

```json
{
  "time_start": 1729358400,
  "time_end": 1729358700,
  "bucket_size": 10,
  "children": [12345, 12346],
  "buckets": [
    {
      "ts": 1729358400,
      "data": {
        "12345": {
          "avg_rss": 52428800,
          "avg_pressure": 2.5,
          "samples": 10
        }
      }
    }
  ]
}
```

## Build Status

✅ `ring_buffer.o` compiles cleanly (4.4K Mach-O arm64)  
⚠️ Link step fails due to **pre-existing** missing dependencies (hiredis, libsodium) — unrelated to heatgraph changes

## Next Steps

1. Resolve vendored dependency build (hiredis, libsodium)
2. Test with a running supervisor: spawn child processes, let metrics accumulate, then export
3. Visualize heatgraph output with a web-based timeline (D3.js, Plotly, or custom SVG renderer)
4. Consider adding pressure threshold alerts (e.g., emit warning when avg_pressure > 5)

## Design Notes

- **Zero-overhead when unused**: The ring buffer is always populated, but export is on-demand
- **Backward compatible**: JSONL metrics file format unchanged; ring buffer is additive
- **Bounded memory**: 3600 samples × ~40 bytes = ~144KB max, regardless of runtime duration
- **No external dependencies**: Pure C, no new libraries required
