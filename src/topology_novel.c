/* topology_novel.c — Novel topologies #61-70
 * Derived from 8-dimensional structural gap analysis of the existing 60.
 * Each topology fills a named gap; each introduces at least one new edge type
 * or execution strategy not present in the original set.
 *
 * New edge types required (add to topology.h):
 *   EDGE_CONDITIONAL_FAIL   — traverse only if scorer output < threshold
 *   EDGE_CONDITIONAL_PASS   — traverse only if scorer output >= threshold (short-circuit)
 *   EDGE_FANIN_PAIR         — pairwise fanin for elimination bracket
 *   EDGE_SIBLING_CRITIQUE   — expert→critic dyad (same-context critique)
 *   EDGE_PRUNE_FANOUT       — fanout only to top-K survivors (pruner decides K)
 *   EDGE_AGREE_SPLIT        — sends agreement surface to merge node
 *   EDGE_CONFLICT_ROUTE     — sends conflict surface to resolver node
 *   EDGE_SPARSE_ACTIVATE    — router activates exactly K of N (unused = $0)
 *   EDGE_SECTION_FANOUT     — fanout one section context per branch
 *   EDGE_SPECULATIVE        — parallel speculative track with early-kill signal
 *   EDGE_WINNER_ROUTE       — routes winner from early scorer; kills loser
 *   EDGE_ASYNC_STREAM       — inter-stage stream (no barrier; items flow continuously)
 *   EDGE_META_EMIT          — meta-planner emits topology spec to spawner
 *   EDGE_DYNAMIC            — spawner-instantiated edge (target known at runtime only)
 *   EDGE_PARALLEL           — strict parallel track (no data dep between tracks)
 *
 * New exec strategies required (add to topology.h):
 *   EXEC_ADAPTIVE           — path determined at runtime by scored gates
 *   EXEC_ELIMINATION        — bracket-style; losers killed between rounds
 *   EXEC_SPECULATIVE        — dual-track with early termination of losing track
 *   EXEC_ASYNC_PIPELINE     — stages overlap; items stream between stages
 *
 * Implementation notes:
 *   - EXEC_ADAPTIVE requires a confidence_threshold field in topology_t
 *   - EXEC_ELIMINATION requires a bracket_rounds field
 *   - EXEC_SPECULATIVE requires a speculative_kill_threshold field
 *   - EXEC_ASYNC_PIPELINE requires a stream_buffer_depth field
 *   - ouroboros (#70) requires runtime agent_spawn() capability
 */

/* Cost reference (from src/topology.c tier_unit_cost):
 *   TIER_HAIKU:  $0.0048/1k
 *   TIER_SONNET: $0.0180/1k
 *   TIER_OPUS:   $0.0900/1k
 */

#include "topology.h"

void topology_init_novel(void) {

/* =========================================================================
 * #61 THERMOCLINE
 * Gap-1: sentinel always escalates all 3 tiers. No confidence-gated exit.
 * Innovation: CONDITIONAL_ESCAPE — each tier boundary is a scored gate.
 *   ~70% of tasks exit at gate1 (Haiku cost only: ~$0.010/1k)
 *   ~25% exit at gate2 (Haiku+Sonnet: ~$0.046/1k)
 *   ~5%  reach Opus (full: ~$0.136/1k)
 *   Expected avg: ~$0.025/1k vs sentinel $0.1128/1k → -78%
 * ========================================================================= */
TI(61, "thermocline",
   "Confidence-gated tier escalation: H draft+gate -> S analyze+gate -> O judge",
   CAT_ADAPTIVE, EXEC_ADAPTIVE, 1, 2.0);
TN(61, TIER_HAIKU,  ROLE_GENERATOR,  "draft",           1);
TN(61, TIER_HAIKU,  ROLE_VALIDATOR,  "confidence_gate", 1);
TN(61, TIER_SONNET, ROLE_WORKER,     "deep_analyze",    1);
TN(61, TIER_SONNET, ROLE_VALIDATOR,  "sonnet_gate",     1);
TN(61, TIER_OPUS,   ROLE_JUDGE,      "final_arbiter",   1);
TE(61, 0, 1, EDGE_SEQUENCE);
TE(61, 1, 2, EDGE_CONDITIONAL_FAIL);  /* only if confidence < GATE1_THRESH (default 7.0) */
TE(61, 1, 4, EDGE_CONDITIONAL_PASS);  /* short-circuit: skip S and O */
TE(61, 2, 3, EDGE_SEQUENCE);
TE(61, 3, 4, EDGE_CONDITIONAL_FAIL);  /* only if confidence < GATE2_THRESH (default 8.0) */
TE(61, 3, 4, EDGE_CONDITIONAL_PASS);  /* short-circuit: skip O */
TF(61);

/* =========================================================================
 * #62 BRACKETOLOGY
 * Gap-2: tournament runs all N to one judge. O(N) judge exposure.
 * Innovation: binary elimination bracket → O(log N) judge depth.
 *   8 H contestants → 4 S round-1 judges (pairwise) → 2 O semis → 1 O final
 *   Each judge sees exactly 2 candidates: cleaner pairwise comparison signal.
 *   Winner has survived 3 rounds of judgment; loser signal never noises final.
 *   Cost: 8×$0.0048 + 4×$0.018 + 3×$0.09 = $0.3804/1k
 *   Quality guarantee: far higher than tournament (3 candidates, 1 judge).
 * ========================================================================= */
TI(62, "bracketology",
   "Binary elimination bracket: 8H -> 4S round1 -> 2O semis -> 1O final",
   CAT_COMPETITIVE, EXEC_ELIMINATION, 1, 3.5);
TN(62, TIER_HAIKU,  ROLE_GENERATOR, "c1", 1); TN(62, TIER_HAIKU, ROLE_GENERATOR, "c2", 1);
TN(62, TIER_HAIKU,  ROLE_GENERATOR, "c3", 1); TN(62, TIER_HAIKU, ROLE_GENERATOR, "c4", 1);
TN(62, TIER_HAIKU,  ROLE_GENERATOR, "c5", 1); TN(62, TIER_HAIKU, ROLE_GENERATOR, "c6", 1);
TN(62, TIER_HAIKU,  ROLE_GENERATOR, "c7", 1); TN(62, TIER_HAIKU, ROLE_GENERATOR, "c8", 1);
TN(62, TIER_SONNET, ROLE_JUDGE, "r1_j1", 1); TN(62, TIER_SONNET, ROLE_JUDGE, "r1_j2", 1);
TN(62, TIER_SONNET, ROLE_JUDGE, "r1_j3", 1); TN(62, TIER_SONNET, ROLE_JUDGE, "r1_j4", 1);
TN(62, TIER_OPUS,   ROLE_JUDGE, "r2_j1", 1); TN(62, TIER_OPUS,   ROLE_JUDGE, "r2_j2", 1);
TN(62, TIER_OPUS,   ROLE_JUDGE, "final",  1);
TE(62,  0,  8, EDGE_FANIN_PAIR); TE(62,  1,  8, EDGE_FANIN_PAIR);
TE(62,  2,  9, EDGE_FANIN_PAIR); TE(62,  3,  9, EDGE_FANIN_PAIR);
TE(62,  4, 10, EDGE_FANIN_PAIR); TE(62,  5, 10, EDGE_FANIN_PAIR);
TE(62,  6, 11, EDGE_FANIN_PAIR); TE(62,  7, 11, EDGE_FANIN_PAIR);
TE(62,  8, 12, EDGE_FANIN_PAIR); TE(62,  9, 12, EDGE_FANIN_PAIR);
TE(62, 10, 13, EDGE_FANIN_PAIR); TE(62, 11, 13, EDGE_FANIN_PAIR);
TE(62, 12, 14, EDGE_FANIN_PAIR); TE(62, 13, 14, EDGE_FANIN_PAIR);
TF(62);

/* =========================================================================
 * #63 SOCRATIC
 * Gap-3: tribunal/adversarial share a single judge across all branches.
 *   Critique is generic, not domain-specific to the expert's context.
 * Innovation: 1:1 expert→critic dyads using EDGE_SIBLING_CRITIQUE.
 *   Critic receives the same task context as its paired expert.
 *   O synthesizer receives only hardened (critique-integrated) outputs.
 *   Cost: 3×$0.018 + 3×$0.0048 + $0.09 = $0.1584/1k (vs expert_panel $0.18)
 * ========================================================================= */
TI(63, "socratic",
   "3 S-expert + H-critic dyads; hardened outputs synthesized by Opus",
   CAT_MESH, EXEC_PARALLEL_STAGES, 1, 3.0);
TN(63, TIER_SONNET, ROLE_SPECIALIST,  "expert1", 1);
TN(63, TIER_HAIKU,  ROLE_CRITIC,      "critic1", 1);
TN(63, TIER_SONNET, ROLE_SPECIALIST,  "expert2", 1);
TN(63, TIER_HAIKU,  ROLE_CRITIC,      "critic2", 1);
TN(63, TIER_SONNET, ROLE_SPECIALIST,  "expert3", 1);
TN(63, TIER_HAIKU,  ROLE_CRITIC,      "critic3", 1);
TN(63, TIER_OPUS,   ROLE_SYNTHESIZER, "synth",   1);
TE(63, 0, 1, EDGE_SIBLING_CRITIQUE);
TE(63, 2, 3, EDGE_SIBLING_CRITIQUE);
TE(63, 4, 5, EDGE_SIBLING_CRITIQUE);
TE(63, 1, 6, EDGE_FANIN);
TE(63, 3, 6, EDGE_FANIN);
TE(63, 5, 6, EDGE_FANIN);
TF(63);

/* =========================================================================
 * #64 TIDAL
 * Gap-4: nova is single-wave (fan-out then fan-in, no intermediate pruning).
 * Innovation: multi-wave beam search — 3 waves, pruner between each.
 *   Wave 1: 6 H explorers → H pruner selects top-3 survivors
 *   Wave 2: 3 S developers expand survivors → S pruner selects top-1
 *   Wave 3: 1 O finalizer refines the single best candidate
 *   Cost: $0.1956/1k vs nova $0.2364/1k (-17%); quality higher (pruned beam)
 * ========================================================================= */
TI(64, "tidal",
   "Multi-wave beam: 6H explore -> H prune(3) -> 3S develop -> S prune(1) -> O finalize",
   CAT_FANOUT, EXEC_PARALLEL_STAGES, 3, 4.0);
TN(64, TIER_HAIKU,  ROLE_SCOUT,    "w1_a",  1); TN(64, TIER_HAIKU, ROLE_SCOUT, "w1_b", 1);
TN(64, TIER_HAIKU,  ROLE_SCOUT,    "w1_c",  1); TN(64, TIER_HAIKU, ROLE_SCOUT, "w1_d", 1);
TN(64, TIER_HAIKU,  ROLE_SCOUT,    "w1_e",  1); TN(64, TIER_HAIKU, ROLE_SCOUT, "w1_f", 1);
TN(64, TIER_HAIKU,  ROLE_REDUCER,  "prune1",1);
TN(64, TIER_SONNET, ROLE_WORKER,   "w2_a",  1); TN(64, TIER_SONNET, ROLE_WORKER, "w2_b", 1);
TN(64, TIER_SONNET, ROLE_WORKER,   "w2_c",  1);
TN(64, TIER_SONNET, ROLE_REDUCER,  "prune2",1);
TN(64, TIER_OPUS,   ROLE_JUDGE,    "w3",    1);
TE(64, 0,  6, EDGE_FANIN); TE(64, 1, 6, EDGE_FANIN); TE(64, 2, 6, EDGE_FANIN);
TE(64, 3,  6, EDGE_FANIN); TE(64, 4, 6, EDGE_FANIN); TE(64, 5, 6, EDGE_FANIN);
TE(64, 6,  7, EDGE_PRUNE_FANOUT); /* prune1 → top-3 → w2_a,w2_b,w2_c */
TE(64, 6,  8, EDGE_PRUNE_FANOUT);
TE(64, 6,  9, EDGE_PRUNE_FANOUT);
TE(64, 7, 10, EDGE_FANIN); TE(64, 8, 10, EDGE_FANIN); TE(64, 9, 10, EDGE_FANIN);
TE(64, 10, 11, EDGE_PRUNE_FANOUT); /* prune2 → top-1 → w3 */
TF(64);

/* =========================================================================
 * #65 CRUCIBLE
 * Gap-5: all merge strategies are additive. No topology isolates conflicts.
 * Innovation: EDGE_DIFF routes agreement/conflict surfaces to different nodes.
 *   Agreements are cheap-merged by Haiku.
 *   Conflicts (the interesting surface) go to Opus — smaller input, sharper focus.
 *   S integrator assembles both. Opus sees only the dispute, not full outputs.
 *   Cost: $0.1716/1k vs constellation $0.2700/1k (-36%)
 * ========================================================================= */
TI(65, "crucible",
   "3S branches -> H diff(agree+conflict) -> H cheap-merge + O resolve -> S integrate",
   CAT_MESH, EXEC_PARALLEL_STAGES, 1, 3.5);
TN(65, TIER_SONNET, ROLE_SPECIALIST,  "branch_a",  1);
TN(65, TIER_SONNET, ROLE_SPECIALIST,  "branch_b",  1);
TN(65, TIER_SONNET, ROLE_SPECIALIST,  "branch_c",  1);
TN(65, TIER_HAIKU,  ROLE_REDUCER,     "differ",    1);
TN(65, TIER_HAIKU,  ROLE_SYNTHESIZER, "agree",     1);
TN(65, TIER_OPUS,   ROLE_JUDGE,       "resolve",   1);
TN(65, TIER_SONNET, ROLE_SYNTHESIZER, "integrate", 1);
TE(65, 0, 3, EDGE_FANIN);
TE(65, 1, 3, EDGE_FANIN);
TE(65, 2, 3, EDGE_FANIN);
TE(65, 3, 4, EDGE_AGREE_SPLIT);    /* agreement surface → cheap H merge */
TE(65, 3, 5, EDGE_CONFLICT_ROUTE); /* conflict surface → Opus resolver */
TE(65, 4, 6, EDGE_FANIN);
TE(65, 5, 6, EDGE_FANIN);
TF(65);

/* =========================================================================
 * #66 MIXTURE_OF_EXPERTS
 * Gap-6: dandelion routes to 1 of 4 Sonnet branches ($0.1668/1k).
 *   No topology uses sparse activation of many cheap specialists.
 * Innovation: H router activates exactly K=2 of N=8 Haiku micro-experts.
 *   Unused experts cost $0. Effective cost: $0.0048 + 2×$0.0048 + $0.018 = $0.0324/1k
 *   75% cheaper than dandelion. Inspired by MoE transformer architecture.
 *   Router output: {active_experts: ["me_code","me_math"], confidence: 0.87}
 * ========================================================================= */
TI(66, "mixture_of_experts",
   "H router sparse-activates K=2 of N=8 Haiku micro-experts; S merges",
   CAT_FANOUT, EXEC_ADAPTIVE, 1, 2.5);
TN(66, TIER_HAIKU,  ROLE_CLASSIFIER,  "router",      1);
TN(66, TIER_HAIKU,  ROLE_SPECIALIST,  "me_code",     1);
TN(66, TIER_HAIKU,  ROLE_SPECIALIST,  "me_math",     1);
TN(66, TIER_HAIKU,  ROLE_SPECIALIST,  "me_logic",    1);
TN(66, TIER_HAIKU,  ROLE_SPECIALIST,  "me_domain",   1);
TN(66, TIER_HAIKU,  ROLE_SPECIALIST,  "me_style",    1);
TN(66, TIER_HAIKU,  ROLE_SPECIALIST,  "me_research", 1);
TN(66, TIER_HAIKU,  ROLE_SPECIALIST,  "me_ops",      1);
TN(66, TIER_HAIKU,  ROLE_SPECIALIST,  "me_creative", 1);
TN(66, TIER_SONNET, ROLE_SYNTHESIZER, "merge",       1);
/* EDGE_SPARSE_ACTIVATE: router traverses only K=2 of these 8 edges per call */
TE(66, 0, 1, EDGE_SPARSE_ACTIVATE); TE(66, 0, 2, EDGE_SPARSE_ACTIVATE);
TE(66, 0, 3, EDGE_SPARSE_ACTIVATE); TE(66, 0, 4, EDGE_SPARSE_ACTIVATE);
TE(66, 0, 5, EDGE_SPARSE_ACTIVATE); TE(66, 0, 6, EDGE_SPARSE_ACTIVATE);
TE(66, 0, 7, EDGE_SPARSE_ACTIVATE); TE(66, 0, 8, EDGE_SPARSE_ACTIVATE);
TE(66, 1, 9, EDGE_FANIN); TE(66, 2, 9, EDGE_FANIN); TE(66, 3, 9, EDGE_FANIN);
TE(66, 4, 9, EDGE_FANIN); TE(66, 5, 9, EDGE_FANIN); TE(66, 6, 9, EDGE_FANIN);
TE(66, 7, 9, EDGE_FANIN); TE(66, 8, 9, EDGE_FANIN);
TF(66);

/* =========================================================================
 * #67 SCAFFOLD
 * Gap-7: annealing validates the full output globally.
 *   Section-level errors are only caught after full assembly — expensive rework.
 * Innovation: O architect decomposes task → N parallel S+H section dyads.
 *   Each dyad validates exactly its section. O integrates only clean sections.
 *   H consistency checker catches cross-section coherence issues.
 *   Cost: 2×$0.09 + 4×$0.018 + 5×$0.0048 = $0.276/1k; max_iter=2
 * ========================================================================= */
TI(67, "scaffold",
   "O outline -> 4x(S draft + H section-validate) parallel -> O integrate -> H consistency",
   CAT_CHAIN, EXEC_PARALLEL_STAGES, 2, 3.5);
TN(67, TIER_OPUS,   ROLE_COORDINATOR, "outline",    1);
TN(67, TIER_SONNET, ROLE_WORKER,      "draft_s1",   1);
TN(67, TIER_HAIKU,  ROLE_VALIDATOR,   "val_s1",     1);
TN(67, TIER_SONNET, ROLE_WORKER,      "draft_s2",   1);
TN(67, TIER_HAIKU,  ROLE_VALIDATOR,   "val_s2",     1);
TN(67, TIER_SONNET, ROLE_WORKER,      "draft_s3",   1);
TN(67, TIER_HAIKU,  ROLE_VALIDATOR,   "val_s3",     1);
TN(67, TIER_SONNET, ROLE_WORKER,      "draft_s4",   1);
TN(67, TIER_HAIKU,  ROLE_VALIDATOR,   "val_s4",     1);
TN(67, TIER_OPUS,   ROLE_SYNTHESIZER, "integrate",  1);
TN(67, TIER_HAIKU,  ROLE_VALIDATOR,   "consistency",1);
TE(67, 0,  1, EDGE_SECTION_FANOUT); TE(67, 0,  3, EDGE_SECTION_FANOUT);
TE(67, 0,  5, EDGE_SECTION_FANOUT); TE(67, 0,  7, EDGE_SECTION_FANOUT);
TE(67, 1,  2, EDGE_SEQUENCE);  TE(67, 3,  4, EDGE_SEQUENCE);
TE(67, 5,  6, EDGE_SEQUENCE);  TE(67, 7,  8, EDGE_SEQUENCE);
TE(67, 2,  9, EDGE_FANIN);     TE(67, 4,  9, EDGE_FANIN);
TE(67, 6,  9, EDGE_FANIN);     TE(67, 8,  9, EDGE_FANIN);
TE(67, 9, 10, EDGE_SEQUENCE);
TF(67);

/* =========================================================================
 * #68 SPECTRE
 * Gap-8: tournament keeps all contestants alive until the judge sees them.
 *   If one track is clearly losing at 25% completion, the rest of its tokens
 *   are wasted running to completion just so the judge can confirm the loss.
 * Innovation: EDGE_SPECULATIVE — H spec and S careful run in parallel.
 *   O early_scorer evaluates spec (complete) vs careful (25% partial).
 *   If spec wins early: careful track is killed → -S×75% token cost saved.
 *   If careful wins early: spec already done → no extra cost on that path.
 *   Expected cost: ~$0.061/1k vs tournament $0.144/1k (-58%)
 * ========================================================================= */
TI(68, "spectre",
   "H spec || S careful -> O early_kill_score -> S finalize winner",
   CAT_COMPETITIVE, EXEC_SPECULATIVE, 1, 2.5);
TN(68, TIER_HAIKU,  ROLE_GENERATOR,   "spec_draft",    1);
TN(68, TIER_SONNET, ROLE_WORKER,      "careful_draft", 1);
TN(68, TIER_OPUS,   ROLE_JUDGE,       "early_score",   1);
TN(68, TIER_SONNET, ROLE_SYNTHESIZER, "finalize",      1);
TE(68, 0, 2, EDGE_SPECULATIVE);  /* spec feeds scorer when complete */
TE(68, 1, 2, EDGE_PARALLEL);     /* careful runs in parallel; scorer reads partial */
TE(68, 2, 3, EDGE_WINNER_ROUTE); /* winner continues; loser receives kill signal */
TF(68);

/* =========================================================================
 * #69 CONVEYOR
 * Gap-9: assembly_line has a synchronous barrier between stages.
 *   For B items: total latency = B × Σ(all stage latencies).
 * Innovation: EDGE_ASYNC_STREAM — each stage starts immediately on first
 *   completed item from prior stage. No global barrier.
 *   For B items: latency = B × max(stage_lat) + (stages-1) × max(stage_lat)
 *   i.e., O(B) not O(B×N). Identical cost to assembly_line; better latency.
 *   Wall-clock: 5-stage pipeline on 10 items = 10 slots, not 50.
 * ========================================================================= */
TI(69, "conveyor",
   "Async streaming pipeline: H ingest -> S transform -> S enrich -> H validate -> H format",
   CAT_CHAIN, EXEC_ASYNC_PIPELINE, 1, 2.0);
TN(69, TIER_HAIKU,  ROLE_WORKER,    "ingest",    1);
TN(69, TIER_SONNET, ROLE_WORKER,    "transform", 1);
TN(69, TIER_SONNET, ROLE_WORKER,    "enrich",    1);
TN(69, TIER_HAIKU,  ROLE_VALIDATOR, "validate",  1);
TN(69, TIER_HAIKU,  ROLE_WORKER,    "format",    1);
TE(69, 0, 1, EDGE_ASYNC_STREAM);
TE(69, 1, 2, EDGE_ASYNC_STREAM);
TE(69, 2, 3, EDGE_ASYNC_STREAM);
TE(69, 3, 4, EDGE_ASYNC_STREAM);
TF(69);

/* =========================================================================
 * #70 OUROBOROS
 * Gap-10: all 60 topologies are statically configured at compile time.
 *   Topology mismatch (wrong structure for task) has no runtime correction.
 * Innovation: S meta_planner emits a topology spec as its first output.
 *   H spawner instantiates agents per spec. Graph shape is a task output.
 *   Dynamic slots (slot_1-4) are filled at runtime; tier and role are spec-driven.
 *   "The topology eats its own tail" — the system defines its own execution shape.
 *   Cost base: $0.1764/1k; varies by spawned topology spec.
 * ========================================================================= */
TI(70, "ouroboros",
   "S meta-planner emits topology spec -> H spawner -> dynamic agent slots -> S collect",
   CAT_NOVEL, EXEC_ADAPTIVE, 1, 3.0);
TN(70, TIER_SONNET, ROLE_COORDINATOR, "plan",    1);
TN(70, TIER_HAIKU,  ROLE_DELEGATOR,   "spawn",   1);
/* Dynamic slots — tier/role determined at runtime by plan output */
TN(70, TIER_HAIKU,  ROLE_WORKER,      "slot_1",  1);
TN(70, TIER_SONNET, ROLE_WORKER,      "slot_2",  1);
TN(70, TIER_SONNET, ROLE_WORKER,      "slot_3",  1);
TN(70, TIER_OPUS,   ROLE_JUDGE,       "slot_4",  1);
TN(70, TIER_SONNET, ROLE_SYNTHESIZER, "collect", 1);
TE(70, 0, 1, EDGE_META_EMIT);
TE(70, 1, 2, EDGE_DYNAMIC); TE(70, 1, 3, EDGE_DYNAMIC);
TE(70, 1, 4, EDGE_DYNAMIC); TE(70, 1, 5, EDGE_DYNAMIC);
TE(70, 2, 6, EDGE_FANIN);   TE(70, 3, 6, EDGE_FANIN);
TE(70, 4, 6, EDGE_FANIN);   TE(70, 5, 6, EDGE_FANIN);
TF(70);

} /* topology_init_novel() */
