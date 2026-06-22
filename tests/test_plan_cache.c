/* tests/test_plan_cache.c — Advanced unit/property/concurrency tests for plan_cache.
 *
 * Build (from repo root):
 *   cc -Iinclude -Wall -Wextra -o test_plan_cache \
 *      tests/test_plan_cache.c src/plan_cache.c -lpthread -lm
 *
 * Run (all tests):
 *   ./test_plan_cache
 * Run a subset (substring match on test name or tag):
 *   ./test_plan_cache adapt jaccard
 * List registered tests without running them:
 *   ./test_plan_cache --list
 *
 * Sanitizer-friendly:
 *   cc -Iinclude -fsanitize=address,undefined -g -O1 ...
 *
 * Harness features over the original:
 *   - Non-fatal CHECK_* assertions: a test keeps running after a failed check
 *     so one run surfaces *all* problems, not just the first.
 *   - Fatal REQUIRE_* assertions guarded by setjmp/longjmp: a NULL-deref-prone
 *     precondition aborts only the current test (no crash, no skipped suite).
 *   - Per-test isolation: each test gets its own throwaway $HOME so on-disk
 *     cache files never bleed between tests; teardown removes it.
 *   - Test registry with name/tag filtering from argv and per-test timing.
 *   - Deterministic xorshift PRNG for reproducible fuzz/property tests.
 */

#include "plan_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ── Harness state ───────────────────────────────────────────────────────── */

static int g_tests_run     = 0;
static int g_tests_passed  = 0;
static int g_tests_failed  = 0;
static int g_checks_run    = 0;   /* total CHECK/REQUIRE evaluations */
static int g_checks_failed = 0;

/* Per-test scratch state */
static int        g_cur_fail  = 0;       /* failed checks in the current test */
static jmp_buf    g_cur_jmp;             /* REQUIRE landing pad */
static char       g_tmp_home[256];       /* this test's throwaway $HOME */
static const char *g_use_color = NULL;   /* "" or ANSI prefix, decided at start */

#define C_RED   (g_use_color[0] ? "\033[31m" : "")
#define C_GRN   (g_use_color[0] ? "\033[32m" : "")
#define C_YEL   (g_use_color[0] ? "\033[33m" : "")
#define C_DIM   (g_use_color[0] ? "\033[2m"  : "")
#define C_RST   (g_use_color[0] ? "\033[0m"  : "")

/* Record a failed check (non-fatal). */
static void fail_check(const char *file, int line, const char *expr,
                       const char *fmt, ...) {
    g_checks_failed++;
    g_cur_fail++;
    fprintf(stderr, "\n    %sFAIL%s %s:%d  %s", C_RED, C_RST, file, line, expr);
    if (fmt && fmt[0]) {
        va_list ap; va_start(ap, fmt);
        fprintf(stderr, "  — ");
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
}

/* Non-fatal: keep going so one run reveals every broken expectation. */
#define CHECK(cond, ...) do {                                       \
    g_checks_run++;                                                 \
    if (!(cond)) fail_check(__FILE__, __LINE__, #cond, "" __VA_ARGS__); \
} while (0)

/* Fatal: abandon this test (unsafe to continue), but never crash the suite. */
#define REQUIRE(cond, ...) do {                                     \
    g_checks_run++;                                                 \
    if (!(cond)) {                                                  \
        fail_check(__FILE__, __LINE__, #cond, "" __VA_ARGS__);      \
        longjmp(g_cur_jmp, 1);                                      \
    }                                                               \
} while (0)

#define CHECK_STR_EQ(a, b, ...) do {                                          \
    g_checks_run++;                                                           \
    const char *_a = (a), *_b = (b);                                         \
    if (!_a || !_b || strcmp(_a, _b) != 0)                                   \
        fail_check(__FILE__, __LINE__, #a " == " #b,                         \
                   "got \"%s\" want \"%s\" " __VA_ARGS__,                    \
                   _a ? _a : "(null)", _b ? _b : "(null)");                  \
} while (0)

#define CHECK_FLOAT_GE(a, b, ...) do {                                       \
    g_checks_run++;                                                          \
    double _a = (a), _b = (b);                                              \
    if (_a < _b - 1e-4)                                                      \
        fail_check(__FILE__, __LINE__, #a " >= " #b,                        \
                   "got %.6f, floor %.6f " __VA_ARGS__, _a, _b);            \
} while (0)

#define CHECK_FLOAT_LE(a, b, ...) do {                                       \
    g_checks_run++;                                                          \
    double _a = (a), _b = (b);                                              \
    if (_a > _b + 1e-4)                                                      \
        fail_check(__FILE__, __LINE__, #a " <= " #b,                        \
                   "got %.6f, ceil %.6f " __VA_ARGS__, _a, _b);             \
} while (0)

#define CHECK_FLOAT_EQ(a, b, ...) do {                                       \
    g_checks_run++;                                                          \
    double _a = (a), _b = (b);                                              \
    if (fabs(_a - _b) > 1e-4)                                                \
        fail_check(__FILE__, __LINE__, #a " ~= " #b,                        \
                   "got %.6f want %.6f " __VA_ARGS__, _a, _b);              \
} while (0)

/* ── Deterministic PRNG (xorshift64) — reproducible fuzz ─────────────────── */

static uint64_t g_rng;
static void rng_seed(uint64_t s) { g_rng = s ? s : 0x9e3779b97f4a7c15ull; }
static uint32_t rng_u32(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}
/* Random printable token string of [min,max) length into buf. */
static void rng_str(char *buf, int min_len, int max_len) {
    static const char alpha[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789  ";
    int span = (max_len > min_len) ? (max_len - min_len) : 1;
    int n = min_len + (int)(rng_u32() % (uint32_t)span);
    for (int i = 0; i < n; i++)
        buf[i] = alpha[rng_u32() % (sizeof(alpha) - 1)];
    buf[n] = '\0';
}

/* ── Per-test setup / teardown ───────────────────────────────────────────── */

static int g_home_seq = 0;

static void test_setup(void) {
    snprintf(g_tmp_home, sizeof(g_tmp_home),
             "/tmp/dsco_pcache_t_%d_%d", (int)getpid(), g_home_seq++);
    mkdir(g_tmp_home, 0700);
    setenv("HOME", g_tmp_home, 1);
    /* Fresh in-memory + on-disk state for every test. */
    plan_cache_free();
    plan_cache_init();
    rng_seed(0xC0FFEEull + (uint64_t)g_home_seq);
}

static void test_teardown(void) {
    plan_cache_free();
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmp_home);
    (void)system(cmd);
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_similarity_identical(void) {
    CHECK_FLOAT_GE(plan_similarity_score("analyze MSFT earnings",
                                         "analyze MSFT earnings"), 1.0f);
    /* Reflexivity for a handful of arbitrary strings. */
    const char *xs[] = { "x", "abc", "the quick brown fox", "BTC ETH SOL" };
    for (size_t i = 0; i < sizeof(xs)/sizeof(xs[0]); i++)
        CHECK_FLOAT_GE(plan_similarity_score(xs[i], xs[i]), 1.0f,
                       "self-similarity must be 1.0 for \"%s\"", xs[i]);
}

static void test_similarity_empty_and_short(void) {
    /* Empty vs non-empty shares no n-grams → 0. */
    CHECK_FLOAT_LE(plan_similarity_score("", "analyze MSFT earnings"), 0.01f);
    CHECK_FLOAT_LE(plan_similarity_score("analyze MSFT earnings", ""), 0.01f);
    /* Identical strings short-circuit to 1.0 *before* n-gram extraction,
     * so this holds even below the 3-gram window. */
    CHECK_FLOAT_GE(plan_similarity_score("ab", "ab"), 1.0f);
    CHECK_FLOAT_GE(plan_similarity_score("", ""), 1.0f);
    /* Distinct sub-trigram strings yield no n-grams on either side → 0. */
    CHECK_FLOAT_LE(plan_similarity_score("ab", "cd"), 0.01f);
}

static void test_similarity_unrelated(void) {
    CHECK_FLOAT_LE(plan_similarity_score("analyze MSFT earnings",
                                         "deploy kubernetes cluster on AWS"), 0.4f);
}

static void test_similarity_high_overlap(void) {
    float s_similar = plan_similarity_score(
        "analyze MSFT and AAPL earnings for Q1 2026",
        "analyze GOOGL and AMZN earnings for Q1 2026");
    float s_unrelated = plan_similarity_score(
        "analyze MSFT and AAPL earnings for Q1 2026",
        "deploy kubernetes cluster on AWS us-east-1");
    CHECK_FLOAT_GE(s_similar, 0.40f);
    CHECK(s_similar > s_unrelated, "near-dup %.3f should beat unrelated %.3f",
          s_similar, s_unrelated);
}

static void test_similarity_whitespace_normalization(void) {
    /* Runs of *internal* whitespace (spaces, tabs, newlines) collapse to a
     * single space, so these normalize to the canonical form exactly. */
    CHECK_FLOAT_GE(plan_similarity_score("analyze   MSFT\t\tearnings",
                                         "analyze MSFT earnings"), 0.99f);
    CHECK_FLOAT_GE(plan_similarity_score("analyze\nMSFT\nearnings",
                                         "analyze MSFT earnings"), 0.99f);
    /* Leading whitespace is dropped entirely; a single trailing space may
     * survive normalization, so padded strings stay very close but not 1.0. */
    CHECK_FLOAT_GE(plan_similarity_score("  analyze MSFT earnings  ",
                                         "analyze MSFT earnings"), 0.90f);
}

static void test_similarity_case_insensitive(void) {
    CHECK_FLOAT_GE(plan_similarity_score("Analyze MSFT Earnings",
                                         "analyze msft earnings"), 0.99f);
}

/* Property: Jaccard is symmetric and bounded in [0,1] for arbitrary inputs. */
static void test_similarity_property_symmetry_bounds(void) {
    char a[128], b[128];
    for (int iter = 0; iter < 800; iter++) {
        rng_str(a, 0, 100);
        rng_str(b, 0, 100);
        float ab = plan_similarity_score(a, b);
        float ba = plan_similarity_score(b, a);
        CHECK_FLOAT_EQ(ab, ba, "symmetry broke on iter %d", iter);
        CHECK_FLOAT_GE(ab, 0.0f, "below 0 on iter %d", iter);
        CHECK_FLOAT_LE(ab, 1.0f, "above 1 on iter %d", iter);
        if (g_cur_fail) break; /* don't spam 800× if something is broken */
    }
}

static void test_store_and_lookup_exact(void) {
    plan_cache_store("analyze MSFT earnings", "fanout_balance", "parallel fetch", 0.90f);
    plan_cache_result_t hit;
    REQUIRE(plan_cache_lookup("analyze MSFT earnings", &hit), "exact lookup must hit");
    CHECK_STR_EQ(hit.topology_name, "fanout_balance");
    CHECK_STR_EQ(hit.rationale, "parallel fetch");
    CHECK_FLOAT_GE(hit.similarity, 0.99f);
}

static void test_lookup_miss(void) {
    plan_cache_store("analyze MSFT earnings", "fanout_balance", "", 0.8f);
    plan_cache_result_t hit;
    CHECK(!plan_cache_lookup("deploy kubernetes on AWS us-east", &hit),
          "unrelated task must miss");
}

static void test_lookup_fuzzy_consistency(void) {
    plan_cache_store("analyze MSFT and AAPL quarterly earnings",
                     "mesh_consensus", "compare two stocks", 0.85f);
    plan_cache_result_t hit;
    bool ok = plan_cache_lookup("analyze GOOGL and AMZN quarterly earnings", &hit);
    /* Whatever the verdict, a hit must clear the documented threshold. */
    if (ok) CHECK_FLOAT_GE(hit.similarity, (double)PLAN_CACHE_MIN_SIM,
                           "a reported hit must be >= MIN_SIM");
}

static void test_hit_count_increments(void) {
    plan_cache_store("summarize Q1 results for MSFT", "specialist_chain", "", 0.88f);
    plan_cache_result_t h1, h2, h3;
    REQUIRE(plan_cache_lookup("summarize Q1 results for MSFT", &h1), "hit 1");
    REQUIRE(plan_cache_lookup("summarize Q1 results for MSFT", &h2), "hit 2");
    REQUIRE(plan_cache_lookup("summarize Q1 results for MSFT", &h3), "hit 3");
    CHECK(h2.hits_before == h1.hits_before + 1, "hits_before should grow by 1");
    CHECK(h3.hits_before == h2.hits_before + 1, "hits_before should grow by 1");
}

static void test_store_json_round_trip(void) {
    const char *task = "analyze BTC volatility";
    const char *pj   = "{\"steps\":[{\"tool\":\"fetch_price\",\"asset\":\"BTC\"}]}";
    plan_cache_store(task, "fanout_balance", "", 0.85f);
    plan_cache_store_json(task, pj);

    const plan_cache_entry_t *e = plan_cache_find_entry(task);
    REQUIRE(e != NULL, "entry must exist");
    REQUIRE(e->plan_json != NULL, "plan_json must be populated");
    CHECK_STR_EQ(e->plan_json, pj);
}

/* plan_json survives free()/init() by lazy-loading from its per-entry file. */
static void test_plan_json_lazy_reload(void) {
    const char *task = "rank top movers in the SP500";
    const char *pj   = "{\"k\":\"v\",\"n\":42,\"arr\":[1,2,3]}";
    plan_cache_store(task, "mesh_consensus", "", 0.9f);
    plan_cache_store_json(task, pj);
    plan_cache_save();
    plan_cache_free();
    plan_cache_init();

    const plan_cache_entry_t *e = plan_cache_find_entry(task);
    REQUIRE(e != NULL, "entry must reload from index");
    REQUIRE(e->plan_json != NULL, "plan_json must lazy-load from disk");
    CHECK_STR_EQ(e->plan_json, pj);
}

static void test_adapt_entity_substitution(void) {
    const char *task = "analyze MSFT and AAPL";
    const char *pj   = "{\"fetch\":\"MSFT\",\"compare\":\"AAPL\"}";
    plan_cache_store(task, "fanout_balance", "", 0.90f);
    plan_cache_store_json(task, pj);

    const plan_cache_entry_t *e = plan_cache_find_entry(task);
    REQUIRE(e != NULL, "entry not found");

    char *adapted = plan_cache_adapt(e, "analyze GOOGL and AMZN");
    REQUIRE(adapted != NULL, "adapted plan must be non-NULL");
    CHECK(strstr(adapted, "GOOGL") != NULL, "expected GOOGL");
    CHECK(strstr(adapted, "AMZN")  != NULL, "expected AMZN");
    CHECK(strstr(adapted, "MSFT") == NULL, "MSFT must be replaced");
    CHECK(strstr(adapted, "AAPL") == NULL, "AAPL must be replaced");
    free(adapted);
}

static void test_adapt_multi_entity_positional(void) {
    const char *task = "buy MSFT then sell AAPL hedge with NVDA";
    const char *pj   = "{\"buy\":\"MSFT\",\"sell\":\"AAPL\",\"hedge\":\"NVDA\"}";
    plan_cache_store(task, "specialist_chain", "", 0.9f);
    plan_cache_store_json(task, pj);
    const plan_cache_entry_t *e = plan_cache_find_entry(task);
    REQUIRE(e != NULL, "entry not found");

    char *adapted = plan_cache_adapt(e, "buy GOOGL then sell AMZN hedge with TSLA");
    REQUIRE(adapted != NULL, "non-NULL");
    CHECK(strstr(adapted, "GOOGL") != NULL, "MSFT→GOOGL");
    CHECK(strstr(adapted, "AMZN")  != NULL, "AAPL→AMZN");
    CHECK(strstr(adapted, "TSLA")  != NULL, "NVDA→TSLA");
    CHECK(strstr(adapted, "MSFT") == NULL && strstr(adapted, "AAPL") == NULL &&
          strstr(adapted, "NVDA") == NULL, "all originals replaced");
    free(adapted);
}

static void test_adapt_identical_entity_is_noop(void) {
    const char *task = "analyze MSFT only";
    const char *pj   = "{\"x\":\"MSFT\"}";
    plan_cache_store(task, "fanout_balance", "", 0.9f);
    plan_cache_store_json(task, pj);
    const plan_cache_entry_t *e = plan_cache_find_entry(task);
    REQUIRE(e != NULL, "entry not found");

    char *adapted = plan_cache_adapt(e, "analyze MSFT only");
    REQUIRE(adapted != NULL, "non-NULL");
    CHECK_STR_EQ(adapted, pj, "same entities → plan unchanged");
    free(adapted);
}

static void test_adapt_fewer_entities_no_crash(void) {
    const char *task = "analyze MSFT AAPL NVDA";
    const char *pj   = "{\"assets\":[\"MSFT\",\"AAPL\",\"NVDA\"]}";
    plan_cache_store(task, "fanout_balance", "", 0.90f);
    plan_cache_store_json(task, pj);
    const plan_cache_entry_t *e = plan_cache_find_entry(task);
    REQUIRE(e != NULL, "entry not found");

    char *adapted = plan_cache_adapt(e, "analyze GOOGL");
    REQUIRE(adapted != NULL, "non-NULL");
    CHECK(strstr(adapted, "GOOGL") != NULL, "first entity substituted");
    /* Only one pair available → AAPL/NVDA remain untouched. */
    CHECK(strstr(adapted, "AAPL") != NULL && strstr(adapted, "NVDA") != NULL,
          "unmapped entities should remain");
    free(adapted);
}

static void test_adapt_null_plan_json_returns_null(void) {
    plan_cache_store("analyze ETH price", "specialist_chain", "", 0.85f);
    const plan_cache_entry_t *e = plan_cache_find_entry("analyze ETH price");
    REQUIRE(e != NULL, "entry not found");
    CHECK(plan_cache_adapt(e, "analyze SOL price") == NULL,
          "no plan_json → NULL");
}

static void test_lru_evicts_lowest_priority(void) {
    /* Use mutually *dissimilar* random tokens so a fuzzy lookup can never
     * cross-match a sibling and mask whether an entry was truly evicted. */
    static char tasks[PLAN_CACHE_MAX][40];
    char topo[40];
    for (int i = 0; i < PLAN_CACHE_MAX; i++) {
        rng_seed(0x5A5A0000ull + (uint64_t)i);
        rng_str(tasks[i], 28, 32);
        snprintf(topo, sizeof(topo), "topo_%d", i);
        plan_cache_store(tasks[i], topo, "", 0.80f);
    }
    /* Touch tasks 1..N-1 so only task 0 keeps hit_count 0 — the tie-break
     * for victim selection favors the fewest hits when timestamps match,
     * and task 0 is also the oldest by last_used. */
    plan_cache_result_t hit;
    for (int i = 1; i < PLAN_CACHE_MAX; i++)
        CHECK(plan_cache_lookup(tasks[i], &hit), "exact token %d must hit", i);

    /* Insert one more distinct token → forces a single eviction. */
    plan_cache_store("brand_new_token_that_triggers_an_eviction_zzz",
                     "new_topo", "", 0.95f);

    char stats[256];
    plan_cache_stats_json(stats, sizeof(stats));
    CHECK(strstr(stats, "\"entries\":100") != NULL, "must stay at capacity: %s", stats);

    /* The never-touched entry 0 is the victim; its token must now miss. */
    CHECK(!plan_cache_lookup(tasks[0], &hit), "untouched LRU victim must be gone");
    /* A touched entry and the newcomer both still resolve. */
    CHECK(plan_cache_lookup(tasks[1], &hit), "touched entry must survive");
    CHECK(plan_cache_lookup("brand_new_token_that_triggers_an_eviction_zzz", &hit),
          "newcomer must be present");
}

static void test_persist_and_reload(void) {
    plan_cache_store("summarize NVDA Q2 earnings", "fanout_balance", "parallel", 0.88f);
    plan_cache_save();
    plan_cache_free();
    plan_cache_init();

    plan_cache_result_t hit;
    REQUIRE(plan_cache_lookup("summarize NVDA Q2 earnings", &hit),
            "must hit after reload");
    CHECK_STR_EQ(hit.topology_name, "fanout_balance");
    CHECK_STR_EQ(hit.rationale, "parallel");
}

/* Persistence must survive JSON metacharacters in task/rationale and plan_json. */
static void test_persist_escaping_round_trip(void) {
    const char *task = "audit \"quoted\" path C:\\data\nrow2\ttab";
    const char *rat  = "needs \"escaping\" \\ and \n newline";
    const char *pj   = "{\"note\":\"he said \\\"hi\\\"\",\"path\":\"C:\\\\x\"}";
    plan_cache_store(task, "mesh_consensus", rat, 0.77f);
    plan_cache_store_json(task, pj);
    plan_cache_save();
    plan_cache_free();
    plan_cache_init();

    plan_cache_result_t hit;
    REQUIRE(plan_cache_lookup(task, &hit), "escaped task must reload & hit");
    CHECK_STR_EQ(hit.topology_name, "mesh_consensus");
    CHECK_STR_EQ(hit.rationale, rat, "rationale must round-trip byte-exact");

    const plan_cache_entry_t *e = plan_cache_find_entry(task);
    REQUIRE(e != NULL && e->plan_json != NULL, "plan_json must reload");
    CHECK_STR_EQ(e->plan_json, pj, "plan_json must round-trip byte-exact");
}

/* Tasks longer than the 255-char task_text buffer must not overflow, and the
 * full task must still resolve via its full-string hash. */
static void test_long_task_truncation(void) {
    char task[600];
    memset(task, 'A', sizeof(task) - 1);
    task[sizeof(task) - 1] = '\0';
    memcpy(task, "find regime breaks ", 19); /* readable prefix */

    plan_cache_store(task, "fanout_balance", "long", 0.9f);
    const plan_cache_entry_t *e = plan_cache_find_entry(task);
    REQUIRE(e != NULL, "long task must store");
    CHECK(strlen(e->task_text) < 256, "task_text must be bounded: len=%zu",
          strlen(e->task_text));

    plan_cache_result_t hit;
    CHECK(plan_cache_lookup(task, &hit), "exact long task must hit via full hash");
}

static void test_stats_json_format_and_math(void) {
    plan_cache_store("task alpha", "fanout_balance", "", 0.90f);
    plan_cache_store("task bravo", "specialist_chain", "", 0.85f);
    plan_cache_result_t hit;
    plan_cache_lookup("task alpha", &hit);  /* +1 hit */
    plan_cache_lookup("task alpha", &hit);  /* +1 hit */
    plan_cache_lookup("task bravo", &hit);  /* +1 hit */

    char buf[512];
    int n = plan_cache_stats_json(buf, sizeof(buf));
    CHECK(n > 0, "non-empty stats");
    CHECK(strstr(buf, "\"entries\":2") != NULL, "two entries: %s", buf);
    CHECK(strstr(buf, "\"total_hits\":3") != NULL, "three hits: %s", buf);
    CHECK(strstr(buf, "\"max\"") != NULL, "missing max");
    CHECK(strstr(buf, "\"threshold\"") != NULL, "missing threshold");
    /* hit_rate = total_hits / (entries + total_hits) = 3 / (2+3) = 0.600 */
    CHECK(strstr(buf, "\"hit_rate\":0.600") != NULL, "hit_rate math: %s", buf);
}

static void test_store_update_preserves_identity(void) {
    plan_cache_store("recurring analytics task", "fanout_balance", "", 0.88f);
    plan_cache_result_t hit;
    plan_cache_lookup("recurring analytics task", &hit);
    plan_cache_lookup("recurring analytics task", &hit);
    int hits_before_update = hit.hits_before + 1; /* count after the 2nd lookup */

    plan_cache_store("recurring analytics task", "specialist_chain", "updated", 0.92f);
    plan_cache_result_t hit2;
    REQUIRE(plan_cache_lookup("recurring analytics task", &hit2), "must still hit");
    CHECK_STR_EQ(hit2.topology_name, "specialist_chain", "topology updated in place");
    CHECK_STR_EQ(hit2.rationale, "updated");
    /* Re-storing the same task must update, not create a duplicate slot... */
    char buf[256];
    plan_cache_stats_json(buf, sizeof(buf));
    CHECK(strstr(buf, "\"entries\":1") != NULL, "no duplicate slot: %s", buf);
    /* ...and must preserve the accumulated hit_count. */
    CHECK(hit2.hits_before >= hits_before_update,
          "update must keep hit history (had %d, now %d)",
          hits_before_update, hit2.hits_before);
}

static void test_find_entry_picks_best_candidate(void) {
    /* Two near-misses and one closer match; find_entry should return the closest. */
    plan_cache_store("analyze MSFT and AAPL earnings report", "topo_a", "a", 0.8f);
    plan_cache_store("analyze MSFT and AAPL earnings summary", "topo_b", "b", 0.8f);
    const char *q = "analyze MSFT and AAPL earnings report";
    const plan_cache_entry_t *e = plan_cache_find_entry(q);
    REQUIRE(e != NULL, "must find a candidate");
    CHECK_STR_EQ(e->topology_name, "topo_a", "should return the exact match");
}

static void test_flush_alias(void) {
    plan_cache_store("flush alias task", "fanout_balance", "", 0.9f);
    plan_cache_flush(); /* alias for save() */
    plan_cache_free();
    plan_cache_init();
    plan_cache_result_t hit;
    CHECK(plan_cache_lookup("flush alias task", &hit), "flush() must persist like save()");
}

static void test_null_safety(void) {
    /* None of these may crash; values are intentionally degenerate. */
    plan_cache_result_t hit;
    CHECK(!plan_cache_lookup(NULL, &hit), "NULL task → miss");
    CHECK(!plan_cache_lookup("task", NULL), "NULL result → false");
    plan_cache_store(NULL, "topo", "", 0.5f);
    plan_cache_store("task", NULL, "", 0.5f);
    plan_cache_store_json(NULL, "{}");
    plan_cache_store_json("task", NULL);
    CHECK(plan_cache_find_entry(NULL) == NULL, "NULL find → NULL");
    CHECK(plan_cache_adapt(NULL, "task") == NULL, "NULL entry → NULL");
    CHECK_FLOAT_LE(plan_similarity_score(NULL, "task"), 0.0f, "NULL sim → 0");
    CHECK_FLOAT_LE(plan_similarity_score("task", NULL), 0.0f, "NULL sim → 0");
}

/* ── Concurrency stress: exercise the internal mutex under contention ────── */

#define NTHREADS   8
#define NOPS       400

static void *worker(void *arg) {
    int tid = (int)(intptr_t)arg;
    char task[96];
    plan_cache_result_t hit;
    for (int i = 0; i < NOPS; i++) {
        snprintf(task, sizeof(task), "thread %d op %d analyze MSFT", tid, i % 50);
        if ((i & 1) == 0)
            plan_cache_store(task, "fanout_balance", "concurrent", 0.8f);
        else
            plan_cache_lookup(task, &hit);
    }
    return NULL;
}

static void test_concurrency_no_corruption(void) {
    pthread_t th[NTHREADS];
    for (int t = 0; t < NTHREADS; t++)
        REQUIRE(pthread_create(&th[t], NULL, worker, (void *)(intptr_t)t) == 0,
                "thread spawn must succeed");
    for (int t = 0; t < NTHREADS; t++)
        pthread_join(th[t], NULL);

    /* Capacity invariant must survive the race; stats must stay parseable. */
    char buf[256];
    int n = plan_cache_stats_json(buf, sizeof(buf));
    CHECK(n > 0, "stats readable after stress");
    int entries = -1;
    const char *p = strstr(buf, "\"entries\":");
    if (p) entries = atoi(p + 10);
    CHECK(entries >= 0 && entries <= PLAN_CACHE_MAX,
          "entries within [0,%d], got %d: %s", PLAN_CACHE_MAX, entries, buf);
}

/* ── Registry & runner ───────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *tag;
    void (*fn)(void);
} test_case_t;

static const test_case_t TESTS[] = {
    { "similarity: identical → 1.0",                "jaccard",  test_similarity_identical },
    { "similarity: empty/short → 0.0",              "jaccard",  test_similarity_empty_and_short },
    { "similarity: unrelated → low",                "jaccard",  test_similarity_unrelated },
    { "similarity: near-dup beats unrelated",       "jaccard",  test_similarity_high_overlap },
    { "similarity: whitespace normalization",       "jaccard",  test_similarity_whitespace_normalization },
    { "similarity: case-insensitive",               "jaccard",  test_similarity_case_insensitive },
    { "similarity: symmetry & bounds (fuzz)",       "property", test_similarity_property_symmetry_bounds },
    { "store+lookup: exact hit",                    "lookup",   test_store_and_lookup_exact },
    { "lookup: unrelated miss",                     "lookup",   test_lookup_miss },
    { "lookup: fuzzy hit respects threshold",       "lookup",   test_lookup_fuzzy_consistency },
    { "lookup: hit_count increments",               "lookup",   test_hit_count_increments },
    { "store_json: round-trips in memory",          "json",     test_store_json_round_trip },
    { "plan_json: lazy reload from disk",           "json",     test_plan_json_lazy_reload },
    { "adapt: entity substitution",                 "adapt",    test_adapt_entity_substitution },
    { "adapt: multi-entity positional",             "adapt",    test_adapt_multi_entity_positional },
    { "adapt: identical entity is no-op",           "adapt",    test_adapt_identical_entity_is_noop },
    { "adapt: fewer entities no crash",             "adapt",    test_adapt_fewer_entities_no_crash },
    { "adapt: no plan_json → NULL",                 "adapt",    test_adapt_null_plan_json_returns_null },
    { "lru: evicts lowest-priority entry",          "lru",      test_lru_evicts_lowest_priority },
    { "persist: reload across init/free",           "persist",  test_persist_and_reload },
    { "persist: JSON-escaping round-trip",          "persist",  test_persist_escaping_round_trip },
    { "store: long task truncation safe",           "persist",  test_long_task_truncation },
    { "stats: json format & hit_rate math",         "stats",    test_stats_json_format_and_math },
    { "store: update preserves identity",           "lookup",   test_store_update_preserves_identity },
    { "find_entry: returns best candidate",         "lookup",   test_find_entry_picks_best_candidate },
    { "flush: alias persists like save",            "persist",  test_flush_alias },
    { "null safety: degenerate inputs",             "safety",   test_null_safety },
    { "concurrency: no corruption under load",      "thread",   test_concurrency_no_corruption },
};
static const int N_TESTS = (int)(sizeof(TESTS) / sizeof(TESTS[0]));

static bool selected(const test_case_t *tc, int argc, char **argv) {
    if (argc <= 1) return true;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue; /* skip flags like --list */
        if (strstr(tc->name, argv[i]) || (tc->tag && strstr(tc->tag, argv[i])))
            return true;
    }
    return false;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    g_use_color = isatty(2) ? "1" : "";

    bool list_only = false;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--list") == 0) list_only = true;

    if (list_only) {
        fprintf(stderr, "registered tests (%d):\n", N_TESTS);
        for (int i = 0; i < N_TESTS; i++)
            fprintf(stderr, "  %-44s [%s]\n", TESTS[i].name, TESTS[i].tag);
        return 0;
    }

    fprintf(stderr, "\nplan_cache tests%s\n", argc > 1 ? " (filtered)" : "");
    fprintf(stderr, "────────────────────────────────────────────────────────────\n");

    double t_start = now_ms();
    for (int i = 0; i < N_TESTS; i++) {
        const test_case_t *tc = &TESTS[i];
        if (!selected(tc, argc, argv)) continue;

        g_tests_run++;
        g_cur_fail = 0;
        fprintf(stderr, "  %-46s ", tc->name);

        test_setup();
        double t0 = now_ms();
        if (setjmp(g_cur_jmp) == 0) {
            tc->fn();
        } /* else: REQUIRE aborted the test; g_cur_fail already > 0 */
        double dt = now_ms() - t0;
        test_teardown();

        if (g_cur_fail == 0) {
            g_tests_passed++;
            fprintf(stderr, "%sPASS%s %s(%.1fms)%s\n", C_GRN, C_RST, C_DIM, dt, C_RST);
        } else {
            g_tests_failed++;
            fprintf(stderr, "\n  %-46s %sFAILED%s (%d check%s) %s(%.1fms)%s\n",
                    tc->name, C_RED, C_RST,
                    g_cur_fail, g_cur_fail == 1 ? "" : "s", C_DIM, dt, C_RST);
        }
    }
    double total_ms = now_ms() - t_start;

    fprintf(stderr, "────────────────────────────────────────────────────────────\n");
    fprintf(stderr, "  %s%d/%d tests passed%s",
            g_tests_failed ? C_YEL : C_GRN, g_tests_passed, g_tests_run, C_RST);
    if (g_tests_failed > 0)
        fprintf(stderr, "  %s(%d failed)%s", C_RED, g_tests_failed, C_RST);
    fprintf(stderr, "   %s%d checks, %d failed · %.1fms%s\n\n",
            C_DIM, g_checks_run, g_checks_failed, total_ms, C_RST);

    return g_tests_failed > 0 ? 1 : 0;
}
