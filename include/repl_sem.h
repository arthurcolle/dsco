#ifndef DSCO_REPL_SEM_H
#define DSCO_REPL_SEM_H

/* repl_sem.h — unified replication memory semantics for dsco + graphsub.
 *
 * The two systems have different native memory semantics, and a unified fleet
 * must not silently conflate them:
 *
 *   graphsub:  STRONG — single-register linearizability, atomic CAS, serialized
 *              writes (Jepsen-validated). Per-shard. Correct but not available
 *              under partition (CP).
 *   dsco fleet: WEAK  — eventual consistency, concurrent writes, causal order
 *              via vector clocks, convergent via CRDT (AP).
 *
 * Naively treating one as the other loses data: a linearizable register read
 * assumes the latest write, which eventual consistency cannot promise; a CRDT
 * merge assumes concurrency is normal, which a serialized register hides.
 *
 * This module makes the consistency *level* an explicit, first-class part of
 * every value, so a unified store can:
 *   - carry a value's native strength (LINEARIZABLE | CAUSAL | EVENTUAL);
 *   - attach the right clock for that level (HLB for strong ordering, vector
 *     clock for causal, wall_ts for LWW);
 *   - provide a *verifiable downgrade*: reading a LINEARIZABLE value from an
 *     eventually-consistent replica reports the reduced guarantee honestly,
 *     with session guarantees (read-your-writes, monotonic reads) tracked by
 *     the client's clock, instead of pretending linearizability;
 *   - resolve conflicts by level: strong writes serialize (CAS), causal writes
 *     merge by vector clock, and genuinely-concurrent semantic conflicts defer
 *     to the calibrated Jev resolver (ai_repl).
 *
 * Deterministic and network-free (like ai_repl); the AI resolver is injected.
 * This is the memory-semantics contract the 100K-fleet store and graphsub's
 * shard persistence both honor. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ai_repl.h"   /* ai_vclock_t, ai_version_t, resolvers */

/* ── Hybrid Logical Clock (physical time + logical counter) ──────────────
 * GraphSub's Jepsen report names the need: "clock skew makes linearizability
 * impossible; need HLC or vector clocks." HLC gives causal, skew-tolerant
 * timestamps that are close to wall time (so they double as LWW tiebreaks)
 * but never go backward under clock skew. */
typedef struct {
    uint64_t physical;    /* max(wall time, last-seen physical)   */
    uint32_t logical;     /* counter for same-physical-tick order */
    uint32_t node;        /* node id (final tiebreak)             */
} ai_hlc_t;

/* Advance the local HLC for a local event. */
void ai_hlc_now(ai_hlc_t *hlc, uint64_t wall_ms, uint32_t node);
/* Advance the local HLC on observing a remote timestamp (recv). */
void ai_hlc_recv(ai_hlc_t *hlc, uint64_t wall_ms, uint32_t node, const ai_hlc_t *remote);
/* Total order: returns <0, 0, >0. */
int ai_hlc_compare(const ai_hlc_t *a, const ai_hlc_t *b);

/* ── Consistency strength ──────────────────────────────────────────────── */

typedef enum {
    CONSISTENCY_LINEARIZABLE = 0,  /* graphsub native: serialized, CAS        */
    CONSISTENCY_CAUSAL,            /* vector-clock causal order               */
    CONSISTENCY_EVENTUAL,          /* plain LWW / last-writer-wins            */
} consistency_level_t;

/* A value stamped with its consistency level + the clock for that level. */
typedef struct {
    char               key[AI_REPL_KEY_MAX];
    char               value[AI_REPL_VAL_MAX];
    consistency_level_t level;
    ai_hlc_t           hlc;        /* ordering for LINEARIZABLE / CAUSAL     */
    ai_vclock_t        vclock;     /* causality for CAUSAL / EVENTUAL        */
    uint32_t           origin_node;
    bool               tombstone;
} rs_version_t;

/* ── Verifiable read: report the *actual* guarantee, not the hoped one ─── */

/* The guarantee a read can honestly offer, given the value's native level and
 * the consistency of the path it was read over. */
typedef enum {
    GUARANTEE_LINEARIZABLE = 0, /* latest write, serialized              */
    GUARANTEE_MONOTONIC,        /* no older-than-already-seen (session)  */
    GUARANTEE_READ_YOUR_WRITES, /* sees own prior writes (session)       */
    GUARANTEE_EVENTUAL,         /* will converge; may be stale           */
} read_guarantee_t;

/* Compute the honest read guarantee: the *weaker* of the value's native level
 * and the path's consistency. Reading a LINEARIZABLE value from an
 * EVENTUAL path yields (at best) a session guarantee, never linearizable —
 * this is the verifiable downgrade. */
read_guarantee_t repl_sem_read_guarantee(consistency_level_t value_level,
                                         consistency_level_t path_level);

/* ── Session guarantees (client-side tracking) ─────────────────────────── */

/* A client's session clock: tracks what this client has written (RYW) and the
 * highest clock it has read (monotonic reads). Carried across the fleet so a
 * client keeps its guarantees regardless of which replica it hits. */
typedef struct {
    ai_hlc_t    last_write;    /* for read-your-writes */
    ai_hlc_t    last_read;     /* for monotonic reads  */
    bool        has_read;
    bool        has_written;
} rs_session_t;

void rs_session_record_write(rs_session_t *s, const ai_hlc_t *at);
void rs_session_record_read(rs_session_t *s, const ai_hlc_t *at);

/* Can this read honor read-your-writes / monotonic-reads for the session?
 * A replica value satisfies the session iff its clock is >= the session's
 * recorded clock for that guarantee. */
bool rs_session_satisfies_ryw(const rs_session_t *s, const rs_version_t *v);
bool rs_session_satisfies_monotonic(const rs_session_t *s, const rs_version_t *v);

/* ── Level-aware resolution ────────────────────────────────────────────── */

/* Resolve two versions of the same key by their consistency levels.
 *   - both LINEARIZABLE -> HLC order wins (serialized; CAS semantics)
 *   - any CAUSAL        -> vector-clock causal order; concurrent -> Jev resolver
 *   - else              -> LWW on HLC/wall (deterministic)
 * Returns the winner. Defer semantic conflicts to the injected resolver only
 * when writes are causally concurrent (same rule as ai_repl). */
const rs_version_t *repl_sem_resolve(const rs_version_t *a, const rs_version_t *b,
                                     ai_repl_resolve_fn resolve, void *ctx,
                                     double *confidence_out);

/* Compare-and-swap (graphsub's native atomic op) across the unified layer:
 * succeeds iff the stored value matches `expected` (linearizable semantics),
 * then installs `desired`. Returns true on success. This is the strong
 * consistency primitive; it is only valid on a LINEARIZABLE path. */
bool repl_sem_cas(rs_version_t *stored, const rs_version_t *expected,
                  const rs_version_t *desired);

#endif /* DSCO_REPL_SEM_H */
