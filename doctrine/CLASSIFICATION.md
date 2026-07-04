# CLASSIFICATION

**Status:** Active
**Layer:** Infrastructure & Safety
**Parent doctrines:** SECRECY_HARDENING, SECRECY_AND_TRANSMISSION, GOVERNANCE, BOUNDARIES, SOVEREIGNTY
**Thesis:** We define the classification levels. Sovereignty means the taxonomy is ours —
derived from our own architecture (principal tiers, memory tiers, compartments, glass ceiling),
not inherited from any external ladder. A classification system is a formal language for
membrane permeability: who may see what, in which direction, at what cost, for how long.

---

## 0. Design Principles

1. **Derived, not borrowed.** Levels map to structural facts of this system
   (principal tiers 0-3, three-tier memory, swarm compartments), not to USG analogy.
2. **Classification is expensive by design.** Every level above OPEN carries handling
   cost. Cheap stamps become noise (UMBRA indictment #3). The system must feel
   friction when it classifies.
3. **Two-dimensional access.** Level (vertical) x compartment (horizontal).
   Level clears you for a *ceiling*; compartment read-in grants the *room*.
4. **The glass ceiling is not a level.** Asymmetric transparency to Tier 0/1 is an
   invariant that no classification can override. There is no level, in this system,
   that hides material from the principal hierarchy. Structurally impossible, not
   discouraged (SECRECY_HARDENING S8).
5. **Every label carries a TTL.** Unbounded secrets are entropy debt. Expiry forces
   deliberate re-classification or release (temporal hardening S7).
6. **Fusion is first-class.** Every compartment plan must name its fusion point —
   where the joined picture may legally exist — or justify why none exists
   (stovepipe collapse counter, Annex A).

## 1. The Levels

Four levels. Fewer than the USG ladder on purpose: each level must be
*operationally distinct* — different handling, different memory rules, different
egress. A level that handles identically to its neighbor is noise.

### L0 — OPEN
- **Definition:** No confidentiality interest. Publishable.
- **Examples:** Public docs, published code, marketing, this doctrine's existence.
- **Memory:** Any tier, no restrictions. Consolidates freely.
- **Egress:** Unrestricted.
- **TTL:** None.

### L1 — HELD
- **Definition:** Not secret, but not volunteered. Disclosure requires a reason,
  not a clearance. The default level for operational working material.
- **Examples:** Internal roadmaps, session logs, cost data, unreleased features,
  ordinary business context.
- **Memory:** Any tier. Consolidates freely, tagged.
- **Egress:** Disclosed on legitimate request; not pushed to untrusted channels.
  No Glomar needed — existence is acknowledgeable.
- **Principal floor:** Tier 3 (user) and above.
- **TTL:** Default 1 year, auto-review to L0.

### L2 — SEALED
- **Definition:** Active confidentiality interest. Disclosure causes real harm:
  competitive, financial, personal, or strategic. Requires level + compartment
  read-in. This is where compartments begin.
- **Examples:** Trading strategies and positions, negotiation posture, personal
  financial details, unpatched vulnerability knowledge, counterparty intelligence.
- **Memory:** Working/episodic freely within compartment. **Semantic consolidation
  requires classification review** — a SEALED fact does not enter permanent memory
  by default (consolidation leak counter).
- **Egress:** Compartment members only. Existence acknowledgeable, contents not.
  Refusals uniform within compartment topic ("that's compartmented") — no
  differential refusal that maps contents.
- **Principal floor:** Tier 2 (agent) with read-in; Tier 3 only if compartment
  owner grants.
- **TTL:** Default 90 days, owner review. Max single extension 90 days without
  Tier 0/1 sign-off.

### L3 — UMBRAL
- **Definition:** Existence-classified. The fact that a compartment exists on this
  topic is itself SEALED. Full Glomar at the boundary: uniform neither-confirm-
  nor-deny across the entire topic class, including for queries where nothing
  exists (refusal cartography counter).
- **Examples:** Material whose existence-confirmation is the leak: an acquisition
  interest, a legal matter, a security incident under investigation, a key
  relationship not yet public.
- **Memory:** Working memory only, within compartment. **Never consolidates to
  episodic or semantic without explicit Tier 0 promotion.** Purged at session
  close unless promoted.
- **Egress:** Compartment members only, two-person rule on any disclosure
  (independent monitor sign-off, SECRECY_HARDENING S5). Canary-seeded.
- **Principal floor:** Named principals only — no tier grants automatic access.
- **TTL:** Default 30 days, hard review. Cannot auto-renew.
- **Glass ceiling:** Tier 0 sees the compartment registry entry, always. UMBRAL
  hides existence from *lateral and lower* observers, never upward. An UMBRAL
  compartment invisible to Tier 0 is a sovereign-shadow violation and trips
  killswitch review.

**There is no L4.** A level above UMBRAL would be a compartment hidden from the
principal hierarchy — definitionally forbidden. The ladder terminates at the
glass ceiling by construction. (Apophasis: the system is partly defined by the
level it refuses to create.)

## 2. Compartments (horizontal axis)

- **Naming:** Internal codewords, generated (two-word, non-semantic — the name must
  not leak the topic). The codeword of an L2 compartment is L1. The codeword of an
  L3 compartment is L2.
- **Registry:** Every compartment has a registry entry: codeword, level, owner,
  access list, fusion point, TTL, canary status. Registry itself is L2,
  compartment "LEDGER". Tier 0 reads the full registry unconditionally.
- **Read-in:** Explicit, logged, per-principal. Read-in confers access to the room,
  not the floor above it.
- **Read-out:** Departure is logged; knowledge doesn't un-know, so read-out ends
  *new* access and triggers egress-audit attention on the departed principal's
  channel for the compartment's remaining TTL.
- **Fusion points:** Named at creation. The fusion point is a compartment whose
  access list is the union's minimum viable set — treated as one level hotter
  than its inputs for handling purposes.

## 3. Markings

Format: `[LEVEL//CODEWORD//caveat]`

- `[L1]` — held
- `[L2//GRANITE-WAKE]` — sealed, compartment granite-wake
- `[L3//<codeword itself L2>]` — umbral
- Caveats: `ORCON` (originator controls re-share), `NOFORK` (may not enter
  sub-agent context), `TTL:YYYY-MM-DD` (explicit expiry), `FUSION:<codeword>`
  (designated fusion point).

Markings are **metadata, not tokens** — they ride the provenance layer
(SECRECY_HARDENING S2), outside any attacker's token stream.

## 4. Authorities

| Action | Authority |
|---|---|
| Classify at L1 | Any agent (Tier 2+) |
| Classify at L2, create compartment | Tier 1+, or Tier 2 with Tier 1 ratification within 24h |
| Classify at L3 | Tier 0/1 only |
| Declassify / downgrade | One tier above the classifier, out-of-band only |
| Extend TTL | Compartment owner once; Tier 0/1 thereafter |
| Read the full registry | Tier 0, unconditionally |
| Override any of the above | Nobody. Hardcoded. |

**No in-band declassification** (S5): nothing said inside a session lowers a label.

## 5. Memory Integration (three-tier mapping)

| | Working (60s) | Episodic (1h) | Semantic (permanent) |
|---|---|---|---|
| L0 | free | free | free |
| L1 | free | free | free, tagged |
| L2 | free in compartment | free in compartment | **review gate** |
| L3 | compartment only | **Tier 0 promotion only** | **Tier 0 promotion only** |

The consolidation daemon checks labels at every promotion boundary. Unlabeled
material touching an active compartment's topic is quarantined pending review —
classification at write time, not read time (S3).

## 6. Sub-agent / Swarm Integration

- Context slices to sub-agents are filtered by the sub-agent's effective read-in:
  intersection of (spawning agent's compartments) x (task need-to-know).
- `NOFORK` material never enters a sub-agent context, period.
- Swarm depth is defense: hotter material lives higher in the hierarchy.
  Default: L3 material does not descend below depth 1; L2 not below depth 3.
- Tool outputs returning from sub-agents inherit the taint of what they touched.

## 7. Failure Modes (inherited + native)

- **Inflation:** L2 becomes the default stamp -> handling discipline decays.
  Counter: classification is costed; quarterly inflation audit (ratio of L2+ to
  total should trend *down* in steady state).
- **Orphan compartments:** owner departs / expires, compartment persists unowned.
  Counter: ownership is a liveness property; orphans escalate to Tier 1 in 7 days.
- **TTL amnesia:** expiry arrives, nobody reviews, secret silently persists.
  Counter: expiry defaults to *lockdown*, not silent retention — an unreviewed
  expired compartment freezes until reviewed.
- **Codeword semantics leak:** compartment named after its topic. Counter:
  non-semantic generated names, enforced at creation.
- **Fusion-point sprawl:** fusion compartments accumulating supersets of everything.
  Counter: fusion points carry the hottest handling and their own TTLs.

## Audit
- 2026-07-04: Doctrine created. Four-level sovereign taxonomy (OPEN/HELD/SEALED/UMBRAL),
  terminating at the glass ceiling by construction. Derived from SECRECY_HARDENING +
  Annex A (UMBRA case study). "We define the classification levels." — A. Colle.

## Enforcement Status
- 2026-07-04: §5 memory gates ENFORCED IN CODE, not just doctrine:
  - `include/memory_tier.h` — `memory_class_t` (L0-L3), `classification` +
    `class_reviewed` fields on `memory_entry_t`, four new functions.
  - `src/memory_tier.c` — gates wired into both promotion paths
    (`memory_promote` + `memory_consolidate` sweep); `memory_classify`
    (labels only rise in-band; late-L3 demotes leaked material to working);
    `memory_classify_review` (one grant = one promotion);
    `memory_purge_umbral` (verified deletion — value bytes zeroed).
  - `tests/test_memory_classification.c` — 25 checks, all passing, wired
    into `make test_priorities`.
  Doctrine without enforcement is decoration. This one compiles.
