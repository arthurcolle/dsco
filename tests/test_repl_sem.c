/* test_repl_sem.c — unit tests for unified replication memory semantics.
 * Deterministic, network-free: proves HLC skew-tolerance, the verifiable
 * downgrade, session guarantees, level-aware resolution, and CAS. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "repl_sem.h"

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", m, __LINE__); g_fail++; } } while(0)

static rs_version_t mk(const char *k, const char *v, consistency_level_t lvl,
                       uint64_t phys, uint32_t logical, uint32_t node) {
    rs_version_t r; memset(&r, 0, sizeof(r));
    snprintf(r.key, sizeof(r.key), "%s", k);
    snprintf(r.value, sizeof(r.value), "%s", v);
    r.level = lvl; r.hlc.physical = phys; r.hlc.logical = logical; r.hlc.node = node;
    r.origin_node = node;
    return r;
}

int main(void) {
    /* ── HLC: never goes backward under clock skew ── */
    ai_hlc_t h = {0};
    ai_hlc_now(&h, 1000, 1);
    CHECK(h.physical == 1000 && h.logical == 0, "hlc initial");
    ai_hlc_now(&h, 500, 1);   /* wall clock went BACKWARD (skew) */
    CHECK(h.physical == 1000 && h.logical == 1, "hlc resists backward skew (physical holds, logical++)");
    ai_hlc_now(&h, 2000, 1);
    CHECK(h.physical == 2000 && h.logical == 0, "hlc advances on newer wall time");

    /* HLC recv: merges remote, never backward */
    ai_hlc_t local = {0}, remote = {0};
    ai_hlc_now(&local, 1000, 1);
    remote.physical = 3000; remote.logical = 5; remote.node = 2;
    ai_hlc_recv(&local, 900, 1, &remote);   /* local wall behind remote */
    CHECK(local.physical == 3000 && local.logical == 6, "hlc recv jumps to max physical, logical+1");

    /* HLC total order */
    ai_hlc_t x = {.physical=100,.logical=1,.node=1}, y = {.physical=100,.logical=1,.node=2};
    CHECK(ai_hlc_compare(&x,&y) < 0, "hlc node tiebreak");

    /* ── Verifiable downgrade: strong value on weak path is NOT linearizable ── */
    CHECK(repl_sem_read_guarantee(CONSISTENCY_LINEARIZABLE, CONSISTENCY_LINEARIZABLE)
          == GUARANTEE_LINEARIZABLE, "strong on strong = linearizable");
    CHECK(repl_sem_read_guarantee(CONSISTENCY_LINEARIZABLE, CONSISTENCY_EVENTUAL)
          == GUARANTEE_EVENTUAL, "strong value on weak path downgrades to eventual (honest)");
    CHECK(repl_sem_read_guarantee(CONSISTENCY_CAUSAL, CONSISTENCY_LINEARIZABLE)
          == GUARANTEE_MONOTONIC, "causal value on strong path = monotonic");

    /* ── Session guarantees: read-your-writes + monotonic reads ── */
    rs_session_t s; memset(&s, 0, sizeof(s));
    rs_version_t w1 = mk("k","v1",CONSISTENCY_EVENTUAL, 100,0,1);
    rs_session_record_write(&s, &w1.hlc);
    rs_version_t older = mk("k","stale",CONSISTENCY_EVENTUAL, 50,0,2);   /* stale replica */
    rs_version_t newer = mk("k","v1",CONSISTENCY_EVENTUAL, 150,0,2);     /* caught-up replica */
    CHECK(!rs_session_satisfies_ryw(&s, &older), "stale replica violates RYW");
    CHECK(rs_session_satisfies_ryw(&s, &newer), "caught-up replica satisfies RYW");
    rs_session_record_read(&s, &newer.hlc);
    rs_version_t regressed = mk("k","v1",CONSISTENCY_EVENTUAL, 120,0,3);
    CHECK(!rs_session_satisfies_monotonic(&s, &regressed), "regressed replica violates monotonic-reads");

    /* ── Level-aware resolution: strong vs strong serializes by HLC ── */
    rs_version_t s_old = mk("k","old",CONSISTENCY_LINEARIZABLE, 100,0,1);
    rs_version_t s_new = mk("k","new",CONSISTENCY_LINEARIZABLE, 200,0,1);
    CHECK(repl_sem_resolve(&s_old,&s_new,NULL,NULL,NULL) == &s_new, "strong: newer HLC wins");
    CHECK(repl_sem_resolve(&s_new,&s_old,NULL,NULL,NULL) == &s_new, "strong: order-independent");

    /* causal: vector clock dominance. Genuine dominance requires shared
     * history: c2 saw c1's clock and then advanced its own, so c2 dominates.
     * (Two nodes ticking independently are CONCURRENT, not dominant.) */
    rs_version_t c1 = mk("k","a",CONSISTENCY_CAUSAL, 100,0,1);
    ai_vclock_tick(&c1.vclock, 1);                 /* node1 writes */
    rs_version_t c2 = c1;                          /* node2 starts from c1's clock */
    snprintf(c2.value, sizeof(c2.value), "b");
    c2.origin_node = 2;
    ai_vclock_tick(&c2.vclock, 2);                 /* node2 follow-up write */
    CHECK(ai_vclock_compare(&c1.vclock,&c2.vclock) == -1, "c2 causally dominates c1");
    CHECK(repl_sem_resolve(&c1,&c2,NULL,NULL,NULL) == &c2, "causal: dominant clock wins");
    /* concurrent: independent writes on different nodes are NOT dominance */
    rs_version_t i1 = mk("k","x",CONSISTENCY_CAUSAL, 100,0,1);
    rs_version_t i2 = mk("k","y",CONSISTENCY_CAUSAL, 100,0,2);
    ai_vclock_tick(&i1.vclock, 1);
    ai_vclock_tick(&i2.vclock, 2);
    CHECK(ai_vclock_compare(&i1.vclock,&i2.vclock) == 2, "independent writes are concurrent");

    /* ── CAS (linearizable) ── */
    rs_version_t stored = mk("ctr","5",CONSISTENCY_LINEARIZABLE, 100,0,1);
    rs_version_t expect_ok = stored;
    rs_version_t desired = mk("ctr","6",CONSISTENCY_LINEARIZABLE, 200,0,1);
    CHECK(repl_sem_cas(&stored,&expect_ok,&desired), "CAS succeeds on match");
    CHECK(strcmp(stored.value,"6")==0, "CAS installed new value");
    rs_version_t expect_bad = mk("ctr","5",CONSISTENCY_LINEARIZABLE, 100,0,1);
    CHECK(!repl_sem_cas(&stored,&expect_bad,&desired), "CAS fails on mismatch (no lost update)");
    /* CAS rejected on a non-linearizable path */
    rs_version_t weak = mk("k","v",CONSISTENCY_EVENTUAL, 100,0,1);
    CHECK(!repl_sem_cas(&weak,&weak,&desired), "CAS rejected on weak path");

    if (g_fail) { fprintf(stderr, "repl_sem: %d FAILURE(S)\n", g_fail); return 1; }
    printf("repl_sem: all tests passed (HLC skew-tolerance, verifiable downgrade, session guarantees, level-aware resolution, CAS)\n");
    return 0;
}
