# Proof Substrate — 250K–450K real tests → Coq/Lean proof swarms

**Status:** PROPOSAL + working prototype (`prop_harness.c`, verified 300K tests @ 0.5s).

## Thesis
The only honest path to 250K–450K **real** tests is property-based / differential /
known-answer generation with **falsifiable oracles** — and those same properties are
the theorems a Coq/Lean proof swarm proves. One **Property Registry**, two backends.

```
                  ┌─ C generator  → 250K–450K executions (fast falsification)
Property Registry ─┤
                  └─ Lean/Coq emit → proof goals → proof swarm (slow verification)
```

## The definition of a REAL test (enforced, not asserted)
> 1 real test = one (distinct input → SUT → oracle that CAN FAIL for a wrong impl)

Three gates the harness measures, not claims:
1. **Distinctness** — inputs deduped by FNV-1a fingerprint; we report distinct %, not raw count.
2. **Kill power** — a mutant (deliberately broken impl) MUST fail. Suite quality = mutation kill rate, not size.
3. **Determinism** — splitmix64 seeded → identical corpus every CI run (reproducible, bisectable).

## Prototype results (`prop_harness 300000`)
| metric | value |
|--------|-------|
| executions | 300,000 |
| distinct inputs | 287,623 (95.9%) |
| oracle failures (correct impl) | 0 ✅ |
| mutant failures caught | 293,806 (97.9% kill) ✅ |
| wall time | 0.52s |
| throughput | ~580K tests/sec/core |

At 580K/sec/core, **450K tests ≈ 0.8s single-core**, seconds across a swarm.

## Property classes (each is a test generator AND a theorem)
| class | example | Lean goal |
|-------|---------|-----------|
| roundtrip | `decode(encode(x)) = x` | `∀ x, decode (encode x) = x` |
| involution | `rev(rev(x)) = x` | `∀ x, rev (rev x) = x` |
| idempotence | `sort(sort(x)) = sort(x)` | `∀ x, sort (sort x) = sort x` |
| differential | `impl_a(x) = impl_b(x)` | `∀ x, f x = g x` |
| algebraic | `add(a,b) = add(b,a)` | commutativity/assoc/identity |
| invariant | `len(filter p x) ≤ len x` | monotonicity bounds |
| known-answer | SHA-256 NIST vectors | `hash v = expected` (decidable) |

## Why test-first, prove-second
- A property test finds a counterexample in **microseconds**; a failed proof search can burn hours.
- If QuickCheck falsifies it, the Lean goal is FALSE — never send it to the proof swarm.
- Only properties that survive N=450K falsification get promoted to proof goals. (SIGNAL / GREED: don't spend proof compute on refutable claims.)

## Scaling plan (counts are honest, distinct-input based)
| phase | properties | tests/prop | total | mechanism |
|-------|-----------|-----------|-------|-----------|
| P0 (done) | 1 | 300K | 300K | this prototype |
| P1 | 20 core primitives | 20K | 400K | base64/hex/json/utf8/bigint/sha/pipeline/eval |
| P2 | +30 differential pairs | 10K | +300K | dual impls, C vs reference |
| P3 | registry → Lean emit | — | — | proof-goal generation |
| P4 | proof swarm | N goals | — | lean/coq workers via swarm topology |

## Files
- `prop_harness.c` — working generator (base64 roundtrip + mutation gate)
- `prop_registry.lean` — the same property as a Lean theorem (bridge sketch)
- (next) `registry.json` — single source of truth, feeds both backends
