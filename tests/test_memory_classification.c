/* test_memory_classification.c — doctrine/CLASSIFICATION.md §5 memory gates.
 * Verifies the sovereign classification levels (L0 OPEN / L1 HELD / L2 SEALED /
 * L3 UMBRAL) enforce their consolidation contracts:
 *   - L2 never auto-promotes episodic->semantic without review (consolidation
 *     leak counter, SECRECY_HARDENING §7)
 *   - L3 never leaves working memory without review; purged at session close
 *   - review grants exactly ONE promotion
 *   - no in-band declassification (labels only rise via memory_classify)
 *   - re-labeling consolidated material to L3 demotes it back to working
 *   - verified deletion: purge zeroes value bytes, not just the active flag */
#include "memory_tier.h"
#include "vm.h"
#include <stdio.h>
#include <string.h>

/* Stubs mirroring tests/test_memory_keep_score.c for full LIB_OBJS link. */
volatile int g_interrupted = 0;
double g_cost_budget = 0.0;
int g_cheap_mode = 0;
vm_t g_vm = {0};

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } \
                           else printf("ok: %s\n", msg); } while (0)

int main(void) {
    memory_store_t m;
    memory_store_init(&m);

    /* ── L0/L1 consolidate freely ─────────────────────────────────────── */
    memory_store(&m, MEM_EPISODIC, "open-fact", "public", 0.9);
    CHECK(memory_promote(&m, "open-fact"), "L0 OPEN promotes freely");

    memory_store(&m, MEM_EPISODIC, "held-fact", "roadmap", 0.9);
    CHECK(memory_classify(&m, "held-fact", MEM_CLASS_HELD), "classify at L1");
    CHECK(memory_promote(&m, "held-fact"), "L1 HELD promotes freely");

    /* ── L2 SEALED: episodic->semantic gated on review ────────────────── */
    memory_store(&m, MEM_WORKING, "sealed-pos", "position detail", 0.95);
    CHECK(memory_classify(&m, "sealed-pos", MEM_CLASS_SEALED), "classify at L2");
    CHECK(memory_promote(&m, "sealed-pos"), "L2 working->episodic is free");
    CHECK(!memory_promote(&m, "sealed-pos"), "L2 episodic->semantic BLOCKED without review");
    CHECK(memory_classify_review(&m, "sealed-pos"), "review grant on L2");
    CHECK(memory_promote(&m, "sealed-pos"), "L2 promotes after review");

    /* ── review grants exactly one promotion ──────────────────────────── */
    memory_store(&m, MEM_EPISODIC, "sealed-2", "counterparty intel", 0.9);
    memory_classify(&m, "sealed-2", MEM_CLASS_SEALED);
    memory_classify_review(&m, "sealed-2");
    CHECK(memory_promote(&m, "sealed-2"), "review permits the promotion");
    const memory_entry_t *s2 = memory_recall(&m, "sealed-2");
    CHECK(s2 && !s2->class_reviewed, "review flag consumed (one review = one promotion)");

    /* ── L3 UMBRAL: never leaves working without review ───────────────── */
    memory_store(&m, MEM_WORKING, "umbral-1", "acquisition interest", 1.0);
    CHECK(memory_classify(&m, "umbral-1", MEM_CLASS_UMBRAL), "classify at L3");
    CHECK(!memory_promote(&m, "umbral-1"), "L3 BLOCKED from leaving working memory");
    CHECK(memory_classify_review(&m, "umbral-1"), "Tier-0 review grant on L3");
    CHECK(memory_promote(&m, "umbral-1"), "L3 promotes only after explicit review");

    /* ── no in-band declassification ──────────────────────────────────── */
    CHECK(!memory_classify(&m, "umbral-1", MEM_CLASS_OPEN),
          "declassification via memory_classify REFUSED (labels only rise)");
    CHECK(!memory_classify(&m, "sealed-pos", MEM_CLASS_HELD),
          "downgrade L2->L1 in-band REFUSED");

    /* ── late L3 label demotes consolidated material back to working ──── */
    memory_store(&m, MEM_SEMANTIC, "late-secret", "already consolidated", 0.9);
    CHECK(memory_classify(&m, "late-secret", MEM_CLASS_UMBRAL), "late re-label to L3");
    const memory_entry_t *ls = memory_recall(&m, "late-secret");
    CHECK(ls && ls->tier == MEM_WORKING, "late L3 label DEMOTES entry to working (leak recall)");

    /* ── sweep never auto-promotes gated material ─────────────────────── */
    /* umbral-2: pinned, max importance, huge access count — everything the
     * sweep loves. Classification must still veto. */
    memory_store(&m, MEM_WORKING, "umbral-2", "invisible topic", 1.0);
    memory_classify(&m, "umbral-2", MEM_CLASS_UMBRAL);
    memory_pin(&m, "umbral-2");
    for (int i = 0; i < 10; i++) memory_recall(&m, "umbral-2");
    /* force a consolidation window */
    m.last_consolidation = 0;
    memory_consolidate(&m);
    const memory_entry_t *u2 = memory_recall(&m, "umbral-2");
    CHECK(u2 && u2->tier == MEM_WORKING,
          "consolidation sweep NEVER auto-promotes UMBRAL (importance/access irrelevant)");

    /* ── purge: verified deletion at session close ────────────────────── */
    int before = m.count;
    int purged = memory_purge_umbral(&m);
    CHECK(purged >= 2, "purge removes all UMBRAL entries (incl. pinned)");
    CHECK(m.count == before - purged, "count reconciled after purge");
    CHECK(memory_recall(&m, "umbral-2") == NULL, "purged entry unrecallable");
    /* verified deletion: scan raw slots for the secret bytes */
    int ghost = 0;
    for (int i = 0; i < MEMTIER_MAX_ENTRIES; i++)
        if (strstr(m.entries[i].value, "invisible topic")) ghost++;
    CHECK(ghost == 0, "value bytes ZEROED, not just deactivated (verified deletion)");

    /* ── unknown label fails closed ───────────────────────────────────── */
    memory_entry_t bogus; memset(&bogus, 0, sizeof bogus);
    bogus.classification = (memory_class_t)7;
    CHECK(!memory_class_promotable(&bogus), "unknown classification fails CLOSED");
    CHECK(!memory_class_promotable(NULL), "NULL guard");

    memory_store_destroy(&m);
    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASS\n", fails);
    return fails ? 1 : 0;
}
