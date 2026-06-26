/* tests/test_plan_cache.c — Advanced unit/property/concurrency tests for plan_cache.
 *
 * Build (from repo root):
 *   cc -Iinclude -Wall -Wextra -o test_plan_cache \
 *      tests/test_plan_cache.c src/plan_cache.c -lpthread -lm
 *
 * Run (all tests):              ./test_plan_cache
 * Filter by name or tag:        ./test_plan_cache adapt jaccard
 * List tests:                   ./test_plan_cache --list
 * CI / TAP output:              ./test_plan_cache --tap
 * Throughput benchmarks:        ./test_plan_cache --bench
 * Flake / order detection:      ./test_plan_cache --repeat 50 --shuffle
 * Demonstrate the guards:       ./test_plan_cache guard --timeout 1
 * Full option list:             ./test_plan_cache --help
 *
 * Sanitizer-friendly:
 *   cc -Iinclude -fsanitize=address,undefined -g -O1 ...
 *
 * Harness features over the original:
 *   - Non-fatal CHECK_* assertions: a test keeps running after a failed check
 *     so one run surfaces *all* problems, not just the first.
 *   - Fatal REQUIRE_* assertions guarded by sigsetjmp: a NULL-deref-prone
 *     precondition aborts only the current test (no crash, no skipped suite).
 *   - Crash isolation: SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT during a test are
 *     caught and recorded as a failure; the rest of the suite keeps running.
 *   - Per-test timeout (SIGALRM): a hung/deadlocked test is aborted and failed
 *     instead of stalling the run (default 30s; --timeout N / --no-timeout).
 *   - Per-test isolation: each test gets its own throwaway $HOME so on-disk
 *     cache files never bleed between tests; teardown removes it.
 *   - Test registry with name/tag filtering, --repeat, --shuffle, and timing.
 *   - TAP v13 output (--tap) and a --bench throughput mode.
 *   - Deterministic xorshift PRNG for reproducible fuzz/property tests.
 */

#include "plan_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ── Harness state ───────────────────────────────────────────────────────── */

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;
static int g_checks_run = 0; /* total CHECK/REQUIRE evaluations */
static int g_checks_failed = 0;

/* Per-test scratch state */
static int g_cur_fail = 0;             /* failed checks in the current test */
static sigjmp_buf g_cur_jmp;           /* REQUIRE / crash / timeout landing pad */
static char g_tmp_home[256];           /* this test's throwaway $HOME */
static const char *g_use_color = NULL; /* "" or ANSI prefix, decided at start */

/* Per-test diagnostic buffer — failures append here, the runner formats them
 * (indented for the pretty printer, "# "-prefixed for TAP). */
static char g_diag[8192];
static size_t g_diag_len = 0;
static void diag_reset(void) {
    g_diag[0] = '\0';
    g_diag_len = 0;
}
static void diag_addf(const char *fmt, ...) {
    if (g_diag_len + 1 >= sizeof(g_diag))
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_diag + g_diag_len, sizeof(g_diag) - g_diag_len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        g_diag_len += (size_t)n;
        if (g_diag_len >= sizeof(g_diag))
            g_diag_len = sizeof(g_diag) - 1;
    }
}

/* Run-mode configuration (set from argv). */
static int g_tap = 0;           /* emit TAP version 13 instead of pretty output */
static int g_timeout_secs = 30; /* per-test wall-clock cap; 0 disables */
static int g_repeat = 1;        /* run the selected suite N times */
static int g_shuffle = 0;       /* randomize test order */
static uint64_t g_shuffle_seed = 0x1234567890abcdefull;

/* Async crash/timeout capture: a fatal signal during a test siglongjmps back
 * into the runner, which records it as a failure and moves on. */
static volatile sig_atomic_t g_signo = 0;

#define C_RED (g_use_color[0] ? "\033[31m" : "")
#define C_GRN (g_use_color[0] ? "\033[32m" : "")
#define C_YEL (g_use_color[0] ? "\033[33m" : "")
#define C_DIM (g_use_color[0] ? "\033[2m" : "")
#define C_RST (g_use_color[0] ? "\033[0m" : "")

/* Record a failed check (non-fatal): append one line to the diag buffer. */
static void fail_check(const char *file, int line, const char *expr, const char *fmt, ...) {
    g_checks_failed++;
    g_cur_fail++;
    /* basename for compactness */
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    diag_addf("%s:%d  %s", base, line, expr);
    if (fmt && fmt[0]) {
        char msg[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        if (msg[0])
            diag_addf("  — %s", msg);
    }
    diag_addf("\n");
}

/* Non-fatal: keep going so one run reveals every broken expectation. */
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        g_checks_run++;                                                                            \
        if (!(cond))                                                                               \
            fail_check(__FILE__, __LINE__, #cond, "" __VA_ARGS__);                                 \
    } while (0)

/* Fatal: abandon this test (unsafe to continue), but never crash the suite. */
#define REQUIRE(cond, ...)                                                                         \
    do {                                                                                           \
        g_checks_run++;                                                                            \
        if (!(cond)) {                                                                             \
            fail_check(__FILE__, __LINE__, #cond, "" __VA_ARGS__);                                 \
            siglongjmp(g_cur_jmp, 1);                                                              \
        }                                                                                          \
    } while (0)

#define CHECK_STR_EQ(a, b, ...)                                                                    \
    do {                                                                                           \
        g_checks_run++;                                                                            \
        const char *_a = (a), *_b = (b);                                                           \
        if (!_a || !_b || strcmp(_a, _b) != 0)                                                     \
            fail_check(__FILE__, __LINE__, #a " == " #b, "got \"%s\" want \"%s\" " __VA_ARGS__,    \
                       _a ? _a : "(null)", _b ? _b : "(null)");                                    \
    } while (0)

#define CHECK_FLOAT_GE(a, b, ...)                                                                  \
    do {                                                                                           \
        g_checks_run++;                                                                            \
        double _a = (a), _b = (b);                                                                 \
        if (_a < _b - 1e-4)                                                                        \
            fail_check(__FILE__, __LINE__, #a " >= " #b, "got %.6f, floor %.6f " __VA_ARGS__, _a,  \
                       _b);                                                                        \
    } while (0)

#define CHECK_FLOAT_LE(a, b, ...)                                                                  \
    do {                                                                                           \
        g_checks_run++;                                                                            \
        double _a = (a), _b = (b);                                                                 \
        if (_a > _b + 1e-4)                                                                        \
            fail_check(__FILE__, __LINE__, #a " <= " #b, "got %.6f, ceil %.6f " __VA_ARGS__, _a,   \
                       _b);                                                                        \
    } while (0)

#define CHECK_FLOAT_EQ(a, b, ...)                                                                  \
    do {                                                                                           \
        g_checks_run++;                                                                            \
        double _a = (a), _b = (b);                                                                 \
        if (fabs(_a - _b) > 1e-4)                                                                  \
            fail_check(__FILE__, __LINE__, #a " ~= " #b, "got %.6f want %.6f " __VA_ARGS__, _a,    \
                       _b);                                                                        \
    } while (0)

/* ── Deterministic PRNG (xorshift64) — reproducible fuzz ─────────────────── */

static uint64_t g_rng;
static void rng_seed(uint64_t s) {
    g_rng = s ? s : 0x9e3779b97f4a7c15ull;
}
static uint32_t rng_u32(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}
/* Random printable token string of [min,max) length into buf. */
static void rng_str(char *buf, int min_len, int max_len) {
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789  ";
    int span = (max_len > min_len) ? (max_len - min_len) : 1;
    int n = min_len + (int)(rng_u32() % (uint32_t)span);
    for (int i = 0; i < n; i++)
        buf[i] = alpha[rng_u32() % (sizeof(alpha) - 1)];
    buf[n] = '\0';
}

/* ── Per-test setup / teardown ───────────────────────────────────────────── */

static int g_home_seq = 0;

static void test_setup(void) {
    snprintf(g_tmp_home, sizeof(g_tmp_home), "/tmp/dsco_pcache_t_%d_%d", (int)getpid(),
             g_home_seq++);
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
    CHECK_FLOAT_GE(plan_similarity_score("analyze MSFT earnings", "analyze MSFT earnings"), 1.0f);
    /* Reflexivity for a handful of arbitrary strings. */
    const char *xs[] = {"x", "abc", "the quick brown fox", "BTC ETH SOL"};
    for (size_t i = 0; i < sizeof(xs) / sizeof(xs[0]); i++)
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
    CHECK_FLOAT_LE(
        plan_similarity_score("analyze MSFT earnings", "deploy kubernetes cluster on AWS"), 0.4f);
}

static void test_similarity_high_overlap(void) {
    float s_similar = plan_similarity_score("analyze MSFT and AAPL earnings for Q1 2026",
                                            "analyze GOOGL and AMZN earnings for Q1 2026");
    float s_unrelated = plan_similarity_score("analyze MSFT and AAPL earnings for Q1 2026",
                                              "deploy kubernetes cluster on AWS us-east-1");
    CHECK_FLOAT_GE(s_similar, 0.40f);
    CHECK(s_similar > s_unrelated, "near-dup %.3f should beat unrelated %.3f", s_similar,
          s_unrelated);
}

static void test_similarity_whitespace_normalization(void) {
    /* Runs of *internal* whitespace (spaces, tabs, newlines) collapse to a
     * single space, so these normalize to the canonical form exactly. */
    CHECK_FLOAT_GE(plan_similarity_score("analyze   MSFT\t\tearnings", "analyze MSFT earnings"),
                   0.99f);
    CHECK_FLOAT_GE(plan_similarity_score("analyze\nMSFT\nearnings", "analyze MSFT earnings"),
                   0.99f);
    /* Leading whitespace is dropped entirely; a single trailing space may
     * survive normalization, so padded strings stay very close but not 1.0. */
    CHECK_FLOAT_GE(plan_similarity_score("  analyze MSFT earnings  ", "analyze MSFT earnings"),
                   0.90f);
}

static void test_similarity_case_insensitive(void) {
    CHECK_FLOAT_GE(plan_similarity_score("Analyze MSFT Earnings", "analyze msft earnings"), 0.99f);
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
        if (g_cur_fail)
            break; /* don't spam 800× if something is broken */
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
    CHECK(!plan_cache_lookup("deploy kubernetes on AWS us-east", &hit), "unrelated task must miss");
}

static void test_lookup_fuzzy_consistency(void) {
    plan_cache_store("analyze MSFT and AAPL quarterly earnings", "mesh_consensus",
                     "compare two stocks", 0.85f);
    plan_cache_result_t hit;
    bool ok = plan_cache_lookup("analyze GOOGL and AMZN quarterly earnings", &hit);
    /* Whatever the verdict, a hit must clear the documented threshold. */
    if (ok)
        CHECK_FLOAT_GE(hit.similarity, (double)PLAN_CACHE_MIN_SIM,
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
    const char *pj = "{\"steps\":[{\"tool\":\"fetch_price\",\"asset\":\"BTC\"}]}";
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
    const char *pj = "{\"k\":\"v\",\"n\":42,\"arr\":[1,2,3]}";
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
    const char *pj = "{\"fetch\":\"MSFT\",\"compare\":\"AAPL\"}";
    plan_cache_store(task, "fanout_balance", "", 0.90f);
    plan_cache_store_json(task, pj);

    const plan_cache_entry_t *e = plan_cache_find_entry(task);
    REQUIRE(e != NULL, "entry not found");

    char *adapted = plan_cache_adapt(e, "analyze GOOGL and AMZN");
    REQUIRE(adapted != NULL, "adapted plan must be non-NULL");
    CHECK(strstr(adapted, "GOOGL") != NULL, "expected GOOGL");
    CHECK(strstr(adapted, "AMZN") != NULL, "expected AMZN");
    CHECK(strstr(adapted, "MSFT") == NULL, "MSFT must be replaced");
    CHECK(strstr(adapted, "AAPL") == NULL, "AAPL must be replaced");
    free(adapted);
}

static void test_adapt_multi_entity_positional(void) {
    const char *task = "buy MSFT then sell AAPL hedge with NVDA";
    const char *pj = "{\"buy\":\"MSFT\",\"sell\":\"AAPL\",\"hedge\":\"NVDA\"}";
    plan_cache_store(task, "specialist_chain", "", 0.9f);
    plan_cache_store_json(task, pj);
    const plan_cache_entry_t *e = plan_cache_find_entry(task);
    REQUIRE(e != NULL, "entry not found");

    char *adapted = plan_cache_adapt(e, "buy GOOGL then sell AMZN hedge with TSLA");
    REQUIRE(adapted != NULL, "non-NULL");
    CHECK(strstr(adapted, "GOOGL") != NULL, "MSFT→GOOGL");
    CHECK(strstr(adapted, "AMZN") != NULL, "AAPL→AMZN");
    CHECK(strstr(adapted, "TSLA") != NULL, "NVDA→TSLA");
    CHECK(strstr(adapted, "MSFT") == NULL && strstr(adapted, "AAPL") == NULL &&
              strstr(adapted, "NVDA") == NULL,
          "all originals replaced");
    free(adapted);
}

static void test_adapt_identical_entity_is_noop(void) {
    const char *task = "analyze MSFT only";
    const char *pj = "{\"x\":\"MSFT\"}";
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
    const char *pj = "{\"assets\":[\"MSFT\",\"AAPL\",\"NVDA\"]}";
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
    CHECK(plan_cache_adapt(e, "analyze SOL price") == NULL, "no plan_json → NULL");
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
    plan_cache_store("brand_new_token_that_triggers_an_eviction_zzz", "new_topo", "", 0.95f);

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
    REQUIRE(plan_cache_lookup("summarize NVDA Q2 earnings", &hit), "must hit after reload");
    CHECK_STR_EQ(hit.topology_name, "fanout_balance");
    CHECK_STR_EQ(hit.rationale, "parallel");
}

/* Persistence must survive JSON metacharacters in task/rationale and plan_json. */
static void test_persist_escaping_round_trip(void) {
    const char *task = "audit \"quoted\" path C:\\data\nrow2\ttab";
    const char *rat = "needs \"escaping\" \\ and \n newline";
    const char *pj = "{\"note\":\"he said \\\"hi\\\"\",\"path\":\"C:\\\\x\"}";
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
    CHECK(strlen(e->task_text) < 256, "task_text must be bounded: len=%zu", strlen(e->task_text));

    plan_cache_result_t hit;
    CHECK(plan_cache_lookup(task, &hit), "exact long task must hit via full hash");
}

static void test_stats_json_format_and_math(void) {
    plan_cache_store("task alpha", "fanout_balance", "", 0.90f);
    plan_cache_store("task bravo", "specialist_chain", "", 0.85f);
    plan_cache_result_t hit;
    plan_cache_lookup("task alpha", &hit); /* +1 hit */
    plan_cache_lookup("task alpha", &hit); /* +1 hit */
    plan_cache_lookup("task bravo", &hit); /* +1 hit */

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
    CHECK(hit2.hits_before >= hits_before_update, "update must keep hit history (had %d, now %d)",
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

#define NTHREADS 8
#define NOPS 400

static void *worker(void *arg) {
    int tid = (int)(intptr_t)arg;
    /* Keep the timeout alarm on the main thread: siglongjmp from a worker into
     * the runner's jmp_buf would be undefined. (Synchronous faults still land
     * on the offending thread, but our code shouldn't fault here.) */
    sigset_t blk;
    sigemptyset(&blk);
    sigaddset(&blk, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &blk, NULL);
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
    if (p)
        entries = atoi(p + 10);
    CHECK(entries >= 0 && entries <= PLAN_CACHE_MAX, "entries within [0,%d], got %d: %s",
          PLAN_CACHE_MAX, entries, buf);
}

/* ── Guard self-tests (tag "manual": skipped unless explicitly filtered) ─── */

/* Deliberately dereferences NULL to prove the crash guard converts a fatal
 * signal into a recorded failure instead of killing the whole runner. */
static void test_guard_crash_demo(void) {
    volatile int *p = NULL;
    CHECK(*p == 0, "unreachable — the NULL read above traps first");
}

/* Spins forever to prove the per-test timeout (SIGALRM) aborts a hang.
 * Run with a short cap, e.g.  ./test_plan_cache --timeout 1 guard  */
static void test_guard_timeout_demo(void) {
    volatile unsigned long x = 0;
    for (;;)
        x++;
}

/* Test descriptor — defined here so the generators below can read their
 * per-case parameter from g_cur_tc->iparam. */
typedef struct {
    const char *name;
    const char *tag;
    void (*fn)(void);
    int iparam;      /* parameter for table-driven/generated cases */
    bool name_owned; /* true if name was strdup'd and must be freed */
} test_case_t;

/* The test currently executing — generated cases read their parameter here. */
static const test_case_t *g_cur_tc = NULL;

/* ── Parameterized / table-driven generators ─────────────────────────────────
 * Each generator is registered hundreds of times with a distinct iparam, so a
 * few functions expand into 600 independent, deterministic test cases. Every
 * case re-seeds from its iparam, so results are reproducible across --repeat
 * and --shuffle. The case's parameter is read from g_cur_tc->iparam. */

/* alnum-only token (no spaces) so generated tasks always yield >=1 3-gram. */
static void rng_token(char *buf, int min_len, int max_len) {
    static const char al[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    int span = (max_len > min_len) ? (max_len - min_len) : 1;
    int n = min_len + (int)(rng_u32() % (uint32_t)span);
    for (int i = 0; i < n; i++)
        buf[i] = al[rng_u32() % (sizeof(al) - 1)];
    buf[n] = '\0';
}

/* Distinct, mutually non-substring uppercase tickers for adapt() cases. */
static const char *TICKERS[] = {
    "MSFT", "AAPL", "GOOGL", "AMZN", "NVDA", "TSLA", "META", "NFLX", "INTC", "ORCL",
    "ADBE", "PYPL", "UBER",  "SHOP", "COIN", "SNOW", "CRWD", "DDOG", "ZS",   "PLTR",
    "ABNB", "ROKU", "TWLO",  "OKTA", "NET",  "PANW", "FTNT", "WDAY", "TEAM", "DASH",
    "RBLX", "HOOD", "SOFI",  "LYFT", "PINS", "SNAP", "SPOT", "DOCU", "ZM",   "MDB",
};
enum { N_TICKERS = (int)(sizeof(TICKERS) / sizeof(TICKERS[0])) };

/* P1 (×200): Jaccard symmetry, [0,1] bounds, and reflexivity on random pairs. */
static void gen_similarity_property(void) {
    int idx = g_cur_tc->iparam;
    rng_seed(0xA5A50000ull ^ ((uint64_t)idx * 2654435761u + 1));
    char a[96], b[96];
    rng_str(a, 0, 80);
    rng_str(b, 0, 80);
    float ab = plan_similarity_score(a, b);
    float ba = plan_similarity_score(b, a);
    CHECK_FLOAT_EQ(ab, ba, "symmetry");
    CHECK_FLOAT_GE(ab, 0.0f, "lower bound");
    CHECK_FLOAT_LE(ab, 1.0f, "upper bound");
    CHECK_FLOAT_GE(plan_similarity_score(a, a), 1.0f, "reflexive");
}

/* P2 (×120): store then exact lookup must hit with the stored topology. */
static void gen_store_lookup(void) {
    int idx = g_cur_tc->iparam;
    rng_seed(0x511E0000ull + (uint64_t)idx);
    char body[64], task[96], topo[32];
    rng_token(body, 12, 40);
    snprintf(task, sizeof(task), "g%d_%s", idx, body);
    snprintf(topo, sizeof(topo), "topo_%d", idx % 7);
    plan_cache_store(task, topo, "gen", 0.80f + (float)(idx % 20) / 100.0f);
    plan_cache_result_t hit;
    REQUIRE(plan_cache_lookup(task, &hit), "exact store must hit (case %d)", idx);
    CHECK_STR_EQ(hit.topology_name, topo);
    CHECK_FLOAT_GE(hit.similarity, 0.99f, "exact hit similarity");
}

/* P3 (×100): adapt() substitutes both entities for a permuted ticker pair. */
static void gen_adapt(void) {
    int idx = g_cur_tc->iparam;
    const char *o1 = TICKERS[(idx + 0) % N_TICKERS];
    const char *o2 = TICKERS[(idx + 1) % N_TICKERS];
    const char *n1 = TICKERS[(idx + 2) % N_TICKERS];
    const char *n2 = TICKERS[(idx + 3) % N_TICKERS];
    char task[64], pj[96], newt[64];
    snprintf(task, sizeof(task), "analyze %s and %s", o1, o2);
    snprintf(pj, sizeof(pj), "{\"buy\":\"%s\",\"sell\":\"%s\"}", o1, o2);
    snprintf(newt, sizeof(newt), "analyze %s and %s", n1, n2);
    plan_cache_store(task, "fanout_balance", "", 0.9f);
    plan_cache_store_json(task, pj);
    const plan_cache_entry_t *e = plan_cache_find_entry(task);
    REQUIRE(e != NULL, "entry must exist (case %d)", idx);
    char *adapted = plan_cache_adapt(e, newt);
    REQUIRE(adapted != NULL, "adapt must produce output");
    CHECK(strstr(adapted, n1) != NULL, "expected %s after substitution", n1);
    CHECK(strstr(adapted, n2) != NULL, "expected %s after substitution", n2);
    CHECK(strcmp(adapted, pj) != 0, "plan must change");
    free(adapted);
}

/* P4 (×60): metadata + plan_json survive save → free → init byte-exact;
 *  a third of cases carry JSON metacharacters to exercise escaping. */
static void gen_persist_roundtrip(void) {
    int idx = g_cur_tc->iparam;
    rng_seed(0x9E3700ull + (uint64_t)idx);
    char body[48], task[96], topo[32], rat[96], pj[96];
    rng_token(body, 10, 30);
    snprintf(task, sizeof(task), "persist%d_%s", idx, body);
    snprintf(topo, sizeof(topo), "topo_%d", idx % 5);
    if (idx % 3 == 0)
        snprintf(rat, sizeof(rat), "needs \"quotes\" \\ and\nnewline %d", idx);
    else
        snprintf(rat, sizeof(rat), "rationale variant %d", idx);
    snprintf(pj, sizeof(pj), "{\"i\":%d,\"q\":\"v\\\"%d\"}", idx, idx);

    plan_cache_store(task, topo, rat, 0.7f);
    plan_cache_store_json(task, pj);
    plan_cache_save();
    plan_cache_free();
    plan_cache_init();

    plan_cache_result_t hit;
    REQUIRE(plan_cache_lookup(task, &hit), "must hit after reload (case %d)", idx);
    CHECK_STR_EQ(hit.topology_name, topo);
    CHECK_STR_EQ(hit.rationale, rat, "rationale byte-exact");
    const plan_cache_entry_t *e = plan_cache_find_entry(task);
    REQUIRE(e != NULL && e->plan_json != NULL, "plan_json must reload");
    CHECK_STR_EQ(e->plan_json, pj, "plan_json byte-exact");
}

/* P5 (×50): whitespace/case mutations stay (near-)identical under similarity. */
static void gen_normalization(void) {
    int idx = g_cur_tc->iparam;
    const char *tk = TICKERS[idx % N_TICKERS];
    char base[96], mut[160];
    snprintf(base, sizeof(base), "analyze %s earnings report case %d", tk, idx);
    /* Build an equivalent string: extra internal spaces + uppercased. */
    size_t w = 0;
    for (const char *p = base; *p && w < sizeof(mut) - 4; p++) {
        char c = *p;
        mut[w++] = (char)toupper((unsigned char)c);
        if (c == ' ' && (idx & 1))
            mut[w++] = ' '; /* widen some gaps */
    }
    mut[w] = '\0';
    /* Internal-only changes (case + collapsible runs) normalize to identical. */
    CHECK_FLOAT_GE(plan_similarity_score(base, mut), 0.99f,
                   "normalized equivalence (internal-only mutation)");
}

/* P6 (×40): stats hit_rate = hits / (entries + hits), computed exactly. */
static void gen_stats_math(void) {
    int idx = g_cur_tc->iparam;
    int E = 1 + (idx % 8);
    int H = idx % 12;
    char task[64];
    for (int i = 0; i < E; i++) {
        snprintf(task, sizeof(task), "stats%d_entry_%d_token", idx, i);
        plan_cache_store(task, "fanout_balance", "", 0.8f);
    }
    snprintf(task, sizeof(task), "stats%d_entry_0_token", idx);
    plan_cache_result_t hit;
    for (int i = 0; i < H; i++)
        plan_cache_lookup(task, &hit);

    char buf[256];
    plan_cache_stats_json(buf, sizeof(buf));
    const char *pe = strstr(buf, "\"entries\":");
    const char *ph = strstr(buf, "\"total_hits\":");
    const char *pr = strstr(buf, "\"hit_rate\":");
    REQUIRE(pe && ph && pr, "stats keys present (case %d)", idx);
    CHECK(atoi(pe + 10) == E, "entries: got %d want %d", atoi(pe + 10), E);
    CHECK(atoi(ph + 13) == H, "total_hits: got %d want %d", atoi(ph + 13), H);
    /* JSON stores hit_rate as %.3f, so allow a 3-decimal rounding margin. */
    double got = atof(pr + 11), want = (double)H / (double)(E + H);
    CHECK(fabs(got - want) < 1e-3, "hit_rate case %d: got %.3f want %.3f", idx, got, want);
}

/* P7 (×30): entry count is exactly min(N, PLAN_CACHE_MAX) after N inserts. */
static void gen_capacity_invariant(void) {
    int idx = g_cur_tc->iparam;
    int N = 1 + idx * 5; /* 1 … 146, crossing the cap at 100 */
    char task[48];
    for (int i = 0; i < N; i++) {
        rng_seed(0xCA9A0000ull + (uint64_t)idx * 1000u + (uint64_t)i);
        rng_token(task, 28, 40); /* dissimilar, distinct tokens */
        plan_cache_store(task, "topo", "", 0.8f);
    }
    int expect = N < PLAN_CACHE_MAX ? N : PLAN_CACHE_MAX;
    char buf[256];
    plan_cache_stats_json(buf, sizeof(buf));
    const char *pe = strstr(buf, "\"entries\":");
    REQUIRE(pe, "entries key present");
    CHECK(atoi(pe + 10) == expect, "N=%d → entries got %d want %d", N, atoi(pe + 10), expect);
}

/* ── Registry & runner ───────────────────────────────────────────────────── */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const test_case_t BASE_TESTS[] = {
    {"similarity: identical → 1.0", "jaccard", test_similarity_identical},
    {"similarity: empty/short → 0.0", "jaccard", test_similarity_empty_and_short},
    {"similarity: unrelated → low", "jaccard", test_similarity_unrelated},
    {"similarity: near-dup beats unrelated", "jaccard", test_similarity_high_overlap},
    {"similarity: whitespace normalization", "jaccard", test_similarity_whitespace_normalization},
    {"similarity: case-insensitive", "jaccard", test_similarity_case_insensitive},
    {"similarity: symmetry & bounds (fuzz)", "property", test_similarity_property_symmetry_bounds},
    {"store+lookup: exact hit", "lookup", test_store_and_lookup_exact},
    {"lookup: unrelated miss", "lookup", test_lookup_miss},
    {"lookup: fuzzy hit respects threshold", "lookup", test_lookup_fuzzy_consistency},
    {"lookup: hit_count increments", "lookup", test_hit_count_increments},
    {"store_json: round-trips in memory", "json", test_store_json_round_trip},
    {"plan_json: lazy reload from disk", "json", test_plan_json_lazy_reload},
    {"adapt: entity substitution", "adapt", test_adapt_entity_substitution},
    {"adapt: multi-entity positional", "adapt", test_adapt_multi_entity_positional},
    {"adapt: identical entity is no-op", "adapt", test_adapt_identical_entity_is_noop},
    {"adapt: fewer entities no crash", "adapt", test_adapt_fewer_entities_no_crash},
    {"adapt: no plan_json → NULL", "adapt", test_adapt_null_plan_json_returns_null},
    {"lru: evicts lowest-priority entry", "lru", test_lru_evicts_lowest_priority},
    {"persist: reload across init/free", "persist", test_persist_and_reload},
    {"persist: JSON-escaping round-trip", "persist", test_persist_escaping_round_trip},
    {"store: long task truncation safe", "persist", test_long_task_truncation},
    {"stats: json format & hit_rate math", "stats", test_stats_json_format_and_math},
    {"store: update preserves identity", "lookup", test_store_update_preserves_identity},
    {"find_entry: returns best candidate", "lookup", test_find_entry_picks_best_candidate},
    {"flush: alias persists like save", "persist", test_flush_alias},
    {"null safety: degenerate inputs", "safety", test_null_safety},
    {"concurrency: no corruption under load", "thread", test_concurrency_no_corruption},
    /* "manual" tests are skipped by default; run via an explicit filter, e.g.
     *   ./test_plan_cache guard --timeout 1   */
    {"guard: crash is caught (NULL deref)", "manual", test_guard_crash_demo},
    {"guard: timeout aborts a hang", "manual", test_guard_timeout_demo},
};
#pragma GCC diagnostic pop
static const int N_BASE = (int)(sizeof(BASE_TESTS) / sizeof(BASE_TESTS[0]));

/* ── Dynamic registry: base tests + 600 generated parameterized cases ────── */

static test_case_t *g_reg = NULL;
static int g_reg_n = 0;
static int g_reg_cap = 0;

static void reg_push(const char *name, const char *tag, void (*fn)(void), int iparam,
                     bool name_owned) {
    if (g_reg_n == g_reg_cap) {
        g_reg_cap = g_reg_cap ? g_reg_cap * 2 : 128;
        g_reg = realloc(g_reg, (size_t)g_reg_cap * sizeof(*g_reg));
    }
    g_reg[g_reg_n].name = name;
    g_reg[g_reg_n].tag = tag;
    g_reg[g_reg_n].fn = fn;
    g_reg[g_reg_n].iparam = iparam;
    g_reg[g_reg_n].name_owned = name_owned;
    g_reg_n++;
}

/* Register `count` cases of one generator with an owned, formatted name. */
static void reg_family(const char *prefix, const char *tag, void (*fn)(void), int count) {
    for (int i = 0; i < count; i++) {
        char nm[96];
        snprintf(nm, sizeof(nm), "%s #%03d", prefix, i);
        reg_push(strdup(nm), tag, fn, i, true);
    }
}

static void build_registry(void) {
    for (int i = 0; i < N_BASE; i++)
        reg_push(BASE_TESTS[i].name, BASE_TESTS[i].tag, BASE_TESTS[i].fn, 0, false);

    /* 600 generated cases across seven property/table-driven families. */
    reg_family("prop-sim", "gen-sim", gen_similarity_property, 200);
    reg_family("gen-store", "gen-store", gen_store_lookup, 120);
    reg_family("gen-adapt", "gen-adapt", gen_adapt, 100);
    reg_family("gen-persist", "gen-persist", gen_persist_roundtrip, 60);
    reg_family("gen-norm", "gen-norm", gen_normalization, 50);
    reg_family("gen-stats", "gen-stats", gen_stats_math, 40);
    reg_family("gen-cap", "gen-cap", gen_capacity_invariant, 30);
}

static void free_registry(void) {
    for (int i = 0; i < g_reg_n; i++)
        if (g_reg[i].name_owned)
            free((char *)g_reg[i].name);
    free(g_reg);
    g_reg = NULL;
    g_reg_n = g_reg_cap = 0;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ── Name/tag filters (parsed from argv, flags excluded) ─────────────────── */

static const char *g_filters[64];
static int g_nfilters = 0;

static bool is_manual(const test_case_t *tc) {
    return tc->tag && strcmp(tc->tag, "manual") == 0;
}

static bool selected(const test_case_t *tc) {
    /* Manual tests never run in the default set — only when a filter names them. */
    if (g_nfilters == 0)
        return !is_manual(tc);
    for (int i = 0; i < g_nfilters; i++)
        if (strstr(tc->name, g_filters[i]) || (tc->tag && strstr(tc->tag, g_filters[i])))
            return true;
    return false;
}

/* ── Crash / timeout guards ──────────────────────────────────────────────── */

static const char *signame(int s) {
    switch (s) {
        case SIGSEGV:
            return "SIGSEGV";
        case SIGBUS:
            return "SIGBUS";
        case SIGFPE:
            return "SIGFPE";
        case SIGILL:
            return "SIGILL";
        case SIGABRT:
            return "SIGABRT";
        case SIGALRM:
            return "SIGALRM";
        default:
            return "signal";
    }
}

static void crash_handler(int sig) {
    g_signo = sig;
    siglongjmp(g_cur_jmp, 2);
}
static void timeout_handler(int sig) {
    (void)sig;
    g_signo = SIGALRM;
    siglongjmp(g_cur_jmp, 3);
}

static void install_guards(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = crash_handler;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sa.sa_handler = timeout_handler;
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);
}

/* ── Order shuffling (Fisher-Yates, local PRNG) ──────────────────────────── */

static void shuffle_order(int *order, int n, uint64_t seed) {
    uint64_t st = seed ? seed : 0x9e3779b97f4a7c15ull;
    for (int i = n - 1; i > 0; i--) {
        st ^= st << 13;
        st ^= st >> 7;
        st ^= st << 17;
        int j = (int)((st >> 11) % (uint64_t)(i + 1));
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
}

/* Print the diagnostic buffer line-by-line with a prefix/colour. */
static void print_diag(FILE *f, const char *line_prefix, const char *col, const char *rst) {
    const char *s = g_diag;
    while (*s) {
        const char *nl = strchr(s, '\n');
        int len = nl ? (int)(nl - s) : (int)strlen(s);
        if (len > 0)
            fprintf(f, "%s%s%.*s%s\n", line_prefix, col, len, s, rst);
        if (!nl)
            break;
        s = nl + 1;
    }
}

/* ── Single-test runner (handles pretty + TAP, crash + timeout) ──────────── */

static void run_one(const test_case_t *tc, int tap_index) {
    g_tests_run++;
    g_cur_fail = 0;
    g_signo = 0;
    g_cur_tc = tc;
    diag_reset();

    if (!g_tap)
        fprintf(stderr, "  %-46s ", tc->name);

    test_setup();
    double t0 = now_ms();
    int jrc = sigsetjmp(g_cur_jmp, 1);
    if (jrc == 0) {
        if (g_timeout_secs > 0)
            alarm((unsigned)g_timeout_secs);
        tc->fn();
        alarm(0);
    } else {
        alarm(0);
        if (jrc == 2) { /* fatal signal */
            g_checks_run++;
            g_checks_failed++;
            g_cur_fail++;
            diag_addf("CRASH: caught %s — test aborted by guard\n", signame(g_signo));
        } else if (jrc == 3) { /* SIGALRM timeout */
            g_checks_run++;
            g_checks_failed++;
            g_cur_fail++;
            diag_addf("TIMEOUT: exceeded %ds — possible hang/deadlock\n", g_timeout_secs);
        }
        /* jrc == 1 (REQUIRE) already recorded its own diagnostic */
    }
    double dt = now_ms() - t0;
    test_teardown();

    bool ok = (g_cur_fail == 0);
    if (ok)
        g_tests_passed++;
    else
        g_tests_failed++;

    if (g_tap) {
        printf("%sok %d - %s # %.1fms\n", ok ? "" : "not ", tap_index, tc->name, dt);
        if (!ok)
            print_diag(stdout, "# ", "", "");
        fflush(stdout);
    } else if (ok) {
        fprintf(stderr, "%sPASS%s %s(%.1fms)%s\n", C_GRN, C_RST, C_DIM, dt, C_RST);
    } else {
        fprintf(stderr, "%sFAIL%s %s(%.1fms)%s\n", C_RED, C_RST, C_DIM, dt, C_RST);
        print_diag(stderr, "      ", C_RED, C_RST);
    }
}

/* ── Benchmark mode ──────────────────────────────────────────────────────── */

static void bench_line(const char *name, long iters, double ms) {
    double per = iters ? (ms * 1e6) / (double)iters : 0.0;     /* ns/op */
    double ops = ms > 0 ? (double)iters / (ms / 1000.0) : 0.0; /* ops/sec */
    fprintf(stderr, "  %-28s %10ld iters  %9.1f ms  %8.0f ns/op  %12.0f ops/s\n", name, iters, ms,
            per, ops);
}

static void run_benchmarks(void) {
    test_setup();
    fprintf(stderr, "\nplan_cache benchmarks  %s(persist-backed; /tmp HOME)%s\n", C_DIM, C_RST);
    fprintf(stderr,
            "────────────────────────────────────────────────────────────────────────────────\n");

    volatile float fsink = 0;
    const char *a = "analyze MSFT and AAPL earnings for Q1 2026";
    const char *b = "analyze GOOGL and AMZN earnings for Q1 2026";

    long N = 200000;
    double t0 = now_ms();
    for (long i = 0; i < N; i++)
        fsink += plan_similarity_score(a, b);
    bench_line("similarity (3-gram jaccard)", N, now_ms() - t0);

    /* miss lookups: no disk write on the miss path */
    plan_cache_store("seed task for benchmark", "fanout_balance", "", 0.9f);
    plan_cache_result_t hit;
    long M = 50000;
    t0 = now_ms();
    char q[64];
    for (long i = 0; i < M; i++) {
        snprintf(q, sizeof(q), "totally unrelated query token %ld zzz", i);
        if (plan_cache_lookup(q, &hit))
            fsink += 1.0f;
    }
    bench_line("lookup miss", M, now_ms() - t0);

    /* hit lookups: each hit bumps stats and re-persists the index */
    long H = 5000;
    t0 = now_ms();
    for (long i = 0; i < H; i++)
        if (plan_cache_lookup("seed task for benchmark", &hit))
            fsink += 1.0f;
    bench_line("lookup hit (+persist)", H, now_ms() - t0);

    /* stores: find-or-alloc + index persist on every call */
    long S = 3000;
    t0 = now_ms();
    char tk[64];
    for (long i = 0; i < S; i++) {
        snprintf(tk, sizeof(tk), "benchmark store task variant %ld", i);
        plan_cache_store(tk, "specialist_chain", "bench", 0.8f);
    }
    bench_line("store (+persist)", S, now_ms() - t0);

    fprintf(stderr,
            "────────────────────────────────────────────────────────────────────────────────\n");
    fprintf(stderr, "  %s(sink=%.0f)%s\n\n", C_DIM, (double)fsink, C_RST);
    test_teardown();
}

/* ── Usage ───────────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [options] [name-or-tag filters...]\n"
            "  --list            list registered tests and exit\n"
            "  --tap             emit TAP version 13 (for CI)\n"
            "  --bench           run throughput benchmarks instead of tests\n"
            "  --repeat N        run the selected suite N times (flake detection)\n"
            "  --shuffle         randomize test order each repetition\n"
            "  --seed=HEX        seed for --shuffle (default fixed/deterministic)\n"
            "  --timeout N       per-test timeout in seconds (default 30)\n"
            "  --no-timeout      disable the per-test timeout\n"
            "  --help            this message\n"
            "Filters match a substring of a test's name or tag; multiple are OR-ed.\n",
            prog);
}

int main(int argc, char **argv) {
    g_use_color = isatty(2) ? "1" : "";

    bool list_only = false, bench = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--list"))
            list_only = true;
        else if (!strcmp(a, "--tap"))
            g_tap = 1;
        else if (!strcmp(a, "--bench"))
            bench = true;
        else if (!strcmp(a, "--shuffle"))
            g_shuffle = 1;
        else if (!strcmp(a, "--no-timeout"))
            g_timeout_secs = 0;
        else if (!strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strncmp(a, "--timeout=", 10))
            g_timeout_secs = atoi(a + 10);
        else if (!strcmp(a, "--timeout") && i + 1 < argc)
            g_timeout_secs = atoi(argv[++i]);
        else if (!strncmp(a, "--repeat=", 9))
            g_repeat = atoi(a + 9);
        else if (!strcmp(a, "--repeat") && i + 1 < argc)
            g_repeat = atoi(argv[++i]);
        else if (!strncmp(a, "--seed=", 7))
            g_shuffle_seed = strtoull(a + 7, NULL, 0);
        else if (a[0] == '-')
            fprintf(stderr, "ignoring unknown flag: %s\n", a);
        else if (g_nfilters < (int)(sizeof(g_filters) / sizeof(g_filters[0])))
            g_filters[g_nfilters++] = a;
    }
    if (g_repeat < 1)
        g_repeat = 1;

    install_guards();
    build_registry();

    if (bench) {
        run_benchmarks();
        free_registry();
        return 0;
    }
    if (list_only) {
        int shown = 0;
        for (int i = 0; i < g_reg_n; i++)
            if (selected(&g_reg[i]))
                shown++;
        fprintf(stderr, "registered tests: %d total, %d selected\n", g_reg_n, shown);
        for (int i = 0; i < g_reg_n; i++)
            if (selected(&g_reg[i]))
                fprintf(stderr, "  %-44s [%s]\n", g_reg[i].name, g_reg[i].tag);
        free_registry();
        return 0;
    }

    /* Build the index of selected tests. */
    int *order = malloc((size_t)g_reg_n * sizeof(int));
    int nsel = 0;
    for (int i = 0; i < g_reg_n; i++)
        if (selected(&g_reg[i]))
            order[nsel++] = i;

    if (g_tap) {
        printf("TAP version 13\n1..%d\n", nsel * g_repeat);
        fflush(stdout);
    } else {
        fprintf(stderr, "\nplan_cache tests%s%s\n", g_nfilters ? " (filtered)" : "",
                g_repeat > 1 ? " ×repeat" : "");
        fprintf(stderr, "────────────────────────────────────────────────────────────\n");
    }

    double t_start = now_ms();
    int tap_index = 1;
    for (int rep = 0; rep < g_repeat; rep++) {
        if (g_shuffle)
            shuffle_order(order, nsel, g_shuffle_seed + (uint64_t)rep);
        if (g_repeat > 1 && !g_tap)
            fprintf(stderr, "%s── pass %d/%d ──%s\n", C_DIM, rep + 1, g_repeat, C_RST);
        for (int k = 0; k < nsel; k++)
            run_one(&g_reg[order[k]], tap_index++);
    }
    double total_ms = now_ms() - t_start;

    FILE *out = g_tap ? stdout : stderr;
    if (!g_tap)
        fprintf(out, "────────────────────────────────────────────────────────────\n");
    fprintf(out, "%s%s%d/%d tests passed%s", g_tap ? "# " : "  ", g_tests_failed ? C_YEL : C_GRN,
            g_tests_passed, g_tests_run, C_RST);
    if (g_tests_failed > 0)
        fprintf(out, "  %s(%d failed)%s", C_RED, g_tests_failed, C_RST);
    fprintf(out, "   %s%d checks, %d failed · %.1fms%s\n\n", C_DIM, g_checks_run, g_checks_failed,
            total_ms, C_RST);

    free(order);
    free_registry();
    return g_tests_failed > 0 ? 1 : 0;
}
