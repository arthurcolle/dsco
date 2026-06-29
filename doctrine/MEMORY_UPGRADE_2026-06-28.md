# MEMORY doctrine — primary-source upgrade (2026-06-28)

Grounding the 3-tier MEMORY doctrine (Working 60s / Episodic 1h / Semantic ∞) in
three canonical agent-memory papers fetched + extracted this session (honest API path).
Evidence tier: [analyzed] from primary text, not [recalled].

## Sources (on disk /tmp/pdfs/, verified)
- MemGPT, arXiv:2310.08560 — OS-style tiered memory + paging.
- Generative Agents, arXiv:2304.03442 — memory stream + retrieval scoring + reflection.
- Voyager, arXiv:2305.16291 — skill library as consolidated procedural memory.

## What the papers do that my doctrine does NOT
| Mechanism (source) | My current doctrine | Gap / upgrade |
|---|---|---|
| **Retrieval score = α₁·recency + α₂·importance + α₃·relevance**, normalized 0–1, recency = exponential decay (Generative Agents) | Promotes by "importance" only, undefined | Adopt the 3-term weighted score. Decay I already have; importance+relevance I don't compute. |
| **Reflection fires when Σ importance > threshold** (Generative Agents) | Consolidation is time-based (60s/1h) | Add an *importance-triggered* consolidation, not just time-triggered. Surprise/cost spikes should flush early. |
| **Memory-pressure eviction + paging** (MemGPT): warn → evict least-relevant → page to archival, iteratively recallable | Tiers exist but eviction policy is unspecified | Define eviction by *relevance*, page to durable store, keep it recallable (= context_recall). |
| **Skill library = consolidated procedural memory**, retrieved by embedding, grown by add_skill (Voyager) | Semantic tier is declarative only | Treat the skills/ dir AS the procedural-memory tier; consolidation = skill creation. Already half-built. |

## Concrete upgrade (proposed, reversible)
1. **Replace** "promote important memories" with computable:
   `keep_score = 0.34·decay(age) + 0.33·importance + 0.33·relevance(query)`; promote top-K.
2. **Add** importance-triggered consolidation: when Σ importance since last flush > θ,
   consolidate now regardless of the 60s/1h clock.
3. **Define eviction**: on context pressure (saw 82.5% this session), evict lowest
   relevance·recency to durable store; recall on demand — exactly what CHECKPOINT did today.
4. **Name** skills/ as the procedural-memory tier in MEMORY doctrine; close the Voyager gap.

## Dalio-consistent note
These are upgrades derived from *observed failure* (my context hit 82.5%, compaction
freed 0 turns — a real consolidation failure this session) + *primary track record*
(three deployed systems), not authored-then-scored prose. This is the corrected method.
