# Chimera data fusion warehouse

The fusion warehouse is Chimera's governed analytical read model. It combines
first-party routing and runtime telemetry with versioned model catalogs,
provider capabilities, benchmarks, and optional normalized external evidence.
It does not replace any operational database or write back to a source.

## Architecture

```text
SQLite and immutable JSON/JSONL sources
        |
        | consistent SQLite backups; source-specific projection, hashing,
        | validation, and redaction
        v
typed staging files -> candidate DuckDB OLAP database -> data-quality gates
                                                        |
                                 +----------------------+
                                 v
                  atomic fusion.duckdb replacement
                                 +
                  immutable, content-addressed Parquet snapshot
                                 +
                  atomic published/current.json pointer
```

SQLite remains the concurrent OLTP source of truth. The builder uses SQLite's
online backup API rather than querying live database files through DuckDB. It
then builds a candidate DuckDB database off-path with community, automatic,
and unsigned extension loading disabled and one execution thread. A failed
load or error-level quality gate leaves the published warehouse untouched.

After validation, the builder exports 22 curated relations as sorted,
Zstandard-compressed Parquet files. Each immutable snapshot contains a
manifest with source provenance, relation row counts, file hashes, and schema
hashes. The completed DuckDB file is replaced atomically, followed by a
separate atomic replacement of the lake's `current.json` pointer.

## Build and inspect

Run commands from the `dsco-cli` repository root. The full local build used for
the current snapshot is:

```bash
python3 scripts/chimera_router/fusion.py build \
  --include-process-metrics \
  --include-incident-metrics \
  --include-swarm-results
```

Those three flags opt into large or mutable observability sources. Without
them, the builder still ingests readable default SQLite stores, the pinned
OpenRouter catalog, the auto-discovered local oh-my-pi catalog, reviewed
provider metadata, transcript/tool metrics, and governed run manifests/WAL.

Inspect the published DuckDB without rebuilding it:

```bash
python3 scripts/chimera_router/fusion.py inspect
```

Additional immutable inputs are explicit and repeatable:

```bash
python3 scripts/chimera_router/fusion.py build \
  --endpoint-json /path/to/openrouter-endpoints.json \
  --benchmark-jsonl /path/to/normalized-benchmarks.jsonl \
  --preference-jsonl /path/to/normalized-preferences.jsonl
```

The builder never fetches these inputs. External benchmark and preference
JSONL must already conform to the normalized contracts; a file is rejected if
a row contains raw-content fields such as prompts, responses, messages,
conversations, text, or content.

## Published artifacts

- DuckDB read model: `~/.dsco/chimera_router/fusion.duckdb`
- Immutable snapshots:
  `~/.dsco/chimera_router/fusion_lake/snapshots/<fusion_snapshot_id>/`
- Current pointer:
  `~/.dsco/chimera_router/fusion_lake/published/current.json`
- Source registry: `scripts/chimera_router/fusion_sources.json`
- Typed schema: `scripts/chimera_router/fusion_schema.sql`

The live warehouse inspected on 2026-08-14 uses schema
`chimera-router-fusion-v2`. It contains 36 registered sources, 14 ingested
source snapshots, and 3,013,611 declared source rows. Its current fusion
snapshot is:

```text
f32f306c0d2677f1262dad5b6d106b21f3ca74cf2a736e7425a4d4ca218c4787
```

That snapshot contains 22 Parquet relations and 634,237 exported relation
rows. The manifest SHA-256 is
`e5d259e9ab97956aefa31380202d21fde8b08bd28d3317a2748d9bffaa92a79c`.
The DuckDB SHA-256 at inspection time is
`272677a43a0e967ce4398a459edb639c9493022643223f0c02899c5d60afc945`.

Selected live relation counts are:

| Evidence lane | Rows |
| --- | ---: |
| Baseline events | 297,857 |
| Chronicle events | 387,077 |
| Runtime process metrics | 75,529 |
| Child process metrics | 1,526,663 |
| Runtime incidents | 213,274 |
| Transcript turn metrics | 14,124 |
| Tool trace metrics | 33,539 |
| Run manifests / valid WAL events | 8,453 / 123,145 |
| OpenRouter catalog models | 547 |
| Native model offers / inference providers | 3,814 / 59 |
| Benchmark observations / local evaluations | 434 / 274 |
| Swarm child results / multi-model groups | 13,486 / 281 |

`declared_source_rows` is the sum of adapter-level snapshot counts. It is not a
count of every raw and derived DuckDB row, and the Parquet row total covers
only the curated export relations.

## Data model

The core lineage tables are `source_registry`, `source_snapshot`,
`warehouse_metadata`, and `data_quality_result`. Every ingested source has a
content hash, capture time, schema version, high-water mark where available,
and adapter-specific provenance.

Important typed fact tables include:

- `baseline_event`, `chronicle_event`, `openrouter_eval`,
  `route_decision`, and the append-only `route_outcome_revision` for
  operational evidence.
- `catalog_model_snapshot`, `native_model_offer_snapshot`,
  `provider_endpoint_snapshot`, and `provider_capability_snapshot` for
  model/provider availability, capabilities, and quoted prices.
- `benchmark_observation` and `preference_observation` for normalized quality
  evidence with explicit provenance.
- `runtime_process_metric`, `child_process_metric`, `runtime_incident`,
  `transcript_turn_metric`, `tool_trace_metric`, `run_manifest_snapshot`, and
  `run_wal_event` for reliability, cost, latency, and execution behavior.
- `swarm_child_result` for hashed, process-level child-agent observations.

The most useful analytical views are:

- `model_provider_offer`: latest native and OpenRouter endpoint offers with
  operational status kept separate from pricing status.
- `model_universe` and `current_routable_model_universe`: historical identity
  coverage versus only the latest catalog/offer snapshots.
- `model_evidence`: catalog descriptors joined to historical outcomes, local
  evaluations, and benchmark evidence.
- `route_training_fact`: decisions joined to the latest outcome revision and
  the catalog snapshot available at decision time.
- `run_execution_fact`: manifest lifecycle, Chronicle identity, valid WAL
  counts, final receipt totals, and stale-running derivation.
- `process_health_hourly`, `incident_daily`, and
  `incident_instance_match`: aggregated health and conservative PID-interval
  attribution.
- `swarm_comparison_group`: non-truncated, non-degenerate multi-model groups
  that can support comparative operational analysis.
- `deduplicated_tool_trace` and `enriched_transcript_turn`: metric-level
  deduplication and best-effort process-instance joins.

Exact native identity is the case-sensitive pair
`(inference_provider_id, model_id)`. Normalized model IDs are join helpers, not
primary identity. Native zero prices are marked `zero_unspecified`, not assumed
free, and dynamic or sentinel prices are unavailable rather than fabricated.

## Privacy boundary

The warehouse is metrics-first. Adapters intentionally omit or hash:

- raw prompts, responses, messages, transcript bodies, tasks, swarm outputs,
  and free-form errors;
- tool arguments and result previews;
- process command lines, executables, working directories, host names, paths,
  crash excerpts, and debugger backtraces;
- provider auth headers, base URLs, compatibility payloads, and signing
  endpoints.

Presence flags, byte counts, stable content hashes, token/cost/latency fields,
and constrained categorical values remain when analytically useful. Hashes,
request feature vectors, model identifiers, and timing correlations are still
sensitive telemetry; this is data minimization, not anonymization.

## Trust and current limitations

- Incident data is operational reliability evidence. Direct model attribution
  is allowed only when an incident's child PID matches an active baseline
  instance interval. Supervisor-only matches are lower-confidence context.
- Swarm files are mutable last-write-wins slots. Their model is requested, not
  independently verified; cost is reported-or-estimated; file modification
  time is only an approximate completion time; and semantic labels are always
  unavailable. Use qualifying groups for operational comparison, never as
  proof that an answer was correct.
- Public preference and benchmark sources are registered with provenance,
  trust tier, license fields, and adapter contracts, but are not automatically
  downloaded or ingested.
- The current live warehouse has zero provider endpoint snapshots, zero
  normalized preference observations, zero route decisions, and zero outcome
  revisions. It therefore does not yet provide the fresh endpoint or
  production counterfactual evidence needed for router promotion.
- The 434 current benchmark observations are catalog-derived weak evidence;
  benchmark quality, human preference, operational success, and provider
  reliability remain separate evidence lanes.

## Reproducibility and quality gates

The fusion snapshot ID hashes the fusion schema version and hash plus the
ordered source content hashes, capture times, schema versions, and high-water
marks. Rebuilding the same logical inputs under the same schema resolves to
the same snapshot ID. The immutable directory preserves the original Parquet
files; its manifest records each file's SHA-256 and relation-schema SHA-256.
`published/current.json` binds the active snapshot ID to the manifest hash.

This is logical/source reproducibility, not a promise that an arbitrary future
DuckDB version will produce a byte-identical database file. Pin the source
snapshots, schema, DuckDB version, manifest, and high-water marks when an
experiment must be replayed.

The current build passes all 13 quality gates with no failed errors or
warnings. Gates cover source registration and presence, key uniqueness,
referential integrity, nonnegative operational domains, provider/native offer
domains, preference validity, catalog as-of temporal integrity, WAL-to-manifest
integrity, observed-model catalog coverage, and Chronicle coverage for primary
run manifests. `--strict-warnings` promotes warning-gate failures to a failed
build.
