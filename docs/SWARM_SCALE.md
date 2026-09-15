# Repeat-aware swarm scaling

`swarm` now accepts `action: "scale"`. It admits up to 4,096 logical tasks, hashes
exact task descriptors, and submits only the unique descriptors to native `create`.
Logical tasks receive shared worker references rather than duplicate inference.
Normal `create`, independent trials and competition swarms are unchanged.

## Use

Call inside a persistent DSCO session or MCP server so the parent stays alive to
own and collect its workers:

```json
{
  "action": "scale",
  "name": "shared-arithmetic",
  "pure_tasks": true,
  "dry_run": true,
  "budget": 0.12,
  "tasks": [
    {"task": "Compute 17*19; return the integer only.", "provider": "openai-codex", "model": "gpt-5.6-luna"},
    {"task": "Compute 17*19; return the integer only.", "provider": "openai-codex", "model": "gpt-5.6-luna"}
  ]
}
```

The pinned lane above was exercised by the live test; select an available lane
from the current registry for other deployments. `dry_run: true` returns the
rewritten create request without submitting workers. Omit it or set it false to
submit. The normal capability gate still applies to both the outer action and
nested create; planning is not an exemption from an operator's run denial.

`pure_tasks: true` is a required **caller assertion**: identical descriptors are
interchangeable computations over immutable input. Do not use it for mutations,
changing remote state, independent samples, diversity/competition trials, random
outcomes, or separate actions that merely happen to have identical text. The
runtime cannot infer purity from a prompt. Use normal `create` for those cases.

## Result contract

- `logical_tasks`, `unique_tasks`, `reused_tasks` describe the admission plan,
  **not completed or verified work**.
- `logical_to_unique_task` maps each original input position to its first-seen
  unique input position. Output order and multiplicity are preserved by this map.
- `logical_worker_ids` explicitly shares the corresponding native worker ID for
  every logical input, but only when create returns a complete positional ID list.
- `delegated` means the gate was called. `dispatch_accepted` reports that call's
  result; neither proves successful completion.
- Collect the returned `execution.group_id`, verify each unique worker result,
  then resolve all of its logical references to that result. A failed worker is
  a shared **failure**, never a manufactured success.
- If launch is partial or IDs cannot be matched, `logical_worker_ids` is null and
  `result_reconciliation_required` is true. Do not shift aliases onto surviving
  workers by assuming compacted ID arrays retain missing positions.
- Retain collected output and the mapping before worker IDs are reused. References
  belong to the current live group, not a permanent cross-run cache.
- A size-limited execution response preserves the group handle when available and
  sets `reconcile_before_retry`. Inspect that group before any retry: workers may
  already exist. Small receipt capacity is rejected before submission when known.

The whole minified descriptor is the identity: provider, model, any task-local
settings, and extra fields all participate. Object key order is retained, so
semantically equal objects in a different order may conservatively remain unique.
Strings and objects are distinct. Global context, pins, budget and other create
options are forwarded unchanged within the request. The scale layer validates
structural bounds, the task string and native-only executor; remaining create
options retain the underlying create contract. There is no semantic similarity
merge, persistent result cache, silent model downgrade or failure promotion.

## What becomes sublinear—and what cannot

Let `N` be logical requests and `U(N)` the number of unique interchangeable tasks.
Native worker submissions are `U(N)`, with ordinary byte/alias processing still
linear in `N` for bounded descriptors. When a workload has `U(N) = sqrt(N)`, its
expensive dispatch count grows as `sqrt(N)`; this does **not** establish the same
curve for billed cost or throughput on arbitrary workloads.

Measured plan families:

| Logical inputs | Unique submissions | Submission ratio |
|---:|---:|---:|
| 16 | 4 | 25% |
| 64 | 8 | 12.5% |
| 256 | 16 | 6.25% |
| 1,024 | 32 | 3.125% |
| 4,096 | 64 | 1.5625% |

A live test submitted 64 identical logical tasks, spawned **one** pinned native
worker, collected its successful `SCALE_OK` result, and resolved all 64 references.
It did not run 64 independent inferences or compare 64 billed invoices.

Distinct independent work has a linear positive-cost floor. Total cost includes
unique input/output tokens, provider prices, coordinator work, retries and local
processing. Increasing concurrency alone cannot guarantee sublinear spending.
`work_ratio` measures submissions only; cost stays explicitly unmeasured.

## Bounds and verification

The request cap is 8 MiB, 4,096 logical tasks, 32 JSON nesting levels, 128 keys per
object, and the configured physical-worker ceiling for unique tasks. Duplicate
keys, embedded NULs and non-native executors are rejected. Hash planning is
expected linear for ordinary bounded descriptors; duplicate-key validation scans
bounded key pairs and collision comparisons are not a worst-case linear guarantee.

- `make test_swarm_scale`: offline correctness and scaling-family checks.
- `python3 tests/test_swarm_scale_mcp.py --artifact-dir /tmp/scale-proof`: live
  registry/gate validation without inference.
- Add `--live` **only with inference authority/budget**: submits one native worker
  with a requested $0.12 budget, waits and collects it. Budget requests and runtime
  estimates are not verified subscription billing.

Evidence and independent-review resolution: `reports/swarm-scale-20260906/`.
