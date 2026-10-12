/* test_ai_repl_sim.c — a real distributed replication simulation.
 * N replicas, causal writes, a network partition that splits the fleet, a
 * semantic conflict on heal resolved by the calibrated resolver, Byzantine
 * nodes dropped by read-repair, and trust-weighted quorum. Exercises ai_repl +
 * repl_sem together under adversarial conditions. The resolver here is a
 * deterministic stand-in for Jev (the live Jev resolver is the same shape). */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "repl_sem.h"

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", m, __LINE__); g_fail++; } } while(0)

/* ── The calibrated resolver stand-in (Jev-shaped): resolve a semantic conflict
 * by preferring the value that is more specific/complete. Returns index 0/1. */
static int sim_resolve(const ai_version_t *a, const ai_version_t *b,
                       double *confidence, void *ctx) {
    (void)ctx;
    /* heuristic Jev-stand-in: the more complete (longer, non-empty) value wins */
    int keep = strlen(a->value) >= strlen(b->value) ? 0 : 1;
    *confidence = 0.9;
    return keep;
}

/* The Byzantine verifier stand-in: distrust claims containing "LIE". */
static double sim_verify(const char *key, const ai_version_t *claim, void *ctx) {
    (void)key; (void)ctx;
    return strstr(claim->value, "LIE") ? 0.05 : 0.95;
}

static rs_version_t wr(const char *k, const char *v, uint32_t node, uint32_t nclock) {
    rs_version_t r; memset(&r, 0, sizeof(r));
    snprintf(r.key, sizeof(r.key), "%s", k);
    snprintf(r.value, sizeof(r.value), "%s", v);
    r.level = CONSISTENCY_CAUSAL;
    r.origin_node = node;
    ai_hlc_now(&r.hlc, 1000 + nclock * 10, node);
    for (uint32_t i = 0; i < nclock; i++) ai_vclock_tick(&r.vclock, node);
    return r;
}

int main(void) {
    printf("=== distributed replication simulation ===\n");

    /* ── Scenario 1: causal write ordering across replicas ── */
    /* Node 1 writes, then node 2 reads-that-write and writes a follow-up.
     * Node 2's write causally dominates (it saw node 1's clock). */
    rs_version_t w1 = wr("config", "threshold=5", 1, 1);
    rs_version_t w2 = w1;   /* node 2 starts from w1's clock */
    snprintf(w2.value, sizeof(w2.value), "threshold=8");
    ai_vclock_tick(&w2.vclock, 2);   /* node 2's follow-up write */
    w2.origin_node = 2;
    ai_hlc_now(&w2.hlc, 1030, 2);

    const rs_version_t *m = repl_sem_resolve(&w1, &w2, NULL, NULL, NULL);
    CHECK(m == &w2, "causal follow-up write dominates (no lost update)");
    printf("  [1] causal ordering: w2 (saw w1) correctly dominates w1\n");

    /* ── Scenario 2: partition -> concurrent writes -> heal -> conflict ── */
    /* Two nodes write the SAME key with NO shared history (a partition).
     * Neither causally dominates -> a genuine semantic conflict. */
    rs_version_t left  = wr("fact", "sky is blue", 1, 1);
    rs_version_t right = wr("fact", "sky is blue and clear today", 2, 1);
    /* independent clocks: concurrent */
    CHECK(ai_vclock_compare(&left.vclock, &right.vclock) == 2, "partition writes are concurrent");

    double conf = 0.0;
    const rs_version_t *winner = repl_sem_resolve(&left, &right, sim_resolve, NULL, &conf);
    CHECK(winner == &right, "concurrent conflict resolved by resolver to the more complete value");
    CHECK(conf > 0.8, "resolver confidence recorded");
    printf("  [2] partition-heal conflict: resolver picked more complete value (conf %.2f)\n", conf);

    /* ── Scenario 3: Byzantine node -> read-repair drops the lie ── */
    ai_version_t versions[3]; memset(versions, 0, sizeof(versions));
    snprintf(versions[0].key, AI_REPL_KEY_MAX, "balance"); snprintf(versions[0].value, AI_REPL_VAL_MAX, "100");
    snprintf(versions[1].key, AI_REPL_KEY_MAX, "balance"); snprintf(versions[1].value, AI_REPL_VAL_MAX, "100");
    snprintf(versions[2].key, AI_REPL_KEY_MAX, "balance"); snprintf(versions[2].value, AI_REPL_VAL_MAX, "LIE: 1000000");
    for (int i = 0; i < 3; i++) { versions[i].origin_node = (uint32_t)i; ai_vclock_tick(&versions[i].clock, (uint32_t)i); }
    double reps[3] = {0.9, 0.9, 0.4};   /* node 2 is low-trust */
    ai_version_t repaired; double rconf = 0.0;
    bool ok = ai_repl_read_repair("balance", versions, 3, reps, sim_verify, sim_resolve,
                                  NULL, 0.5, &repaired, &rconf);
    CHECK(ok, "read-repair found a trustworthy value");
    CHECK(strcmp(repaired.value, "100") == 0, "Byzantine lie dropped, honest value wins");
    printf("  [3] read-repair: Byzantine 'LIE' claim dropped, honest '100' kept (conf %.2f)\n", rconf);

    /* ── Scenario 4: all-Byzantine -> no trustworthy value ── */
    snprintf(versions[0].value, AI_REPL_VAL_MAX, "LIE a"); snprintf(versions[1].value, AI_REPL_VAL_MAX, "LIE b");
    double lowreps[3] = {0.1, 0.1, 0.1};
    ok = ai_repl_read_repair("balance", versions, 3, lowreps, sim_verify, sim_resolve,
                             NULL, 0.5, &repaired, &rconf);
    CHECK(!ok, "all-distrusted claims yield no value (fail safe)");
    printf("  [4] all-Byzantine: correctly returns no value\n");

    /* ── Scenario 5: trust-weighted quorum ── */
    /* 2 high-trust nodes respond; 3 low-trust stay silent. Count-based quorum
     * would fail (2 of 5), but trust-weighted quorum passes. */
    ai_quorum_node_t qn[5] = {
        {.reputation=0.95, .responded=true},  {.reputation=0.9, .responded=true},
        {.reputation=0.1,  .responded=false}, {.reputation=0.1, .responded=false},
        {.reputation=0.05, .responded=false},
    };
    double qconf = 0.0;
    CHECK(ai_repl_quorum(qn, 5, 0.7, &qconf), "trust-weighted quorum reached by 2 high-trust nodes");
    printf("  [5] trust quorum: 2 high-trust nodes reach quorum (confidence %.2f)\n", qconf);
    /* invert: only low-trust respond -> no quorum */
    for (int i = 0; i < 5; i++) qn[i].responded = (i >= 2);
    CHECK(!ai_repl_quorum(qn, 5, 0.7, &qconf), "low-trust responses do not reach quorum");
    printf("  [5b] and 3 low-trust nodes correctly do NOT reach quorum\n");

    /* ── Scenario 6: session guarantees across a replica switch ── */
    /* Client writes to node A, then reads from lagging node B. RYW must catch it. */
    rs_session_t sess; memset(&sess, 0, sizeof(sess));
    rs_version_t client_write = wr("pref", "dark-mode", 1, 1);
    rs_session_record_write(&sess, &client_write.hlc);
    rs_version_t lagging = wr("pref", "light-mode", 2, 0);   /* node B hasn't seen the write */
    lagging.hlc.physical = client_write.hlc.physical - 50;   /* behind */
    CHECK(!rs_session_satisfies_ryw(&sess, &lagging), "lagging replica violates read-your-writes");
    printf("  [6] session: RYW correctly refuses a stale read after replica switch\n");

    if (g_fail) { fprintf(stderr, "\nai_repl_sim: %d FAILURE(S)\n", g_fail); return 1; }
    printf("\nai_repl_sim: all scenarios passed — causal ordering, partition-heal conflict, "
           "Byzantine read-repair, trust quorum, session guarantees\n");
    return 0;
}
