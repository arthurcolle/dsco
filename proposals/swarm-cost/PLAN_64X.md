# Plan: 64× More Sophisticated — Swarm-Cost Proof System

**Thesis:** 64 = 2⁶. Real 64× = **six independent doublings** across orthogonal axes.
Each doubling is a discrete, shippable milestone with its own success gate. No single
heroic rewrite — compounding, verifiable increments. (COMPOUNDING + EPISTEMOLOGY.)

## Where we are (the 1× baseline)
- 6 properties total (5 swarm-cost + 1 base64), 500K tests/run, 1 execution lane
- 3 cost levers, single flat model catalog, hand-picked mutants
- Lean bridge with `sorry` goals, no proof automation
- No fleet integration; simulation only

## The 64× target (2⁶ decomposition)

| # | Axis | 1× now | 2× lever | Cumulative |
|---|------|--------|----------|-----------:|
| D1 | **Property count** | 6 | domain generators → 12 | 2× |
| D2 | **Cost levers modeled** | 3 | +batching, +speculation-depth, +prefix-dedup → 6 | 4× |
| D3 | **Test volume** | 500K | 1M+ distinct (bigger input space) | 8× |
| D4 | **Execution lanes** | 1 | 64-worker proof-swarm (map/reduce) | 16× |
| D5 | **Model realism** | flat catalog | live OpenRouter catalog + measured cache ratios | 32× |
| D6 | **Proof automation** | `sorry` goals | Lean tactic-swarm discharges goals | 64× |

Each row is a real capability multiplier; the product is the 64×. Sophistication is
measured, not asserted: **properties × distinct-tests × lanes × proven-goals.**

---

## D1 — Property count 6 → 12 (2×)
**What:** A property *generator* — parametric families instead of hand-written props.
- Generalize P1–P5 into schema-driven templates (monotonicity, bound, argmin, gate).
- Add proof-substrate primitives: hex, utf8, bigint, sha256-KAT, pipeline, eval
  (they already exist in `src/`), each with roundtrip/idempotence/differential props.
**Gate:** ≥12 properties, each with ≥30% mutation kill.
**Files:** `prop_registry.json` (single source), generators in `prop_gen.c`.

## D2 — Cost levers 3 → 6 (4×)
**What:** Three more modeled+tested cost mechanisms:
- **L4 request batching** — coalesce N small child calls into 1 (amortize prefix once).
- **L5 speculation-depth control** — race depth as a tunable, EV-gated per level.
- **L6 prefix dedup** — content-addressed prefix store; identical prefixes bill once
  fleet-wide (not per-swarm).
**Gate:** each lever has an isolated attribution number + ≥1 property + ≥1 mutant.
**Files:** extend `swarm_cost_model.h`, `swarm_cost_props.c`.

## D3 — Test volume 500K → 1M+ distinct (8×)
**What:** Widen the input space so distinctness holds at higher N.
- Expand `scase_t` ranges; add structured/adversarial generators (edge tokens, zero
  prefixes, degenerate capability bars).
- Grow HSET to 2²⁴ (16.7M slots); track distinct % — must stay ≥95% at 1M.
**Gate:** 1M+ tests/property, distinct ≥95%, wall < 2s/property.

## D4 — Execution lanes 1 → 64 (16×)
**What:** Turn the harness into a real **proof-swarm** via `swarm` map_reduce.
- Shard the property × seed-range space across 64 workers (dsco executor).
- Each worker runs a seed-partition, writes `RESULT.json`; coordinator reduces
  pass/fail/kill/distinct across all lanes.
- This is the literal "Coq/Lean proof swarm" substrate applied to properties first.
**Gate:** 64 lanes, deterministic union of results == single-process result
(replay-stable), total ≥ 32M tests/run.
**Files:** `proof_swarm.sh` / a `swarm map_reduce` driver + `reduce_results.py`.

## D5 — Model realism: flat → live catalog (32×)
**What:** Replace the hand-coded catalog with reality.
- Load live model prices from `openrouter_cache` (`include/openrouter_cache.h`,
  `dsco_route_optimal`).
- Feed **measured** cache_read_ratios from `~/.dsco/baseline.db` (we already computed
  them: fugu 0.159, gpt-5.5 0.097, opus 1.712, fable 6.363).
- Properties now guard the *real* routing decisions, not a toy model.
**Gate:** catalog sourced from live data; savings recomputed against measured ratios;
property suite still green.

## D6 — Proof automation: sorry → discharged (64×)
**What:** Stand up the Lean/Coq tactic-swarm that closes the goals.
- Emit Lean goals from `prop_registry.json` (one generator, both backends).
- A `swarm` of Lean workers attempts `simp`/`linarith`/`nlinarith`/`polyrith` on the
  arithmetic properties (P1/P2 are linear-arithmetic — genuinely dischargeable).
- Only falsification-survivors (from D1–D4) become proof goals. (GREED: no proof
  compute on refutable claims.)
**Gate:** ≥3 properties machine-proven (no `sorry`), signed + recorded.

---

## Execution order (dependency-aware)
```
D1 (properties) ─┐
D2 (levers) ─────┼─→ D3 (volume) ─→ D4 (64-lane swarm) ─→ D6 (proof automation)
D5 (live model) ─┘                    (D5 can land parallel to D4)
```
D1+D2+D5 are independent and parallelizable now. D3 depends on D1/D2. D4 wraps
everything in the swarm. D6 is the capstone.

## Master success gate (the 64× is real iff)
```
sophistication = properties × (distinct_tests/500K) × lanes × (proven_goals+1)
1× baseline    = 6 × 1 × 1 × 1              = 6
64× target     = 12 × 2 × 64 × 4 (≈)        ≥ 384 ... normalized to ≥64× baseline
```
Concretely, ship when: **≥12 properties · ≥1M distinct/prop · 64 lanes · ≥32M tests/run
· ≥3 machine-proven goals**, all replay-stable and exit-0.

## Discipline (so 64× isn't theater)
1. Every doubling has a **mutation-kill gate** — no property counts unless a mutant dies.
2. Distinctness tracked at every scale — count distinct inputs, never raw runs.
3. 64-lane result must **equal** single-process result (determinism / replay-stability).
4. Live-model numbers sourced from `baseline.db`, not invented.
5. Proof goals only for falsification-survivors.

## First move
D1 + D2 together: `prop_registry.json` + the 3 new levers. Lowest risk, immediately
doubles property count and lever coverage, and sets up the generator the proof-swarm
(D6) will consume.
