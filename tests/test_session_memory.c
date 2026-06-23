/* test_session_memory.c — Priority 5: Memory/Context Persistence
 *
 * Build (from dsco-cli root):
 *   cc -Wall -O0 -g -std=c2y -I include \
 *      src/session_memory.c src/json_util.c \
 *      tests/test_session_memory.c \
 *      -o build/test_session_memory
 * Run:
 *   DSCO_SESSION_PATH=/tmp/dsco_test_sessions.json ./build/test_session_memory
 *
 * session_db_t is ~2.75 MB; always heap-allocate to avoid stack overflow.
 */

#include "session_memory.h"
#include "json_util.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Stubs for globals defined in objects excluded from this test link
 * (agent.o, main.o). g_agent_exit_requested lives in tools.o (linked) → omit. */
volatile int g_interrupted = 0;
double g_cost_budget = 0.0;
int g_cheap_mode = 0;
vm_t g_vm = {0};

/* ── Minimal test harness ─────────────────────────────────────────────── */

static int g_run = 0, g_pass = 0, g_fail = 0;

#define TEST(name)                                                                                 \
    do {                                                                                           \
        g_run++;                                                                                   \
        fprintf(stderr, "  %-52s ", (name));                                                       \
    } while (0)
#define PASS()                                                                                     \
    do {                                                                                           \
        g_pass++;                                                                                  \
        fprintf(stderr, "\033[32mPASS\033[0m\n");                                                  \
    } while (0)
#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        g_fail++;                                                                                  \
        fprintf(stderr, "\033[31mFAIL\033[0m: %s\n", (msg));                                       \
    } while (0)
#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            FAIL(msg);                                                                             \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* Heap-allocate a session_db_t. session_init does memset internally so the
   caller does not need to zero the memory first. */
#define ALLOC_DB(var)                                                                              \
    session_db_t *var = malloc(sizeof(*var));                                                      \
    if (!(var)) {                                                                                  \
        FAIL("OOM allocating session_db_t");                                                       \
        return;                                                                                    \
    }

#define FREE_DB(var)                                                                               \
    do {                                                                                           \
        session_free(var);                                                                         \
        free(var);                                                                                 \
    } while (0)

static void reset_test_db(void) {
    const char *p = getenv("DSCO_SESSION_PATH");
    if (p && *p)
        remove(p);
}

/* Back-date a KV entry's created_at to simulate TTL expiry. */
static void backdate_kv(session_db_t *db, const char *key, double secs) {
    for (int i = 0; i < db->kv_count; i++) {
        if (db->kv[i].active && strcmp(db->kv[i].key, key) == 0) {
            db->kv[i].created_at -= secs;
            return;
        }
    }
}

/* Force access_count for a KV entry (for promotion tests). */
static void set_kv_access_count(session_db_t *db, const char *key, int count) {
    for (int i = 0; i < db->kv_count; i++) {
        if (db->kv[i].active && strcmp(db->kv[i].key, key) == 0) {
            db->kv[i].access_count = count;
            return;
        }
    }
}

static int get_kv_access_count(session_db_t *db, const char *key) {
    for (int i = 0; i < db->kv_count; i++) {
        if (db->kv[i].active && strcmp(db->kv[i].key, key) == 0)
            return db->kv[i].access_count;
    }
    return -1;
}

static memory_tier_t get_kv_tier(session_db_t *db, const char *key) {
    for (int i = 0; i < db->kv_count; i++) {
        if (db->kv[i].active && strcmp(db->kv[i].key, key) == 0)
            return db->kv[i].tier;
    }
    return (memory_tier_t)-1;
}

/* ── Tests ────────────────────────────────────────────────────────────── */

static void test_basic_init_free(void) {
    TEST("init and free (fresh DB)");
    reset_test_db();

    ALLOC_DB(db);
    int rc = session_init(db, "test task");
    ASSERT(rc == 0, "session_init failed");
    ASSERT(db->initialized, "not initialized");
    ASSERT(db->current_session_id[0] != '\0', "no session id");
    ASSERT(db->record_count == 1, "expected 1 session record");
    ASSERT(strcmp(db->records[0].task_text, "test task") == 0, "wrong task_text");

    FREE_DB(db);
    PASS();
}

static void test_null_task_allowed(void) {
    TEST("session_init with NULL task_text is allowed");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, NULL) == 0, "init failed with NULL task");
    ASSERT(db->initialized, "not initialized");
    FREE_DB(db);
    PASS();
}

static void test_remember_and_recall(void) {
    TEST("remember + recall (same session)");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, "crypto analysis") == 0, "init failed");

    ASSERT(session_remember(db, "btc_eth_corr", "0.92", 0) == 0, "remember failed");
    ASSERT(session_remember(db, "vol_high", "true", 0) == 0, "remember failed");

    const char *v = session_recall(db, "btc_eth_corr");
    ASSERT(v != NULL, "recall returned NULL");
    ASSERT(strcmp(v, "0.92") == 0, "wrong value");

    v = session_recall(db, "vol_high");
    ASSERT(v != NULL, "recall returned NULL");
    ASSERT(strcmp(v, "true") == 0, "wrong value");

    v = session_recall(db, "nonexistent_key");
    ASSERT(v == NULL, "expected NULL for missing key");

    FREE_DB(db);
    PASS();
}

static void test_update_existing_key(void) {
    TEST("remember overwrites existing key");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, NULL) == 0, "init failed");

    ASSERT(session_remember(db, "price", "100", 0) == 0, "first store failed");
    ASSERT(session_remember(db, "price", "200", 0) == 0, "update failed");

    const char *v = session_recall(db, "price");
    ASSERT(v != NULL, "recall returned NULL");
    ASSERT(strcmp(v, "200") == 0, "value not updated");

    FREE_DB(db);
    PASS();
}

static void test_access_count_increments_on_recall(void) {
    TEST("recall increments access_count");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, NULL) == 0, "init failed");

    ASSERT(session_remember(db, "cnt_key", "val", 0) == 0, "store failed");
    ASSERT(get_kv_access_count(db, "cnt_key") == 1, "expected initial access_count == 1");

    session_recall(db, "cnt_key");
    session_recall(db, "cnt_key");
    ASSERT(get_kv_access_count(db, "cnt_key") == 3, "expected access_count == 3 after two recalls");

    FREE_DB(db);
    PASS();
}

static void test_ttl_working_infers_tier(void) {
    TEST("TTL <= 60 => working tier");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, NULL) == 0, "init failed");
    ASSERT(session_remember(db, "tmp_key", "val", SESSION_TTL_WORKING) == 0, "store failed");
    ASSERT(get_kv_tier(db, "tmp_key") == MEM_WORKING, "expected working tier");

    FREE_DB(db);
    PASS();
}

static void test_ttl_episodic_infers_tier(void) {
    TEST("TTL > 60 => episodic tier");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, NULL) == 0, "init failed");
    ASSERT(session_remember(db, "ep_key", "val", SESSION_TTL_EPISODIC) == 0, "store failed");
    ASSERT(get_kv_tier(db, "ep_key") == MEM_EPISODIC, "expected episodic tier");

    FREE_DB(db);
    PASS();
}

static void test_ttl_zero_infers_semantic(void) {
    TEST("TTL = 0 => semantic tier");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, NULL) == 0, "init failed");
    ASSERT(session_remember(db, "sem_key", "val", SESSION_TTL_SEMANTIC) == 0, "store failed");
    ASSERT(get_kv_tier(db, "sem_key") == MEM_SEMANTIC, "expected semantic tier");

    FREE_DB(db);
    PASS();
}

static void test_ttl_expiry_on_recall(void) {
    TEST("recall returns NULL after TTL expires");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, NULL) == 0, "init failed");

    ASSERT(session_remember(db, "ephemeral", "vanish", 5) == 0, "store failed");
    backdate_kv(db, "ephemeral", 10.0); /* 10s in the past — past 5s TTL */

    const char *v = session_recall(db, "ephemeral");
    ASSERT(v == NULL, "expected NULL — TTL expired");

    FREE_DB(db);
    PASS();
}

static void test_evict_expired(void) {
    TEST("session_evict_expired removes expired entries");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, NULL) == 0, "init failed");

    ASSERT(session_remember(db, "live_key", "live", 0) == 0, "store failed");
    ASSERT(session_remember(db, "dead_key", "dead", 5) == 0, "store failed");
    backdate_kv(db, "dead_key", 10.0);

    int evicted = session_evict_expired(db);
    ASSERT(evicted == 1, "expected 1 eviction");
    ASSERT(session_recall(db, "live_key") != NULL, "live key should survive");
    ASSERT(session_recall(db, "dead_key") == NULL, "dead key should be gone");

    FREE_DB(db);
    PASS();
}

static void test_cross_session_persistence(void) {
    TEST("cross-session: persist -> reinit -> recall");
    reset_test_db();

    /* Session 1: store semantic (permanent) facts */
    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "session one") == 0, "init S1 failed");
        ASSERT(session_remember(db, "market_regime", "bull", SESSION_TTL_SEMANTIC) == 0,
               "store failed");
        ASSERT(session_remember(db, "risk_level", "low", SESSION_TTL_SEMANTIC) == 0,
               "store failed");
        ASSERT(session_persist(db) == 0, "persist S1 failed");
        FREE_DB(db);
    }

    /* Session 2: load from disk and recall those keys */
    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "session two") == 0, "init S2 failed");

        const char *regime = session_recall(db, "market_regime");
        ASSERT(regime != NULL, "cross-session recall returned NULL");
        ASSERT(strcmp(regime, "bull") == 0, "wrong value after reload");

        const char *risk = session_recall(db, "risk_level");
        ASSERT(risk != NULL, "risk_level recall failed");
        ASSERT(strcmp(risk, "low") == 0, "wrong risk_level value");

        FREE_DB(db);
    }

    PASS();
}

static void test_cross_session_episodic_survives(void) {
    TEST("cross-session: episodic entry survives (within TTL)");
    reset_test_db();

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "research") == 0, "init failed");
        ASSERT(session_remember(db, "correlation", "0.87", SESSION_TTL_EPISODIC) == 0,
               "store failed");
        ASSERT(session_persist(db) == 0, "persist failed");
        FREE_DB(db);
    }

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "follow-up") == 0, "init failed");
        const char *v = session_recall(db, "correlation");
        ASSERT(v != NULL, "episodic entry should survive across sessions");
        ASSERT(strcmp(v, "0.87") == 0, "wrong value");
        FREE_DB(db);
    }

    PASS();
}

static void test_cross_session_working_expired(void) {
    TEST("cross-session: working entry evicted after TTL expires");
    reset_test_db();

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "quick scan") == 0, "init failed");
        ASSERT(session_remember(db, "tmp_scan", "results", SESSION_TTL_WORKING) == 0,
               "store failed");
        /* Simulate 2 minutes passing: backdate past the 60s working TTL */
        backdate_kv(db, "tmp_scan", 120.0);
        ASSERT(session_persist(db) == 0, "persist failed");
        FREE_DB(db);
    }

    {
        ALLOC_DB(db);
        /* session_init calls session_evict_expired, so tmp_scan should be gone */
        ASSERT(session_init(db, "next session") == 0, "init failed");
        const char *v = session_recall(db, "tmp_scan");
        ASSERT(v == NULL, "expired working entry should be evicted on load");
        FREE_DB(db);
    }

    PASS();
}

static void test_multiple_kv_entries_serialized(void) {
    TEST("multiple KV entries survive persist+reload");
    reset_test_db();

    const char *keys[] = {"alpha", "beta", "gamma", "delta", "epsilon"};
    const char *values[] = {"1", "2", "3", "4", "5"};
    int n = 5;

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, NULL) == 0, "init failed");
        for (int i = 0; i < n; i++)
            ASSERT(session_remember(db, keys[i], values[i], 0) == 0, "store failed");
        ASSERT(session_persist(db) == 0, "persist failed");
        FREE_DB(db);
    }

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, NULL) == 0, "init failed");
        int matched = 0;
        for (int i = 0; i < n; i++) {
            const char *v = session_recall(db, keys[i]);
            if (v && strcmp(v, values[i]) == 0)
                matched++;
        }
        ASSERT(matched == n, "not all entries survived reload");
        FREE_DB(db);
    }

    PASS();
}

static void test_special_chars_in_value(void) {
    TEST("values with JSON special chars survive serialization");
    reset_test_db();

    const char *tricky = "it's a \"test\" with\nnewlines\\backslashes";

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, NULL) == 0, "init failed");
        ASSERT(session_remember(db, "tricky_val", tricky, 0) == 0, "store failed");
        ASSERT(session_persist(db) == 0, "persist failed");
        FREE_DB(db);
    }

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, NULL) == 0, "init failed");
        const char *v = session_recall(db, "tricky_val");
        ASSERT(v != NULL, "tricky value not recalled");
        ASSERT(strcmp(v, tricky) == 0, "tricky value corrupted after roundtrip");
        FREE_DB(db);
    }

    PASS();
}

static void test_promotion_working_to_episodic(void) {
    TEST("promote: working -> episodic at access_count >= threshold");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, NULL) == 0, "init failed");

    ASSERT(session_remember(db, "hot_key", "val", SESSION_TTL_WORKING) == 0, "store failed");
    ASSERT(get_kv_tier(db, "hot_key") == MEM_WORKING, "should start as working");

    set_kv_access_count(db, "hot_key", SESSION_PROMOTE_THRESHOLD);
    int promoted = session_promote(db);

    ASSERT(promoted >= 1, "expected at least one promotion");
    ASSERT(get_kv_tier(db, "hot_key") == MEM_EPISODIC, "should be episodic after promote");

    FREE_DB(db);
    PASS();
}

static void test_promotion_episodic_to_semantic(void) {
    TEST("promote: episodic -> semantic at access_count >= threshold");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, NULL) == 0, "init failed");

    ASSERT(session_remember(db, "core_fact", "42", SESSION_TTL_EPISODIC) == 0, "store failed");
    ASSERT(get_kv_tier(db, "core_fact") == MEM_EPISODIC, "should start as episodic");

    set_kv_access_count(db, "core_fact", SESSION_PROMOTE_THRESHOLD);
    int promoted = session_promote(db);

    ASSERT(promoted >= 1, "expected at least one promotion");
    ASSERT(get_kv_tier(db, "core_fact") == MEM_SEMANTIC, "should be semantic after promote");

    /* Verify TTL cleared to 0 */
    for (int i = 0; i < db->kv_count; i++) {
        if (strcmp(db->kv[i].key, "core_fact") == 0) {
            ASSERT(db->kv[i].ttl_seconds == SESSION_TTL_SEMANTIC, "TTL should be 0 for semantic");
            break;
        }
    }

    FREE_DB(db);
    PASS();
}

static void test_promotion_semantic_persists_after_reload(void) {
    TEST("promote: semantic survives persist + reload");
    reset_test_db();

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, NULL) == 0, "init failed");
        ASSERT(session_remember(db, "learned_fact", "important", SESSION_TTL_EPISODIC) == 0,
               "store failed");
        set_kv_access_count(db, "learned_fact", SESSION_PROMOTE_THRESHOLD);
        session_promote(db);
        ASSERT(get_kv_tier(db, "learned_fact") == MEM_SEMANTIC, "should be semantic");
        ASSERT(session_persist(db) == 0, "persist failed");
        FREE_DB(db);
    }

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, NULL) == 0, "init failed");
        const char *v = session_recall(db, "learned_fact");
        ASSERT(v != NULL, "semantic fact should survive reload");
        ASSERT(strcmp(v, "important") == 0, "wrong value");
        ASSERT(get_kv_tier(db, "learned_fact") == MEM_SEMANTIC, "should still be semantic");
        FREE_DB(db);
    }

    PASS();
}

static void test_session_end(void) {
    TEST("session_end: promotes + persists atomically");
    reset_test_db();

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "trading analysis") == 0, "init failed");
        ASSERT(session_remember(db, "edge_factor", "0.42", SESSION_TTL_EPISODIC) == 0,
               "store failed");
        set_kv_access_count(db, "edge_factor", SESSION_PROMOTE_THRESHOLD);
        ASSERT(session_end(db) == 0, "session_end failed");
        FREE_DB(db);
    }

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, NULL) == 0, "init failed");
        const char *v = session_recall(db, "edge_factor");
        ASSERT(v != NULL, "fact should survive session_end + reload");
        ASSERT(strcmp(v, "0.42") == 0, "wrong value");
        ASSERT(get_kv_tier(db, "edge_factor") == MEM_SEMANTIC,
               "should be semantic (promoted by session_end)");
        FREE_DB(db);
    }

    PASS();
}

static void test_session_lookup_similar_basic(void) {
    TEST("lookup_similar finds past sessions by task text");
    reset_test_db();

    /* Build history: two past sessions */
    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "analyze crypto market trends") == 0, "init S1 failed");
        ASSERT(session_remember(db, "btc_trend", "up", 0) == 0, "store failed");
        ASSERT(session_persist(db) == 0, "persist S1 failed");
        FREE_DB(db);
    }

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "analyze stock market data") == 0, "init S2 failed");
        ASSERT(session_persist(db) == 0, "persist S2 failed");
        FREE_DB(db);
    }

    /* New session: query for sessions similar to "analyze crypto" */
    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "new session") == 0, "init S3 failed");

        const session_record_t *results[5];
        int found = session_lookup_similar(db, "analyze crypto", results, 5);
        ASSERT(found > 0, "expected at least one match");

        /* Best match should contain both "analyze" and a known task word */
        bool found_relevant = false;
        for (int i = 0; i < found; i++) {
            if (strstr(results[i]->task_text, "analyze")) {
                found_relevant = true;
                break;
            }
        }
        ASSERT(found_relevant, "expected matching session in results");

        FREE_DB(db);
    }

    PASS();
}

static void test_session_lookup_similar_ranking(void) {
    TEST("lookup_similar returns best match first");
    reset_test_db();

    /* Session A: "crypto analysis deep dive" — 2 of 3 query words match */
    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "crypto analysis deep dive") == 0, "init A failed");
        ASSERT(session_persist(db) == 0, "persist A failed");
        FREE_DB(db);
    }

    /* Session B: "crypto report" — 1 of 3 query words match */
    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "crypto report") == 0, "init B failed");
        ASSERT(session_persist(db) == 0, "persist B failed");
        FREE_DB(db);
    }

    /* Query: "crypto analysis summary" — session A should rank first */
    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "current") == 0, "init failed");

        const session_record_t *results[5];
        int found = session_lookup_similar(db, "crypto analysis summary", results, 5);
        ASSERT(found >= 2, "expected at least 2 matches");
        /* Session with "analysis" in it should rank above "report" */
        ASSERT(strstr(results[0]->task_text, "analysis") != NULL,
               "higher-overlap session should rank first");

        FREE_DB(db);
    }

    PASS();
}

static void test_session_lookup_similar_no_match(void) {
    TEST("lookup_similar returns 0 for completely unrelated query");
    reset_test_db();

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "weather forecast analysis") == 0, "init failed");
        ASSERT(session_persist(db) == 0, "persist failed");
        FREE_DB(db);
    }

    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "new query") == 0, "init failed");
        const session_record_t *results[5];
        int found = session_lookup_similar(db, "xyz123 qwerty999", results, 5);
        ASSERT(found == 0, "expected no match for unrelated nonsense query");
        FREE_DB(db);
    }

    PASS();
}

static void test_lookup_excludes_current_session(void) {
    TEST("lookup_similar excludes the current session");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, "analyze market data") == 0, "init failed");

    const session_record_t *results[5];
    int found = session_lookup_similar(db, "analyze market data", results, 5);
    for (int i = 0; i < found; i++) {
        ASSERT(strcmp(results[i]->id, db->current_session_id) != 0,
               "current session should not appear in lookup results");
    }

    FREE_DB(db);
    PASS();
}

static void test_status_json(void) {
    TEST("session_status_json produces valid output");
    reset_test_db();

    ALLOC_DB(db);
    ASSERT(session_init(db, "status test") == 0, "init failed");
    ASSERT(session_remember(db, "k1", "v1", 0) == 0, "store failed");
    ASSERT(session_remember(db, "k2", "v2", SESSION_TTL_WORKING) == 0, "store failed");

    char buf[512];
    int n = session_status_json(db, buf, sizeof(buf));
    ASSERT(n > 0, "status_json returned empty");
    ASSERT(strstr(buf, "kv_entries") != NULL, "missing kv_entries field");
    ASSERT(strstr(buf, "current_session") != NULL, "missing current_session field");
    ASSERT(strstr(buf, "semantic") != NULL, "missing semantic count");

    FREE_DB(db);
    PASS();
}

/* ── Recall accuracy measurement ─────────────────────────────────────── */

static void test_recall_accuracy(void) {
    TEST("recall accuracy >= 95% across 100 keys (cross-session)");
    reset_test_db();

    /* Store 100 key-value pairs and persist */
    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "accuracy test") == 0, "init failed");

        for (int i = 0; i < 100; i++) {
            char key[64], val[64];
            snprintf(key, sizeof(key), "accuracy_key_%03d", i);
            snprintf(val, sizeof(val), "val_%03d", i);
            ASSERT(session_remember(db, key, val, 0) == 0, "store failed");
        }

        ASSERT(session_persist(db) == 0, "persist failed");
        FREE_DB(db);
    }

    /* Reload and check recall */
    {
        ALLOC_DB(db);
        ASSERT(session_init(db, "accuracy check") == 0, "init failed");

        int recalled = 0;
        for (int i = 0; i < 100; i++) {
            char key[64], expected[64];
            snprintf(key, sizeof(key), "accuracy_key_%03d", i);
            snprintf(expected, sizeof(expected), "val_%03d", i);
            const char *v = session_recall(db, key);
            if (v && strcmp(v, expected) == 0)
                recalled++;
        }

        double accuracy = (double)recalled / 100.0;
        char msg[128];
        snprintf(msg, sizeof(msg), "recall accuracy %.1f%% < 95%% (%d/100 correct)",
                 accuracy * 100.0, recalled);
        ASSERT(accuracy >= 0.95, msg);

        FREE_DB(db);
    }

    PASS();
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    /* Use a temp path so tests don't pollute the real DB */
    if (!getenv("DSCO_SESSION_PATH") || !*getenv("DSCO_SESSION_PATH"))
        setenv("DSCO_SESSION_PATH", "/tmp/dsco_test_sessions.json", 1);

    fprintf(stderr, "\n=== session_memory tests ===\n\n");

    /* Lifecycle */
    test_basic_init_free();
    test_null_task_allowed();

    /* Remember / recall */
    test_remember_and_recall();
    test_update_existing_key();
    test_access_count_increments_on_recall();

    /* Tier inference */
    test_ttl_working_infers_tier();
    test_ttl_episodic_infers_tier();
    test_ttl_zero_infers_semantic();

    /* TTL / eviction */
    test_ttl_expiry_on_recall();
    test_evict_expired();

    /* Cross-session persistence */
    test_cross_session_persistence();
    test_cross_session_episodic_survives();
    test_cross_session_working_expired();
    test_multiple_kv_entries_serialized();
    test_special_chars_in_value();

    /* Promotion */
    test_promotion_working_to_episodic();
    test_promotion_episodic_to_semantic();
    test_promotion_semantic_persists_after_reload();

    /* Session end */
    test_session_end();

    /* Similarity lookup */
    test_session_lookup_similar_basic();
    test_session_lookup_similar_ranking();
    test_session_lookup_similar_no_match();
    test_lookup_excludes_current_session();

    /* Diagnostics */
    test_status_json();

    /* Recall accuracy benchmark */
    test_recall_accuracy();

    /* Cleanup */
    reset_test_db();

    int total = g_run;
    fprintf(stderr, "\n=== Results: %d/%d passed", g_pass, total);
    if (g_fail)
        fprintf(stderr, ", \033[31m%d FAILED\033[0m", g_fail);
    fprintf(stderr, " ===\n\n");

    return g_fail > 0 ? 1 : 0;
}
