#include "ai_repl.h"

#include <string.h>

/* ai_repl.c — AI-verified distributed replication semantics. See ai_repl.h.
 * Deterministic data-plane (vector clocks, CRDT LWW, trust-weighted quorum) is
 * pure and network-free; the AI resolver/verifier are injected. */

/* ── Vector clocks ─────────────────────────────────────────────────────── */

static size_t idx_of(const ai_vclock_t *vc, uint32_t node) {
    for (size_t i = 0; i < vc->n; i++)
        if (vc->node_index[i] == node) return i;
    return (size_t)-1;
}

void ai_vclock_tick(ai_vclock_t *vc, uint32_t node) {
    if (!vc) return;
    size_t i = idx_of(vc, node);
    if (i == (size_t)-1) {
        if (vc->n >= AI_REPL_MAX_NODES) return;
        i = vc->n++;
        vc->node_index[i] = node;
        vc->tick[i] = 0;
    }
    vc->tick[i]++;
}

void ai_vclock_merge(ai_vclock_t *dst, const ai_vclock_t *src) {
    if (!dst || !src) return;
    for (size_t i = 0; i < src->n; i++) {
        uint32_t node = src->node_index[i];
        size_t j = idx_of(dst, node);
        if (j == (size_t)-1) {
            if (dst->n >= AI_REPL_MAX_NODES) continue;
            j = dst->n++;
            dst->node_index[j] = node;
            dst->tick[j] = 0;
        }
        if (src->tick[i] > dst->tick[j]) dst->tick[j] = src->tick[i];
    }
}

static uint64_t clock_get(const ai_vclock_t *vc, uint32_t node) {
    size_t i = idx_of(vc, node);
    return i == (size_t)-1 ? 0 : vc->tick[i];
}

/* Partial order. a dominates b if every component of a >= b and at least one
 * strictly greater. Concurrent if neither dominates. */
int ai_vclock_compare(const ai_vclock_t *a, const ai_vclock_t *b) {
    if (!a || !b) return 2;
    bool a_gt = false, b_gt = false;
    /* union of nodes */
    for (size_t i = 0; i < a->n; i++) {
        uint32_t node = a->node_index[i];
        uint64_t av = a->tick[i], bv = clock_get(b, node);
        if (av > bv) a_gt = true;
        if (bv > av) b_gt = true;
    }
    for (size_t i = 0; i < b->n; i++) {
        uint32_t node = b->node_index[i];
        uint64_t bv = b->tick[i], av = clock_get(a, node);
        if (bv > av) b_gt = true;
        if (av > bv) a_gt = true;
    }
    if (a_gt && !b_gt) return 1;
    if (b_gt && !a_gt) return -1;
    if (!a_gt && !b_gt) return 0;
    return 2;   /* concurrent */
}

/* ── CRDT LWW merge ────────────────────────────────────────────────────── */

const ai_version_t *ai_repl_lww_merge(const ai_version_t *a, const ai_version_t *b) {
    if (!a) return b;
    if (!b) return a;
    int ord = ai_vclock_compare(&a->clock, &b->clock);
    /* compare returns -1/0/+1/2; only +1/-1 are dominance. 2 (concurrent) must
     * fall through to the tiebreak, not be treated as "a wins". */
    if (ord == 1) return a;
    if (ord == -1) return b;
    /* concurrent or equal: deterministic tiebreak on wall_ts then origin. */
    if (a->wall_ts != b->wall_ts) return a->wall_ts > b->wall_ts ? a : b;
    if (a->origin_node != b->origin_node) return a->origin_node > b->origin_node ? a : b;
    /* full tie: lexicographic on value for total determinism */
    return strcmp(a->value, b->value) >= 0 ? a : b;
}

/* ── Trust-weighted quorum ─────────────────────────────────────────────── */

bool ai_repl_quorum(const ai_quorum_node_t *nodes, size_t n,
                    double quorum_fraction, double *confidence) {
    if (confidence) *confidence = 0.0;
    if (!nodes || n == 0 || quorum_fraction <= 0.0 || quorum_fraction > 1.0)
        return false;
    double total = 0.0, responded = 0.0;
    for (size_t i = 0; i < n; i++) {
        double rep = nodes[i].reputation;
        if (rep < 0.0) rep = 0.0;
        if (rep > 1.0) rep = 1.0;
        total += rep;
        if (nodes[i].responded) responded += rep;
    }
    if (total <= 0.0) return false;
    double frac = responded / total;
    if (confidence) *confidence = frac;
    return frac >= quorum_fraction;
}

/* ── Causal + AI resolution ────────────────────────────────────────────── */

const ai_version_t *ai_repl_resolve(const ai_version_t *a, const ai_version_t *b,
                                    ai_repl_resolve_fn resolve, void *ctx,
                                    double *confidence_out) {
    if (confidence_out) *confidence_out = 1.0;
    if (!a) return b;
    if (!b) return a;
    int ord = ai_vclock_compare(&a->clock, &b->clock);
    /* Causality dominates: no AI needed. compare returns -1/0/+1/2; only +1/-1
     * are dominance. 2 (concurrent) must reach the resolver below. */
    if (ord == 1) return a;
    if (ord == -1) return b;
    /* Concurrent writes: this is the only case where an arbitrary LWW tiebreak
     * can destroy semantic meaning. Defer to the AI resolver if provided. */
    if (resolve) {
        double conf = 0.0;
        int keep = resolve(a, b, &conf, ctx);
        if (confidence_out) *confidence_out = conf;
        if (keep == 0) return a;
        if (keep == 1) return b;
        /* resolver abstained: fall through to deterministic LWW */
    }
    const ai_version_t *w = ai_repl_lww_merge(a, b);
    return w;
}

/* ── Read-repair across replicas ───────────────────────────────────────── */

bool ai_repl_read_repair(const char *key,
                         const ai_version_t *versions, size_t n,
                         const double *reputations,
                         ai_repl_verify_fn verify, ai_repl_resolve_fn resolve,
                         void *ctx, double trust_threshold,
                         ai_version_t *out, double *confidence_out) {
    if (confidence_out) *confidence_out = 0.0;
    if (!key || !versions || !out || n == 0) return false;

    /* 1. Byzantine filter: drop claims the verifier distrusts. */
    const ai_version_t *trusted[AI_REPL_MAX_NODES];
    double trust_w[AI_REPL_MAX_NODES];
    size_t tn = 0;
    for (size_t i = 0; i < n && tn < AI_REPL_MAX_NODES; i++) {
        double sound = verify ? verify(key, &versions[i], ctx) : 1.0;
        double rep = reputations ? reputations[i] : 1.0;
        double w = sound * rep;
        if (w >= trust_threshold) {
            trusted[tn] = &versions[i];
            trust_w[tn] = w;
            tn++;
        }
    }
    if (tn == 0) return false;   /* nothing trustworthy */

    /* 2. Fold trusted versions by causal + AI resolution. */
    const ai_version_t *winner = trusted[0];
    double conf_sum = trust_w[0];
    for (size_t i = 1; i < tn; i++) {
        double c = 0.0;
        winner = ai_repl_resolve(winner, trusted[i], resolve, ctx, &c);
        conf_sum += trust_w[i];
    }

    *out = *winner;
    /* Confidence in the read: weighted fraction of total trust that agreed. */
    double total_w = 0.0;
    for (size_t i = 0; i < n; i++) total_w += reputations ? reputations[i] : 1.0;
    if (confidence_out)
        *confidence_out = total_w > 0.0 ? conf_sum / total_w : 0.0;
    return true;
}
