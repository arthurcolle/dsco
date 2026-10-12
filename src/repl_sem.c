#include "repl_sem.h"

#include <stdio.h>
#include <string.h>

/* repl_sem.c — unified replication memory semantics. See repl_sem.h.
 * Deterministic, network-free; the AI resolver is injected. */

/* ── Hybrid Logical Clock ──────────────────────────────────────────────── */

void ai_hlc_now(ai_hlc_t *hlc, uint64_t wall_ms, uint32_t node) {
    if (!hlc) return;
    if (wall_ms > hlc->physical) {
        hlc->physical = wall_ms;
        hlc->logical = 0;
    } else {
        hlc->logical++;
    }
    hlc->node = node;
}

void ai_hlc_recv(ai_hlc_t *hlc, uint64_t wall_ms, uint32_t node, const ai_hlc_t *remote) {
    if (!hlc || !remote) return;
    uint64_t maxp = wall_ms;
    if (hlc->physical > maxp) maxp = hlc->physical;
    if (remote->physical > maxp) maxp = remote->physical;

    if (maxp == hlc->physical && maxp == remote->physical)
        hlc->logical = (hlc->logical > remote->logical ? hlc->logical : remote->logical) + 1;
    else if (maxp == hlc->physical)
        hlc->logical = hlc->logical + 1;
    else if (maxp == remote->physical)
        hlc->logical = remote->logical + 1;
    else
        hlc->logical = 0;
    hlc->physical = maxp;
    hlc->node = node;
}

int ai_hlc_compare(const ai_hlc_t *a, const ai_hlc_t *b) {
    if (a->physical != b->physical) return a->physical < b->physical ? -1 : 1;
    if (a->logical != b->logical) return a->logical < b->logical ? -1 : 1;
    if (a->node != b->node) return a->node < b->node ? -1 : 1;
    return 0;
}

/* ── Verifiable read guarantee ─────────────────────────────────────────── */

static consistency_level_t weaker(consistency_level_t a, consistency_level_t b) {
    /* LINEARIZABLE(0) strongest, EVENTUAL(2) weakest; the read is as weak as
     * the weaker of the two. */
    return a > b ? a : b;
}

read_guarantee_t repl_sem_read_guarantee(consistency_level_t value_level,
                                         consistency_level_t path_level) {
    consistency_level_t w = weaker(value_level, path_level);
    switch (w) {
        case CONSISTENCY_LINEARIZABLE: return GUARANTEE_LINEARIZABLE;
        case CONSISTENCY_CAUSAL:       return GUARANTEE_MONOTONIC;
        default:                       return GUARANTEE_EVENTUAL;
    }
}

/* ── Session guarantees ────────────────────────────────────────────────── */

void rs_session_record_write(rs_session_t *s, const ai_hlc_t *at) {
    if (!s || !at) return;
    if (!s->has_written || ai_hlc_compare(at, &s->last_write) > 0) s->last_write = *at;
    s->has_written = true;
}

void rs_session_record_read(rs_session_t *s, const ai_hlc_t *at) {
    if (!s || !at) return;
    if (!s->has_read || ai_hlc_compare(at, &s->last_read) > 0) s->last_read = *at;
    s->has_read = true;
}

bool rs_session_satisfies_ryw(const rs_session_t *s, const rs_version_t *v) {
    if (!s || !s->has_written) return true;   /* nothing written to read back */
    if (!v) return false;
    return ai_hlc_compare(&v->hlc, &s->last_write) >= 0;
}

bool rs_session_satisfies_monotonic(const rs_session_t *s, const rs_version_t *v) {
    if (!s || !s->has_read) return true;      /* nothing read yet */
    if (!v) return false;
    return ai_hlc_compare(&v->hlc, &s->last_read) >= 0;
}

/* ── Level-aware resolution ────────────────────────────────────────────── */

const rs_version_t *repl_sem_resolve(const rs_version_t *a, const rs_version_t *b,
                                     ai_repl_resolve_fn resolve, void *ctx,
                                     double *confidence_out) {
    if (confidence_out) *confidence_out = 1.0;
    if (!a) return b;
    if (!b) return a;

    /* Both strong: serialize by HLC (CAS / linearizable semantics). */
    if (a->level == CONSISTENCY_LINEARIZABLE && b->level == CONSISTENCY_LINEARIZABLE)
        return ai_hlc_compare(&a->hlc, &b->hlc) >= 0 ? a : b;

    /* Causal path: vector-clock causal order. compare returns -1 (a<b),
     * +1 (a>b), 0 (equal), 2 (concurrent). Only +1/-1 imply dominance; 2 is
     * concurrent and MUST fall through to the resolver, not be treated as a win. */
    int ord = ai_vclock_compare(&a->vclock, &b->vclock);
    if (ord == 1) return a;
    if (ord == -1) return b;

    /* Causally concurrent: defer semantic conflicts to the calibrated resolver. */
    if (resolve) {
        ai_version_t av = {0}, bv = {0};
        snprintf(av.key, sizeof(av.key), "%s", a->key);
        snprintf(av.value, sizeof(av.value), "%s", a->value);
        av.origin_node = a->origin_node; av.tombstone = a->tombstone;
        av.clock = a->vclock; av.wall_ts = a->hlc.physical;
        snprintf(bv.key, sizeof(bv.key), "%s", b->key);
        snprintf(bv.value, sizeof(bv.value), "%s", b->value);
        bv.origin_node = b->origin_node; bv.tombstone = b->tombstone;
        bv.clock = b->vclock; bv.wall_ts = b->hlc.physical;
        double conf = 0.0;
        int keep = resolve(&av, &bv, &conf, ctx);
        if (confidence_out) *confidence_out = conf;
        if (keep == 0) return a;
        if (keep == 1) return b;
    }

    /* Deterministic LWW fallback on HLC. */
    return ai_hlc_compare(&a->hlc, &b->hlc) >= 0 ? a : b;
}

/* ── Compare-and-swap (linearizable) ───────────────────────────────────── */

bool repl_sem_cas(rs_version_t *stored, const rs_version_t *expected,
                  const rs_version_t *desired) {
    if (!stored || !expected || !desired) return false;
    /* CAS is only valid on a linearizable path. */
    if (stored->level != CONSISTENCY_LINEARIZABLE ||
        expected->level != CONSISTENCY_LINEARIZABLE ||
        desired->level != CONSISTENCY_LINEARIZABLE)
        return false;
    /* Match on value + clock (the linearizable compare). */
    if (strcmp(stored->value, expected->value) != 0) return false;
    if (ai_hlc_compare(&stored->hlc, &expected->hlc) != 0) return false;
    /* Install desired. */
    *stored = *desired;
    return true;
}
