#ifndef DSCO_AI_REPL_H
#define DSCO_AI_REPL_H

/* ai_repl.h — AI-verified distributed replication semantics.
 *
 * The 100K-fleet thesis needs more than "replicate the data": replicas diverge,
 * nodes are Byzantine, and conflicts are often *semantic*, not syntactic. This
 * module fuses classic distributed-systems replication primitives with the Jev
 * calibrated decision layer to make replication intelligent:
 *
 *   DISTRIBUTED SYSTEMS            AI (Jev, calibrated)
 *   ─────────────────            ──────────────────────
 *   vector clocks (causality)  +   semantic conflict resolution (which of two
 *                                    concurrent values is correct *in meaning*?)
 *   CRDT merge (LWW / OR-Set)  +   calibrated merge for non-commutative values
 *   quorum read/write (N,R,W)  +   trust-weighted quorum (weight votes by the
 *                                    node's reputation, not just count)
 *   Byzantine detection        +   Jev verdict: is this node's claim sound?
 *                                    (the jev_gate_verify_claim verifier)
 *
 * Pure data-plane logic (clocks, CRDT, quorum tally) is deterministic and
 * network-free, so it is unit-testable. The AI layer is injected as function
 * pointers so tests stub it; in production it calls jev_gates. Same governance:
 * recommend-only, receipts, fail-open to the deterministic default.
 *
 * This is the replication substrate the fleet memory (indefinite temporal
 * store) and the trust ledger both build on. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AI_REPL_MAX_NODES 64
#define AI_REPL_KEY_MAX 128
#define AI_REPL_VAL_MAX 512

/* ── Vector clocks (causality) ─────────────────────────────────────────── */

typedef struct {
    uint64_t tick[AI_REPL_MAX_NODES];   /* per-node logical counter */
    uint32_t node_index[AI_REPL_MAX_NODES];
    size_t   n;
} ai_vclock_t;

/* Partial order: returns -1 (a<b), +1 (a>b), 2 (concurrent), 0 (equal). */
int ai_vclock_compare(const ai_vclock_t *a, const ai_vclock_t *b);
void ai_vclock_tick(ai_vclock_t *vc, uint32_t node);
void ai_vclock_merge(ai_vclock_t *dst, const ai_vclock_t *src);

/* ── A versioned value (for CRDT / quorum) ─────────────────────────────── */

typedef struct {
    char        key[AI_REPL_KEY_MAX];
    char        value[AI_REPL_VAL_MAX];
    ai_vclock_t clock;
    uint32_t    origin_node;
    uint64_t    wall_ts;      /* last-writer-wins tiebreak (deterministic) */
    bool        tombstone;    /* deleted (OR-Set remove) */
} ai_version_t;

/* CRDT last-writer-wins register merge: deterministic (vector clock, then
 * wall_ts, then origin_node). Returns the winner (a or b). Never loses a
 * causally-later write; concurrent writes resolve deterministically. */
const ai_version_t *ai_repl_lww_merge(const ai_version_t *a, const ai_version_t *b);

/* ── Trust-weighted quorum ─────────────────────────────────────────────── */

typedef struct {
    double reputation;    /* 0..1 from the trust ledger */
    bool   responded;
    double vote;          /* the node's vote weight (reputation if responded) */
} ai_quorum_node_t;

/* Decide whether a quorum is reached, weighting votes by reputation rather
 * than counting nodes. A quorum is reached when the *sum of responding
 * reputation* >= quorum_fraction of the *sum of all reputation*. This lets 2
 * high-trust nodes outvote 5 low-trust ones — correct when nodes are
 * heterogeneous and some are near-Byzantine. Returns true on quorum, and sets
 * *confidence to the fraction of total reputation that responded. */
bool ai_repl_quorum(const ai_quorum_node_t *nodes, size_t n,
                    double quorum_fraction, double *confidence);

/* ── The AI resolver (injected) ────────────────────────────────────────── */

/* Resolve a *semantic* conflict between two concurrent values. Returns the
 * index of the value to keep (0 or 1) and sets *confidence. This is the Jev
 * call: "which of these two concurrent writes is correct in meaning?" The
 * default (NULL) falls back to LWW. */
typedef int (*ai_repl_resolve_fn)(const ai_version_t *a, const ai_version_t *b,
                                  double *confidence, void *ctx);

/* Verify a node's claimed value (Byzantine check). Returns a soundness
 * probability 0..1; below the caller's threshold the claim is distrusted.
 * Default (NULL) trusts all claims at 1.0. */
typedef double (*ai_repl_verify_fn)(const char *key, const ai_version_t *claim,
                                    void *ctx);

/* Resolve two concurrent versions: use the AI resolver when the writes are
 * causally concurrent (the only case where LWW's arbitrary tiebreak can lose
 * semantic meaning); use deterministic LWW when one causally dominates.
 * Returns the winner. This is the fusion point: causality decides *when* to
 * call the AI, the AI decides *what* wins. */
const ai_version_t *ai_repl_resolve(const ai_version_t *a, const ai_version_t *b,
                                    ai_repl_resolve_fn resolve, void *ctx,
                                    double *confidence_out);

/* Read-repair across a set of replica versions for one key: drop claims the
 * verifier distrusts, fold the rest by causal+AI resolution, and return the
 * winning value. Returns true if any trustworthy value exists. */
bool ai_repl_read_repair(const char *key,
                         const ai_version_t *versions, size_t n,
                         const double *reputations,
                         ai_repl_verify_fn verify, ai_repl_resolve_fn resolve,
                         void *ctx, double trust_threshold,
                         ai_version_t *out, double *confidence_out);

#endif /* DSCO_AI_REPL_H */
