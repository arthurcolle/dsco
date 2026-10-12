# Durable work and provider cost frontiers

The companion `dspy_multidimensional_reasoning_and_cognitive_bias_reduction`
repository now supplies `cognitive_agent.planning`, `executor`, and `decomposition`.
Its `cognitive_agent/PLANNING.md` documents commands and constraints. These are
explicit components, not an automatic replacement for the native DSCO planner.

The SQLite planner separates the steering inbox from dependency-ready work.
Claims apply pending steering transactionally, inherit parent dependency gates,
and use expiring attempt tokens. Cancellation fences stale completions. Group
completion requires checked child outcomes plus explicit group evidence. Structured
DSPy decomposition imports validated plans atomically and retains safe accounting
metadata even for rejected proposals. Missing cost is unknown.

The bounded executor invokes the actual DSCO binary, observes steering, cleans up
owned process groups, and retains per-attempt output, costs, and validation receipts.
It defaults to tool-free analysis; callers supply an outcome verifier. Native
capability grants remain enforced. Lease fencing does not guarantee exactly-once
external side effects. This version has no lease renewal; keep each execution
shorter than its lease and decompose longer work into bounded tasks.

Native improvements include live IPC steering at model boundaries, hierarchical
ready-frontier fixes, immutable targeted-queue destinations across lease recovery,
and `swarm {"action":"health"}`. Health is read-only, reports unknown progress
explicitly, and never kills workers automatically. Legacy native completion-by-ID
is not upgraded to the companion planner's attempt-token CAS contract.

## Compare cost without inventing savings

```sh
python3 scripts/provider_cost_frontier.py \
  --catalog reports/endurance-swarm-20260905/catalog/openrouter-models.json \
  --quotes reports/endurance-swarm-20260905/catalog/first-party-quotes.json \
  --empirical reports/endurance-swarm-20260905/frontier-summary.json \
  --input-tokens 8000 --output-tokens 500 --require-tools \
  --output /tmp/dsco-frontier.json
```

Input tokens above are uncached; `--cached-tokens` adds cached input. Scenario
comparison preserves provider, quote channel, observation time, source, time bands,
request fees, context limits and unknown prices. It never changes routing. A catalog
Pareto frontier is not evidence that a route is accessible or its answers correct.
Pilot success intervals cannot establish production reliability.

Direct DeepSeek accounting now uses its official cached pricing table and UTC time
bands when available. Marketplace fallback remains explicitly labeled as a reference.
Background refresh is bounded and retains the last valid table on fetch/schema failure.
Provider-reported costs take precedence; estimates and subscription reference costs
remain retained separately. Historical raw records are never silently repriced.

For long sessions, optimize verified work per dollar, reuse stable prompt prefixes,
and keep idle scheduling local. Use a stronger model when measured retries or failed
validation erase a cheap model's savings. Cheap/free quotes do not establish quota,
throughput, zero compute cost, or unlimited runtime. See the campaign report for
measured provider failures and the limits of the endurance proof.

Full model/endpoint/product expansion is documented in [Provider lanes](PROVIDER_LANES.md).
