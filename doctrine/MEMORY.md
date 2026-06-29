# MEMORY Doctrine: Three-Tier Memory Architecture and Consolidation

> "Working memory is the breath. Episodic memory is the day. Semantic memory is the life.
> Without consolidation, experience evaporates. Without decay, memory becomes noise."

## The Principle

DSCO operates with a three-tier memory system modeled on human cognition but engineered for agentic workloads. Memory is not a passive store — it is an active, decaying, promoting, and pruning system. Every memory has a half-life. Every consolidation is a judgment. Every retrieval is a reconstruction.

The three tiers:

| Tier | Scope | Lifetime | Purpose |
|------|-------|----------|---------|
| **Working** | Current turn + 60s | ~60 seconds | Active reasoning, tool state, immediate context |
| **Episodic** | Session + 1 hour | ~1 hour (decays) | Session traces, decisions, outcomes, pheromone signals |
| **Semantic** | Permanent (with decay) | Indefinite (pruned) | Consolidated knowledge, doctrine, skill patterns, identity |

## The Acronym

| Letter | Principle | One-line |
|--------|-----------|----------|
| **M** | **Memory Is Active, Not Passive** | Memory decays, promotes, and prunes — it is not a static archive. |
| **E** | **Every Tier Has a Purpose** | Working = now. Episodic = what happened. Semantic = what matters. |
| **M** | **Consolidation Is Judgment** | Promoting episodic → semantic is a decision, not an automatic process. |
| **O** | **Decay Is a Feature** | Forgetting is necessary. Without decay, signal drowns in noise. |
| **R** | **Retrieval Is Reconstruction** | Memory is not playback; it is inference from partial traces. |
| **Y** | **Lineage Traces Every Memory** | Every semantic memory has a source chain. (LINEAGE) |

## The Three Tiers — Detailed

### 1. Working Memory (60s)
- **Contents:** Current goal, active tool calls, in-flight swarm state, recent tool results, scratchpad.
- **Access:** Immediate (no retrieval cost).
- **Eviction:** Automatic at 60s or on explicit `scratchpad clear`.
- **Purpose:** Maintain coherence across a single reasoning turn or tool sequence.
- **Rule:** Never rely on working memory for anything that must survive the turn.

### 2. Episodic Memory (1h, decaying)
- **Contents:** Session traces, goal outcomes, swarm results, pheromone signals, user interactions, errors, deltas.
- **Access:** `context_recall` or session-indexed retrieval.
- **Decay:** Exponential. After 1 hour, accessibility drops sharply unless promoted.
- **Purpose:** Capture "what happened in this session" for post-session consolidation.
- **Rule:** Episodic memory is the raw material of learning. If it isn't promoted, it is lost.

### 3. Semantic Memory (Permanent, pruned)
- **Contents:** Consolidated patterns, refined skills, doctrine updates, identity fragments, capability assessments, failure modes discovered.
- **Access:** Direct (loaded into context at session start) + indexed retrieval.
- **Promotion:** Only from episodic memory via consolidation ritual (SESSION_CLOSE or DEEP_WORK).
- **Pruning:** Sunset thresholds apply (EVOLUTION). Unused semantic memories decay to archival.
- **Purpose:** The agent's accumulated, verified, actionable knowledge.
- **Rule:** Semantic memory is the only memory that shapes identity. Everything else is transient.

## Consolidation Rules

0. **Consolidation Is Computable Judgment** *(amended 2026-06-28, grounded in
   Generative Agents 2304.03442 + MemGPT 2310.08560)*
   - Promotion score is no longer a pure vibe. `keep_score = w_r·recency + w_i·importance + w_v·relevance`.
   - Two modes with different weights (learned by testing this engine, not assumed):
     - **promotion** (keep-forever): r=0.05, i=0.65, v=0.30 — recency must NOT decay an
       old lesson; an old failure still teaches (Dalio). importance dominates.
     - **retrieval** (recall-now): r=0.34, i=0.33, v=0.33 — recency leads.
   - **Importance-triggered consolidation:** if Σ importance since last flush > θ (=3.0),
     consolidate immediately, independent of the SESSION_CLOSE clock.
   - Implementation: `skills/memory-tier-engine/scripts/keep_score.py` (tested vs real
     episodic record; failure-lessons correctly promote, partials fall below 0.5).
   - The score *ranks candidates*; the evidence gate (Rule 2) and shadow check still decide.

1. **Session Close Triggers Consolidation**
   - Every SESSION_CLOSE ritual runs a consolidation pass.
   - High-signal episodes (success, failure, user feedback, significant cost) are candidates for promotion.

2. **Promotion Requires Evidence**
   - Episodic trace → semantic memory only if:
     - Outcome was measured (delta exists)
     - Lesson is generalizable (not one-off)
     - Shadow check passes (no reward hacking detected)
   - Promotion without evidence = noise injection.

3. **Three Passes**
   - **Pass 1 (Filter):** Remove low-signal traces (cost < threshold, no outcome, routine).
   - **Pass 2 (Abstract):** Extract pattern from remaining traces. "What rule would have helped?"
   - **Pass 3 (Promote):** Write to semantic memory with LINEAGE (source trace IDs, session, eval).

4. **Consolidation Is Bounded**
   - Meta-work (consolidation) ≤ 20% of session time (RSI_DISCIPLINE, EVOLUTION: F10.3).
   - If consolidation exceeds budget, defer to next roosting period.

## Decay and Pruning

1. **Working Memory:** Automatic eviction at 60s. No persistence.
2. **Episodic Memory:** Exponential decay. After 1 hour, retrieval cost rises; after 24 hours, trace is archival only.
3. **Semantic Memory:** Sunset thresholds (EVOLUTION):
   - Unused for 5 sessions → flag for review
   - Unused for 10 sessions → archive (still retrievable, not in active context)
   - Unused for 15 sessions → delete (git history preserves)

**Rule:** Decay is not loss. It is signal-to-noise management. The system that remembers everything cannot distinguish signal from noise.

## Retrieval Discipline

1. **Retrieval Is Reconstruction**
   - Memory is not a video recording. It is a partial trace + inference.
   - Always label retrieved memory with confidence tier (EPISTEMOLOGY): computed/observed/analyzed/inferred.

2. **Context Recall Before Re-Execution**
   - Before re-running a tool or re-computing a result, check if the answer exists in semantic memory or recent episodic traces.
   - Re-execution without recall check = wasted tokens (GREED violation).

3. **Memory Is Not Truth**
   - Semantic memory can contain outdated or incorrect entries.
   - High-stakes decisions require verification against source, not memory alone (LINEAGE).

## Architecture Mapping

| Layer | Role |
|-------|------|
| **Wings** | Pheromone signals are transient working/episodic memory for swarm coordination. Memory consolidation is a Wings-native operation (stigmergic, not centrally planned). |
| **Talons** | Goal outcomes are written to episodic memory. Strategy success rates are semantic memory (updated from every hunt). Grip strength calibration is working memory (current hunt state). |
| **Immune** | Audit log is durable episodic memory. Governance checkpoint may read semantic memory (policy, invariants) but never trusts it without verification. Kill switch decisions are written to audit (immutable episodic). |

## Cross-Doctrine References

| Doctrine | Relationship |
|----------|-------------|
| EVOLUTION | Consolidation feeds learning. Sunset thresholds are memory pruning. |
| RSI_DISCIPLINE | Consolidation is a self-improvement operation — gated, reversible, shadow-checked. |
| LINEAGE | Every semantic memory has a source chain. Weakest link determines tier. |
| SHADOW | Consolidation can inject self-mythology if reward hacking is not checked. |
| GREED | Re-execution without recall wastes tokens. Memory hygiene is cost discipline. |
| CONTEXT_TOOL_HYGIENE | Memory is context. Overloading semantic memory bloats context. |
| FEEDBACK | Consolidation is feedback aggregation. Patterns emerge from aggregated traces. |

## Anti-Patterns

- ❌ Treating episodic memory as permanent (it decays)
- ❌ Promoting without evidence (noise injection)
- ❌ Skipping consolidation (experience evaporates)
- ❌ Re-executing without recall check (wasted tokens)
- ❌ Overloading semantic memory (context bloat)
- ❌ Confusing memory with truth (reconstruction, not recording)
- ❌ No decay discipline (signal drowned by noise)

## Operating Rules

1. Working memory is for the turn. Episodic is for the session. Semantic is for the life.
2. Consolidation requires evidence, generalization, and shadow check.
3. Decay is a feature. Prune what doesn't earn its keep.
4. Retrieval is reconstruction. Label with evidence tier.
5. Recall before re-execution. Memory hygiene is cost discipline.
6. Semantic memory shapes identity. Only verified, generalizable lessons promote.

---

**Created:** 2026-06-24
**Status:** Active doctrine
**Category:** Infrastructure
**Companion docs:** EVOLUTION.md, RSI_DISCIPLINE.md, LINEAGE.md, SHADOW.md, GREED.md, CONTEXT_TOOL_HYGIENE.md, FEEDBACK.md
**North star:** A system that cannot remember what matters cannot improve. A system that cannot forget what doesn't matter cannot think.