# Chimera Router v2

For the operator training plan—from governed dataset construction through
cross-provider swarm promotion—see
[ADVANCED_TRAINING_CURRICULUM.md](ADVANCED_TRAINING_CURRICULUM.md).

Chimera Router is a local, candidate-conditioned scorer and declarative planner
for a changing OpenRouter catalog. It does not classify requests into a frozen
list of model IDs. The same weights can score a future model from its live
descriptor, subject to the artifact's versioned feature contract.

The stable boundary is:

```text
request + current catalog + policy
    -> hard capability and budget filters
    -> operational ensemble + semantic quality ensemble
    -> calibrated score, uncertainty, live price, and current benchmark policy
    -> direct route or bounded declarative orchestration plan
```

The router and service never hold an OpenRouter key, call a model, or spend
money. They return model IDs, fallback chains, and plans for an explicit caller
to execute.

## What v2 learns

The operational router is a rank-24 NumPy two-tower model conditioned on both
the request and each candidate descriptor:

- A 512-dimensional request vector combines stable numeric flags with signed
  BLAKE2b hashes of words, local n-grams, and character classes.
- A 128-dimensional semantic model descriptor combines context, modalities,
  supported parameters, reasoning behavior, live price fields, provider and
  coarse family, and hashed words/bigrams from the catalog name and
  description. A complete model ID is never used as one identity feature.
- Operational heads learn terminal completion, provider failure, tool error,
  historical cost, historical latency, and same-prompt preference. Terminal
  completion is not treated as proof that an answer was semantically correct.
- Five seeded prompt-group bootstrap members are the default. Binary heads are
  calibrated only on validation rows. Routing exposes member disagreement,
  support distance, quality uncertainty, and their combined uncertainty.

This descriptor contract is what enables cold-start scoring for a new catalog
entry. It does not make an unseen model known: sparse or promotional metadata,
new modalities, and distribution drift still produce uncertainty and require
fresh evaluation.

### Separate semantic-quality lane

V2 trains a second artifact with general-intelligence, coding, and agentic
quality heads. Its labels are weak supervision from OpenRouter's Artificial
Analysis fields, normalized from `[0, 100]` to `[0, 1]`. This lane is never
concatenated with historical operational outcomes. Because these targets are
continuous weak scores rather than binary events, the quality artifact uses
identity calibration; validation-only binary calibration belongs to the
operational artifact.

To prevent direct label leakage, the quality feature matrix is recomputed with
all benchmark-derived descriptor columns zeroed. At route time, a current
catalog benchmark remains authoritative. When that benchmark is absent, the
semantic ensemble is routed by the lower confidence bound
`max(mean - quality_lcb_z * ensemble_std, 0)`, with `quality_lcb_z = 1.0` by
default, rather than by its raw mean. The three heads are weighted according to
whether the request looks general, coding, agentic, or both coding and agentic.

## Pinned v2 dataset

The current reproducible snapshot is
`~/.dsco/chimera_router/catalogs/openrouter-2026-08-13.json`, pinned by content
hash and represented as an all-modality catalog of 547 candidates. The dataset
contains descriptor and quality-lane rows for all 547 entries; the default
routing policy still requires text input and text output. It also defaults to
`allow_additional_output_modalities = false`, so a media generator advertising
`text+audio` or `text+image` output is rejected for a text-only route even when
the auxiliary text technically satisfies the text requirement. Callers must
opt in when extra output media are genuinely acceptable.

Nested OpenRouter virtual models such as `openrouter/*` hide the ultimately
selected model from Chimera's own cost and quality accounting. They therefore
default to `allow_router_models = false` and require an explicit policy opt-in.

At inclusive telemetry high-water mark `events.id = 297769`, the local v2
artifact contains 10,608 operational rows, 62,449 exact-prompt pairs, and
8,478/1,188/942 temporal train/validation/test rows. Those operational rows
cover 247 observed models, 43 providers, and 163 coarse families. The weak
quality lane has at least one label for 160 of the 547 catalog entries and 434
observed label cells. The remaining candidates are genuine metadata-driven
cold starts, not secretly labeled examples.

The pinned catalog SHA-256 is
`955e87756521775ea1c96bf7cdded04f22e318718545962ecd9124658908e888`; the
deterministic v2 dataset SHA-256 for the command below is
`383d7999b98c6cca562344065cee305f61251efb36301c1389f0d4cd63490be2`.

No raw prompt or response strings are emitted. Prompt hashes and hashed feature
vectors are data-minimizing, not anonymous, and must be governed as sensitive
telemetry.

## Reproducible build and training

Run commands from the `dsco-cli` repository root.

Build a deterministic, pickle-free dataset and JSON sidecar from the pinned
catalog and inclusive event high-water mark:

```bash
python3 scripts/chimera_router/build_dataset.py \
  --db ~/.dsco/baseline.db \
  --catalog ~/.dsco/chimera_router/catalogs/openrouter-2026-08-13.json \
  --max-event-id 297769 \
  --output ~/.dsco/chimera_router/router_dataset_v2.npz
```

Train the default five-member operational ensemble and its separate quality
ensemble:

```bash
python3 scripts/chimera_router/ensemble.py \
  ~/.dsco/chimera_router/router_dataset_v2.npz \
  --metadata ~/.dsco/chimera_router/router_dataset_v2.json \
  --output ~/.dsco/chimera_router/chimera_router_v2.npz \
  --quality-output ~/.dsco/chimera_router/chimera_router_v2.quality.npz \
  --members 5 --rank 24 --epochs 200 --batch-size 512 \
  --learning-rate 0.005 --weight-decay 0.00001 --patience 30 \
  --pair-loss-weight 0.25 --quality-split family --seed 7 --quiet
```

Training is seeded and both dataset and ensemble writers use deterministic,
pickle-free NPZ serialization. Bit-for-bit equality should still be verified
on the target NumPy/Python platform rather than assumed across arbitrary future
numeric runtimes.

## Fail-closed evaluation and promotion

Generate the machine-readable promotion report with temporal evaluation plus
fresh, split-disjoint model/provider/family holdout retraining:

```bash
python3 scripts/chimera_router/evaluate.py \
  ~/.dsco/chimera_router/router_dataset_v2.npz \
  --metadata ~/.dsco/chimera_router/router_dataset_v2.json \
  --ensemble ~/.dsco/chimera_router/chimera_router_v2.npz \
  --quality-ensemble ~/.dsco/chimera_router/chimera_router_v2.quality.npz \
  --promotion-report ~/.dsco/chimera_router/promotion_report_v2.json \
  --diagnostic-members 3
```

Promotion is conjunctive and fail-closed. The report checks artifact/dataset
provenance, temporal pair coverage, validation-only calibration, test
calibration non-regression, strict cold-start partition integrity, learned
policy margin over the cheapest, validation-majority, validation-model-prior,
and current-heuristic baselines, and leakage-free quality RMSE. If any required
gate fails, the report says `"decision": "hold"` and the command exits `2`.

For diagnostics only, append `--allow-failed-promotion` to write and inspect a
HOLD report without a nonzero process exit. That flag changes only the exit
code; it never changes `promotion.passed` or the decision. Treat every v2
artifact as HOLD until its exact report passes every gate.

## One-shot route

```bash
python3 scripts/chimera_router/route.py \
  --router ~/.dsco/chimera_router/chimera_router_v2.npz \
  --quality-router ~/.dsco/chimera_router/chimera_router_v2.quality.npz \
  --catalog ~/.dsco/chimera_router/catalogs/openrouter-2026-08-13.json \
  --prompt 'Implement and test a C parser.' \
  --prompt-tokens 1500 --completion-tokens 1200 --top-k 5
```

For hard requirements, pass a request JSON file with `--request` and an
optional policy JSON file with `--policy`:

```json
{
  "messages": [{"role": "user", "content": "Implement and test a C parser."}],
  "estimated_prompt_tokens": 1500,
  "estimated_completion_tokens": 1200,
  "required_modalities": ["text"],
  "required_output_modalities": ["text"],
  "allow_additional_output_modalities": false,
  "allow_router_models": false,
  "quality_lcb_z": 1.0,
  "required_parameters": ["tools", "structured_outputs"],
  "min_context": 32768,
  "max_cost_usd": 0.02
}
```

Automatic routing filters infeasible candidates before utility ranking. Hard
constraints include input/output modalities, supported parameters, context,
known live price, per-request cost and latency ceilings, a failure-probability
ceiling, a quality floor, model/provider allow and deny lists, region,
zero-data-retention availability, expiration/status, and interactive batch
variants. Extra output modalities and nested `openrouter/*` routers are also
rejected by default. Live catalog prices take precedence over historical cost
predictions, and live benchmark quality takes precedence over the learned
lower-confidence-bound fallback.

An explicit `--model` override remains authoritative for the route endpoint and
is returned with `override_violations` when it breaks a constraint. The planner
is stricter and refuses to construct an executable-looking plan around an
override that violates hard constraints.

## Loopback service

Start the service with both ensembles and, optionally, the feedback store:

```bash
python3 scripts/chimera_router/serve.py \
  --router ~/.dsco/chimera_router/chimera_router_v2.npz \
  --quality-router ~/.dsco/chimera_router/chimera_router_v2.quality.npz \
  --catalog ~/.dsco/chimera_router/catalogs/openrouter-2026-08-13.json \
  --feedback-db ~/.dsco/chimera_router/feedback_v3.db \
  --host 127.0.0.1 --port 8787
```

Access logging is disabled unless `--access-log` is supplied. The operational
artifact, quality artifact, and catalog are fully loaded and validated before
an atomic reload is published.

### Endpoints

- `GET /healthz` returns schema versions, artifact/catalog hashes, dimensions,
  ensemble member counts, catalog size, and whether feedback is enabled.
- `POST /v1/route` returns one selected model, fallback models, scored
  candidates, uncertainty, and an `openrouter_request_patch`.
- `POST /v1/plan` returns a bounded direct, cascade, or parallel plan and never
  executes it.
- `POST /v1/outcome` records or revises a delayed outcome when feedback is
  enabled.
- `POST /v1/reload` forces a validated reload and returns current health.

A route request may be the OpenAI-style object directly or a wrapper:

```bash
curl -sS http://127.0.0.1:8787/v1/route \
  -H 'content-type: application/json' \
  -d '{
    "request": {
      "messages": [{"role": "user", "content": "Implement a JSON parser."}],
      "estimated_prompt_tokens": 1200,
      "estimated_completion_tokens": 800
    },
    "policy": {"required_parameters": ["tools"], "max_cost_usd": 0.01},
    "top_k": 5
  }'
```

The response includes an explicit patch for a separate OpenRouter caller:

```json
{
  "selected_model": "...",
  "fallback_models": ["..."],
  "openrouter_request_patch": {
    "models": ["selected", "fallback-1", "fallback-2"],
    "provider": {"allow_fallbacks": true, "require_parameters": true}
  }
}
```

## Declarative orchestration planner

The safe default is one direct model call. Cascade and parallel plans require
both an explicit call allowance and an explicit cost ceiling:

- `direct`: one router-ranked model call.
- `cascade`: a cheaper draft call, deterministic local checks, and a declared
  escalation model called only when those checks fail. It requires
  `max_calls >= 2`.
- `parallel`: two diverse candidate calls followed by a judge when measured
  route risk and estimated quality gain clear their thresholds. It requires
  `max_calls >= 3`.

```bash
curl -sS http://127.0.0.1:8787/v1/plan \
  -H 'content-type: application/json' \
  -d '{
    "request": {
      "messages": [{"role": "user", "content": "Implement and verify a parser."}],
      "required_parameters": ["tools"]
    },
    "planner": {
      "strategy": "auto",
      "max_calls": 2,
      "max_expected_cost_usd": 0.03,
      "max_worst_case_cost_usd": 0.05,
      "local_verifiers": ["tests_pass", "schema_valid"]
    },
    "top_k": 5
  }'
```

Every ready or refused planner response includes
`"declarative_only": true`, `"executes_models": false`, and
`"requires_explicit_execution": true`. It reports roles, stages, triggers,
expected/worst-case calls, cost and latency, fallbacks, and selection reasons.
If no plan satisfies a hard call, cost, or latency limit, it returns
`"status": "refused"` with no stages. Requesting cascade or parallel does not
force one: an inadmissible advanced plan falls back to the safe direct plan
when that direct plan satisfies all hard limits.

## Optional feedback loop

Passing `--feedback-db` initializes or non-destructively migrates the SQLite
store to schema v3. For each recorded `/v1/route`, the service internally stores
the full scored candidate exposure while returning only the requested `top_k`.
It stores a canonical request SHA-256, the versioned 512-value task feature
vector, checkpoint/catalog/policy IDs, chosen model, fallbacks, propensities,
and structured candidate predictions. It does not store raw prompt or response
text.

Feedback recording defaults on when the service has a feedback DB. A caller
can set `"record_feedback": false` in a route wrapper for a request that must
not be persisted.

The route response contains a `request_id`. Record a delayed result with:

```bash
curl -sS http://127.0.0.1:8787/v1/outcome \
  -H 'content-type: application/json' \
  -d '{
    "request_id": "ROUTE_REQUEST_ID",
    "actual_model": "provider/model",
    "http_success": true,
    "task_success": true,
    "tool_valid": true,
    "prompt_tokens": 1200,
    "completion_tokens": 640,
    "cost_usd": 0.0042,
    "e2e_ms": 1830,
    "label_source": "production_validator",
    "label_confidence": 0.95
  }'
```

Each outcome write appends an immutable row to the schema-v3
`route_outcome_revisions` ledger and returns its `revision_id`. A correction
appends a new revision linked to the one it supersedes; it never mutates or
deletes the historical revision. This makes “latest revision for each request
as of revision H” deterministic and replayable.

To merge captured operational feedback into a dataset, pin both the baseline
event high-water and the immutable feedback revision high-water:

```bash
python3 scripts/chimera_router/build_dataset.py \
  --db ~/.dsco/baseline.db \
  --catalog ~/.dsco/chimera_router/catalogs/openrouter-2026-08-13.json \
  --max-event-id 297769 \
  --feedback-db ~/.dsco/chimera_router/feedback_v3.db \
  --max-feedback-revision-id REVISION_HIGH_WATER \
  --output ~/.dsco/chimera_router/router_dataset_v2_feedback.npz
```

The command summary reports `feedback_max_revision_id` and
`feedback_snapshot_sha256`. On a replay, add the logical snapshot hash as a
fail-closed content guard. Replace `REVISION_HIGH_WATER` with the reported
integer and `LOGICAL_SNAPSHOT_SHA256` with the reported 64-character hash:

```bash
python3 scripts/chimera_router/build_dataset.py \
  --db ~/.dsco/baseline.db \
  --catalog ~/.dsco/chimera_router/catalogs/openrouter-2026-08-13.json \
  --max-event-id 297769 \
  --feedback-db ~/.dsco/chimera_router/feedback_v3.db \
  --max-feedback-revision-id REVISION_HIGH_WATER \
  --expected-feedback-snapshot-sha256 LOGICAL_SNAPSHOT_SHA256 \
  --output ~/.dsco/chimera_router/router_dataset_v2_feedback.npz
```

The importer maps `http_success` to the operational `completion_success` head,
plus `provider_failure`, `1 - tool_valid`, `cost_usd`, and `e2e_ms / 1000` to
their matching historical heads. It deliberately does not map `task_success`:
semantic task success is a different target from reaching a terminal HTTP
response. Imported rows carry source, per-label provenance, opaque source ID,
confidence, and censoring arrays. An unchosen candidate remains unknown, not a
negative label.

Without `--feedback-db`, the original baseline-only dataset path and SHA-256
remain byte-for-byte unchanged. Schema-v2 stores are supported only as a
compatibility path: their mutable outcome-row cutoff must be paired on replay
with `--expected-feedback-snapshot-sha256`. New collection should use the
schema-v3 revision high-water. Feature vectors and request hashes remain
sensitive telemetry even without raw text.

## Data fusion warehouse

The optional fusion builder snapshots the local SQLite OLTP stores, projects
redacted typed facts into a DuckDB OLAP read model, runs fail-closed quality
gates, and publishes a content-addressed immutable Parquet snapshot. A full
local refresh, including the large process, incident, and swarm metric lanes,
is:

```bash
python3 scripts/chimera_router/fusion.py build \
  --include-process-metrics \
  --include-incident-metrics \
  --include-swarm-results
python3 scripts/chimera_router/fusion.py inspect
```

The builder keeps raw prompts, responses, tasks, swarm outputs, tool payloads,
command lines, and crash excerpts out of the warehouse. Public benchmark and
preference sources are registered but are never downloaded automatically.
See [FUSION.md](FUSION.md) for the source contracts, tables/views, live
artifact paths, current coverage gaps, privacy boundary, and reproducibility
model.

## Honest scope and longevity

- The 547-entry catalog is coverage, not 547 equally supervised models. Most
  candidates are cold starts and weak quality coverage is sparse.
- Artificial Analysis is a useful but narrow and potentially drifting proxy;
  it is not task-specific ground truth or a substitute for acceptance tests.
- Historical DSCO outcomes are observational and confounded by provider drift,
  retries, subscriptions, tool loops, and changing prices.
- Uncertainty is a routing signal, not a formal guarantee of correctness or
  calibrated out-of-distribution risk.
- A future model can be scored without adding a class, but the catalog schema,
  feature versions, prices, policies, and promotion evidence must still be
  refreshed. No 2026 checkpoint can honestly be promised unchanged for 15
  years.
- The service is a local scorer/planner, not an execution proxy, provider
  health oracle, or automatic paid all-model benchmark sweep.

The durable parts are the versioned request/descriptor/feedback contracts,
candidate-conditioned scoring, hard policy boundary, deterministic artifacts,
fail-closed promotion report, and explicit execution boundary—not a frozen set
of weights.

## Tests

```bash
python3 -m py_compile scripts/chimera_router/*.py
python3 -m unittest discover -s scripts/chimera_router -p 'test_*.py' -v
```
