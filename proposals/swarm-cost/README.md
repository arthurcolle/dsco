# Cheaper Swarms — cost-optimization layer

**Status:** working simulation (`swarm_cost_optimizer.c`, verified) + integration plan.
**Source of truth:** `~/.dsco/RUNTIME_INTELLIGENCE.md` (observed $3,643 lifetime spend).

## The problem (from real runtime data)
1. Children re-send the same big context prefix at **full price** — observed cache_read_ratio
   0.097 (gpt-5.5), 0.075 (kimi). The frozen prefix is paid for N times.
2. Every child runs on **one premium model** regardless of subtask difficulty.
3. Speculative race pays for N lanes, keeps 1 — **275 forced kills** = paid work discarded.

## Three levers (per-lever attribution, isolated not stacked)

| Lever | Mechanism | Independent saving |
|-------|-----------|-------------------:|
| **1. Shared frozen-prefix cache** | Parent warms the system+context prefix once; children pay cache-read (~0.1×) on it | **−72%** |
| **2. Cost-aware routing** | Route each subtask to cheapest model ≥ its capability bar | **−81%** |
| **3. EV-gated race** | Only race when latency value > wasted spend; else single lane | eliminates race waste |
| **All three combined** | | **−92%** vs realistic baseline |

Headline vs worst-case baseline (premium + no-cache + 4-lane race): **−95.5%** ($1.94 → $0.087 on a 4-child swarm).

Conservative fleet extrapolation: if 40% of lifetime spend is cacheable prefix, the reduction rate implies **~$1,390 saved** historically — and compounding forward.

## Integration plan (maps to src/swarm.c)

### Lever 1 — shared frozen-prefix cache
- **Hook:** `src/swarm.c:1251 swarm_prepare_executor_env` + the child `setenv` block (~385–557).
- Parent computes the frozen-prefix hash once (mechanism already exists:
  `DSCO_CACHE_PARENT_FROZEN_*`, commit c9a36b4). Extend so the parent **warms**
  the provider cache before fork, then every child inherits the same breakpoint
  and pays cache-read, not full input.
- Provider cache breakpoints already implemented per `include/openrouter_cache.h`
  and the anthropic cache-breakpoint tests in `tests/test.c`.

### Lever 2 — cost-aware routing
- **Hook:** child model selection in `swarm_spawn_*` (the `-m` arg / `DSCO_MODEL`).
- Use existing router: `dsco_route_optimal(budget_per_1m, min_ctx)` and
  `dsco_route_cheapest_tool()` in `openrouter_cache.h`.
- Add a per-subtask `min_capability` field to the swarm task struct; route to the
  cheapest catalog model that clears it. Premium models only for high-bar subtasks.

### Lever 3 — EV-gated race
- **Hook:** `provider_fabric` race path + `swarm_enforce_budgets` (src/swarm.c:1006).
- Before racing N lanes, compute EV: `race iff (E[latency_saved]·latency_value) >
  (N-1)·expected_lane_cost`. Otherwise run one lane.
- Reduces the 275 forced-kill waste directly.

## Files
- `swarm_cost_model.h` — **shared SUT**: catalog, `scm_turn_cost`, `scm_route_cheapest`, `scm_should_race`
- `swarm_cost_optimizer.c` — the demo/attribution (uses the shared header)
- `swarm_cost` — built binary; `./swarm_cost` prints the attribution table
- `swarm_cost_props.c` — **property harness** (pi/prop-substrate architecture)
- `swarm_cost_props` — built binary; `./swarm_cost_props [N]`
- `swarm_cost_props.lean` — the same invariants as Lean theorems (proof-swarm goals)

## Property harness (proof-substrate architecture)
Reuses `prop_harness.c`'s three gates — splitmix64 determinism, FNV-1a distinctness,
mutation-kill — pointed at the shared cost model. Because the demo and the harness
call the SAME `swarm_cost_model.h`, a property that passes IS a property of the
model the demo reports.

| Property | Invariant | kill rate |
|----------|-----------|----------:|
| P1 cache_monotone | cached prefix cost ≤ uncached | 100% |
| P2 cache_bound | cached ≤ 0.15·prefix + rest (the −85% claim) | 100% |
| P3 route_optimal | router returns true argmin over capable models | 39% |
| P4 route_capable | routed model always clears the capability bar | 85% |
| P5 race_gate | never race when waste ≥ value | 62% |

Verified: **500K real tests in 0.2s**, 449,951 distinct inputs, all properties
sound (0 fail on correct impl) AND killing (mutants caught). `status: REAL (not theater)`.

```bash
cc -O2 -std=c11 -o swarm_cost_props swarm_cost_props.c && ./swarm_cost_props 100000
```

Each property is falsified at scale here BEFORE it becomes a Lean goal in
`swarm_cost_props.lean` — test-first, prove-second. This is the direct tie-in to
the 250K–450K real-tests / Coq-Lean proof-swarm goal.

## Success gates
- `swarm_cost` exits non-zero unless savings > 30% AND optimized < baseline → **exit 0**.
- `swarm_cost_props` exits non-zero unless every property is sound AND has kill power → **exit 0**.

## Next step
Wire **Lever 1** into `src/swarm.c` (parent-frozen-prefix plumbing already exists via
`DSCO_CACHE_PARENT_FROZEN_*`). The P2 property (`child_prefix_cost ≤ 0.15 × full`)
is now a live regression guard: if the wiring drifts, the harness fails and the
Lean goal is false.
