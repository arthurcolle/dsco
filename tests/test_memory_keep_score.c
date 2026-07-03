/* test_memory_keep_score.c — doctrine/MEMORY.md Rule 0 (keep_score).
 * Verifies the computable promotion/retrieval scorer added 2026-06-28.
 * Regression target: an OLD high-importance failure-lesson must out-rank a
 * fresh trivial trace in PROMOTION mode (recency must not erase a lesson),
 * while RETRIEVAL mode prefers the fresh trace. Parity with keep_score.py. */
#include "memory_tier.h"
#include "vm.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <math.h>

/* Stubs for runtime globals defined in objects excluded from this test link
 * (main.o, agent.o). Mirrors tests/test_session_memory.c so the full LIB_OBJS
 * set (incl. tools.o, which references g_vm) resolves. */
volatile int g_interrupted = 0;
double g_cost_budget = 0.0;
int g_cheap_mode = 0;
vm_t g_vm = {0};

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } \
                           else printf("ok: %s\n", msg); } while (0)

int main(void) {
    const double now = 1000000.0;

    memory_entry_t old_fail;     memset(&old_fail, 0, sizeof old_fail);
    old_fail.created_at = now - 4 * 86400.0;   /* 4 days old  */
    old_fail.importance = 0.80;                /* failure-lesson */

    memory_entry_t fresh_trivial; memset(&fresh_trivial, 0, sizeof fresh_trivial);
    fresh_trivial.created_at = now - 5.0;      /* 5s old */
    fresh_trivial.importance = 0.10;

    double p_old = memory_keep_score(&old_fail, MEM_KEEP_PROMOTION, 0.5, now);
    double p_new = memory_keep_score(&fresh_trivial, MEM_KEEP_PROMOTION, 0.5, now);
    double r_old = memory_keep_score(&old_fail, MEM_KEEP_RETRIEVAL, 0.5, now);
    double r_new = memory_keep_score(&fresh_trivial, MEM_KEEP_RETRIEVAL, 0.5, now);

    printf("PROMOTION old=%.3f new=%.3f | RETRIEVAL old=%.3f new=%.3f\n",
           p_old, p_new, r_old, r_new);

    CHECK(p_old > p_new, "promotion keeps old failure-lesson over fresh trivial");
    CHECK(r_new > r_old, "retrieval prefers fresh over old");
    CHECK(fabs(p_old - 0.670) < 0.01, "parity with keep_score.py (old promotion ~0.67)");
    CHECK(p_old >= 0.0 && p_old <= 1.0, "score within [0,1]");
    CHECK(memory_keep_score(NULL, MEM_KEEP_PROMOTION, 0.5, now) == 0.0, "NULL guard");

    /* importance clamping: out-of-range importance must not break bounds */
    memory_entry_t weird; memset(&weird, 0, sizeof weird);
    weird.created_at = now; weird.importance = 9.9;
    double s = memory_keep_score(&weird, MEM_KEEP_PROMOTION, 9.9, now);
    CHECK(s <= 1.0 && s >= 0.0, "clamps out-of-range importance/relevance");

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASS\n", fails);
    return fails ? 1 : 0;
}
