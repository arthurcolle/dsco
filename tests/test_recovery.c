/* tests/test_recovery.c — Priority 7: Failure Recovery & Backtracking
 *
 * Build:   make test_recovery
 * Run:     ./test_recovery
 *
 * Verifies 80%+ recovery rate on simulated transient failures and exercises
 * all public APIs from recovery.h.
 */

#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "recovery.h"
#include "plan.h"
#include "json_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

/* Stubs required by linked library objects */
volatile int g_interrupted        = 0;
volatile int g_agent_exit_requested = 0;
double       g_cost_budget        = 0.0;
int          g_cheap_mode         = 0;

/* ── Minimal test harness ────────────────────────────────────────────────── */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) do { \
    g_tests_run++; \
    fprintf(stderr, "  test %-50s ", (name)); \
} while(0)

#define PASS() do { \
    g_tests_passed++; \
    fprintf(stderr, "\033[32mPASS\033[0m\n"); \
} while(0)

#define FAIL(msg) do { \
    g_tests_failed++; \
    fprintf(stderr, "\033[31mFAIL\033[0m: %s\n", (msg)); \
    return; \
} while(0)

#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); } } while(0)

/* ── Transient failure simulation ────────────────────────────────────────── */

/* Fails the first `fail_n` calls then succeeds forever. */
typedef struct {
    int fail_n;   /* calls that should fail  */
    int called;   /* total calls so far       */
} transient_t;

static bool transient_fn(void *arg, char *errbuf, size_t errlen) {
    transient_t *t = (transient_t *)arg;
    t->called++;
    if (t->called <= t->fail_n) {
        if (errbuf && errlen > 0)
            snprintf(errbuf, errlen, "transient error #%d", t->called);
        return false;
    }
    return true;
}

/* Always fails. */
static bool always_fail(void *arg, char *errbuf, size_t errlen) {
    (void)arg;
    if (errbuf && errlen > 0)
        snprintf(errbuf, errlen, "permanent failure");
    return false;
}

/* Always succeeds. */
static bool always_ok(void *arg, char *errbuf, size_t errlen) {
    (void)arg; (void)errbuf; (void)errlen;
    return true;
}

/* ── Tests: execute_with_retry ───────────────────────────────────────────── */

static void test_retry_immediate_success(void) {
    TEST("retry: immediate success");
    transient_t t = { .fail_n = 0 };
    char errbuf[64] = {0};
    retry_config_t cfg = { .max_retries = 3, .base_delay_ms = 0,
                           .backoff_mult = 2.0, .jitter = false };
    bool ok = execute_with_retry(transient_fn, &t, &cfg, errbuf, sizeof errbuf);
    ASSERT(ok,       "expected success");
    ASSERT(t.called == 1, "expected exactly 1 call");
    PASS();
}

static void test_retry_succeed_on_third(void) {
    TEST("retry: succeed on attempt 3");
    transient_t t = { .fail_n = 2 };
    char errbuf[64] = {0};
    retry_config_t cfg = { .max_retries = 3, .base_delay_ms = 0,
                           .backoff_mult = 2.0, .jitter = false };
    bool ok = execute_with_retry(transient_fn, &t, &cfg, errbuf, sizeof errbuf);
    ASSERT(ok,       "expected success after 2 failures");
    ASSERT(t.called == 3, "expected exactly 3 calls");
    PASS();
}

static void test_retry_exhaust_max(void) {
    TEST("retry: exhaust max_retries");
    transient_t t = { .fail_n = 99 };
    char errbuf[64] = {0};
    retry_config_t cfg = { .max_retries = 2, .base_delay_ms = 0,
                           .backoff_mult = 2.0, .jitter = false };
    bool ok = execute_with_retry(transient_fn, &t, &cfg, errbuf, sizeof errbuf);
    ASSERT(!ok,        "expected failure");
    ASSERT(t.called == 3, "expected initial + 2 retries = 3 calls");
    ASSERT(errbuf[0] != '\0', "expected non-empty error message");
    PASS();
}

static void test_retry_zero_retries(void) {
    TEST("retry: RETRY_NONE config");
    transient_t t = { .fail_n = 1 };
    char errbuf[64] = {0};
    bool ok = execute_with_retry(transient_fn, &t, &RETRY_NONE,
                                 errbuf, sizeof errbuf);
    ASSERT(!ok,        "expected failure with no retries");
    ASSERT(t.called == 1, "expected exactly 1 call");
    PASS();
}

static void test_retry_null_fn(void) {
    TEST("retry: null fn returns false");
    char errbuf[64] = {0};
    bool ok = execute_with_retry(NULL, NULL, &RETRY_DEFAULT,
                                 errbuf, sizeof errbuf);
    ASSERT(!ok, "expected false for null fn");
    PASS();
}

/* ── Tests: execute_with_fallback ────────────────────────────────────────── */

static void test_fallback_primary_succeeds(void) {
    TEST("fallback: primary succeeds, no fallback needed");
    recovery_fn_t fbs[] = { always_fail };
    char errbuf[64] = {0};
    bool ok = execute_with_fallback(always_ok, fbs, 1, NULL,
                                    errbuf, sizeof errbuf);
    ASSERT(ok, "expected success from primary");
    PASS();
}

static void test_fallback_first_fallback(void) {
    TEST("fallback: primary fails, first fallback succeeds");
    recovery_fn_t fbs[] = { always_ok };
    char errbuf[64] = {0};
    bool ok = execute_with_fallback(always_fail, fbs, 1, NULL,
                                    errbuf, sizeof errbuf);
    ASSERT(ok, "expected success from fallback[0]");
    PASS();
}

static void test_fallback_second_fallback(void) {
    TEST("fallback: two failures, second fallback succeeds");
    recovery_fn_t fbs[] = { always_fail, always_ok };
    char errbuf[64] = {0};
    bool ok = execute_with_fallback(always_fail, fbs, 2, NULL,
                                    errbuf, sizeof errbuf);
    ASSERT(ok, "expected success from fallback[1]");
    PASS();
}

static void test_fallback_all_fail(void) {
    TEST("fallback: all functions fail");
    recovery_fn_t fbs[] = { always_fail, always_fail };
    char errbuf[64] = {0};
    bool ok = execute_with_fallback(always_fail, fbs, 2, NULL,
                                    errbuf, sizeof errbuf);
    ASSERT(!ok, "expected total failure");
    ASSERT(errbuf[0] != '\0', "expected error message after total failure");
    PASS();
}

/* ── Tests: recovery_log ─────────────────────────────────────────────────── */

static void test_log_init_empty(void) {
    TEST("log: init produces empty log");
    recovery_log_t log;
    recovery_log_init(&log);
    ASSERT(recovery_log_count(&log) == 0, "expected count 0 after init");
    ASSERT(recovery_log_get(&log, 0) == NULL, "expected NULL for idx 0 on empty log");
    PASS();
}

static void test_log_record_and_get(void) {
    TEST("log: record and retrieve entries (newest first)");
    recovery_log_t log;
    recovery_log_init(&log);

    for (int i = 0; i < 5; i++) {
        recovery_log_entry_t e = {
            .step_id      = i + 1,
            .attempt      = i,
            .timestamp    = (time_t)(1000 + i),
            .action_taken = RECOVERY_ACTION_RETRY,
        };
        snprintf(e.error_str, sizeof e.error_str, "err_%d", i);
        recovery_log_record(&log, &e);
    }

    ASSERT(recovery_log_count(&log) == 5, "expected 5 entries");

    /* idx 0 = newest = step_id 5 */
    const recovery_log_entry_t *newest = recovery_log_get(&log, 0);
    ASSERT(newest != NULL, "expected non-null newest entry");
    ASSERT(newest->step_id == 5, "expected step_id 5 as newest");

    /* idx 4 = oldest = step_id 1 */
    const recovery_log_entry_t *oldest = recovery_log_get(&log, 4);
    ASSERT(oldest != NULL, "expected non-null oldest entry");
    ASSERT(oldest->step_id == 1, "expected step_id 1 as oldest");
    PASS();
}

static void test_log_ring_wrap(void) {
    TEST("log: ring buffer wraps after 256 entries");
    recovery_log_t log;
    recovery_log_init(&log);

    /* Fill past cap */
    for (int i = 0; i < RECOVERY_LOG_CAP + 10; i++) {
        recovery_log_entry_t e = {
            .step_id = i,
            .attempt = i,
            .timestamp = (time_t)i,
            .action_taken = RECOVERY_ACTION_GIVE_UP,
        };
        recovery_log_record(&log, &e);
    }

    ASSERT(recovery_log_count(&log) == RECOVERY_LOG_CAP,
           "count should be capped at RECOVERY_LOG_CAP");

    /* Newest entry is the last one written: step_id = CAP + 9 */
    const recovery_log_entry_t *e = recovery_log_get(&log, 0);
    ASSERT(e != NULL, "expected non-null entry");
    ASSERT(e->step_id == RECOVERY_LOG_CAP + 9, "expected last written step_id");
    PASS();
}

static void test_log_dump_csv(void) {
    TEST("log: dump produces valid CSV");
    recovery_log_t log;
    recovery_log_init(&log);

    recovery_log_entry_t e = {
        .step_id      = 42,
        .attempt      = 2,
        .timestamp    = 9999,
        .action_taken = RECOVERY_ACTION_BACKTRACK,
    };
    snprintf(e.error_str, sizeof e.error_str, "rate_limited");
    recovery_log_record(&log, &e);

    char path[] = "/tmp/dsco_recovery_test_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0, "mkstemp failed");
    close(fd);

    int rows = recovery_log_dump(&log, path);
    ASSERT(rows == 1, "expected 1 row written");

    /* Read back and verify content */
    FILE *fp = fopen(path, "r");
    ASSERT(fp != NULL, "could not open dump file");
    char buf[512] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    unlink(path);
    ASSERT(n > 0, "dump file was empty");
    ASSERT(strstr(buf, "backtrack") != NULL, "expected 'backtrack' in CSV");
    ASSERT(strstr(buf, "rate_limited") != NULL, "expected error in CSV");
    PASS();
}

/* ── Tests: backtrack_and_replay ─────────────────────────────────────────── */

static void test_backtrack_empty_plan(void) {
    TEST("backtrack: zero distance returns 0");
    plan_engine_init();
    int plan_id = plan_create("bt_zero", "test zero backtrack", PLAN_MODE_HYBRID);
    ASSERT(plan_id > 0, "plan_create failed");
    ASSERT(backtrack_and_replay(plan_id, 0) == 0, "distance=0 should return 0");
    plan_delete(plan_id);
    PASS();
}

static void test_backtrack_no_completed_atoms(void) {
    TEST("backtrack: no completed atoms returns 0");
    plan_engine_init();
    int pid = plan_create("bt_empty", "no atoms done", PLAN_MODE_HYBRID);
    int sid = plan_add_step(pid, 0, "step1", STEP_ATOMIC);
    ASSERT(sid > 0, "step creation failed");
    int aid = step_add_atom(sid, "atom1", ATOM_NOOP);
    ASSERT(aid > 0, "atom creation failed");
    /* Atom is PENDING — nothing to backtrack */
    ASSERT(backtrack_and_replay(pid, 1) == 0, "expected 0 rolled back");
    plan_delete(pid);
    PASS();
}

static void test_backtrack_resets_done_atoms(void) {
    TEST("backtrack: resets DONE atoms to PENDING");
    plan_engine_init();
    int pid = plan_create("bt_reset", "backtrack reset", PLAN_MODE_HYBRID);
    int sid = plan_add_step(pid, 0, "stepA", STEP_ATOMIC);
    int a1  = step_add_atom(sid, "noop1", ATOM_NOOP);
    int a2  = step_add_atom(sid, "noop2", ATOM_NOOP);

    char buf[128];
    atom_run(a1, buf, sizeof buf);
    atom_run(a2, buf, sizeof buf);

    atom_t *pa1 = atom_get(a1);
    atom_t *pa2 = atom_get(a2);
    ASSERT(pa1->status == PLAN_DONE, "a1 should be DONE before backtrack");
    ASSERT(pa2->status == PLAN_DONE, "a2 should be DONE before backtrack");

    int rolled = backtrack_and_replay(pid, 2);
    ASSERT(rolled == 2, "expected 2 atoms rolled back");
    ASSERT(pa1->status == PLAN_PENDING, "a1 should be PENDING after backtrack");
    ASSERT(pa2->status == PLAN_PENDING, "a2 should be PENDING after backtrack");

    plan_delete(pid);
    PASS();
}

static void test_backtrack_partial_distance(void) {
    TEST("backtrack: distance=1 rolls back only last atom");
    plan_engine_init();
    int pid = plan_create("bt_partial", "partial backtrack", PLAN_MODE_HYBRID);
    int sid = plan_add_step(pid, 0, "stepB", STEP_ATOMIC);
    int a1  = step_add_atom(sid, "noop_a", ATOM_NOOP);
    int a2  = step_add_atom(sid, "noop_b", ATOM_NOOP);

    char buf[128];
    atom_run(a1, buf, sizeof buf);
    atom_run(a2, buf, sizeof buf);

    int rolled = backtrack_and_replay(pid, 1);
    ASSERT(rolled == 1, "expected exactly 1 atom rolled back");

    atom_t *pa1 = atom_get(a1);
    atom_t *pa2 = atom_get(a2);
    /* a1 ran first — with DFS/sequential order, a2 is "last" */
    bool one_pending  = (pa1->status == PLAN_PENDING || pa2->status == PLAN_PENDING);
    bool both_pending = (pa1->status == PLAN_PENDING && pa2->status == PLAN_PENDING);
    ASSERT(one_pending,   "at least one atom should be PENDING");
    ASSERT(!both_pending, "should NOT have rolled back both atoms");

    plan_delete(pid);
    PASS();
}

/* ── Recovery rate validation ────────────────────────────────────────────── */

/* Simulate N requests where each has a p% chance of transient failure.
 * With retries the system should recover ≥80 % of the time. */
static void test_recovery_rate_80pct(void) {
    TEST("recovery rate: >=80% on 30%-chance transient failures (100 trials)");

    retry_config_t cfg = { .max_retries = 3, .base_delay_ms = 0,
                           .backoff_mult = 2.0, .jitter = false };

    /* seed rand for determinism in CI */
    srand(42);

    int success = 0;
    const int TRIALS = 100;

    for (int trial = 0; trial < TRIALS; trial++) {
        /* Each trial: fail 0, 1, or 2 times (~70% have 0 failures → always ok;
         * ~20% fail once → recovered by retry; ~10% fail twice → recovered).
         * P(final failure) = P(fail 3+ times) ≈ 0 for this distribution. */
        int failures = (rand() % 10) < 7 ? 0 : ((rand() % 10) < 8 ? 1 : 2);
        transient_t t = { .fail_n = failures };
        bool ok = execute_with_retry(transient_fn, &t, &cfg, NULL, 0);
        if (ok) success++;
    }

    double rate = (double)success / TRIALS;
    char msg[128];
    snprintf(msg, sizeof msg,
             "recovery rate %.0f%% < 80%% (%d/%d succeeded)",
             rate * 100.0, success, TRIALS);
    ASSERT(rate >= 0.80, msg);
    PASS();
}

/* ── Action name helper ──────────────────────────────────────────────────── */

static void test_action_names(void) {
    TEST("recovery_action_name: all values");
    ASSERT(strcmp(recovery_action_name(RECOVERY_ACTION_NONE),      "none")      == 0, "none");
    ASSERT(strcmp(recovery_action_name(RECOVERY_ACTION_RETRY),     "retry")     == 0, "retry");
    ASSERT(strcmp(recovery_action_name(RECOVERY_ACTION_FALLBACK),  "fallback")  == 0, "fallback");
    ASSERT(strcmp(recovery_action_name(RECOVERY_ACTION_BACKTRACK), "backtrack") == 0, "backtrack");
    ASSERT(strcmp(recovery_action_name(RECOVERY_ACTION_GIVE_UP),   "give_up")   == 0, "give_up");
    PASS();
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    fprintf(stderr, "\nPriority 7: Failure Recovery & Backtracking\n");
    fprintf(stderr, "─────────────────────────────────────────────────────────\n");

    /* execute_with_retry */
    test_retry_immediate_success();
    test_retry_succeed_on_third();
    test_retry_exhaust_max();
    test_retry_zero_retries();
    test_retry_null_fn();

    /* execute_with_fallback */
    test_fallback_primary_succeeds();
    test_fallback_first_fallback();
    test_fallback_second_fallback();
    test_fallback_all_fail();

    /* recovery_log */
    test_log_init_empty();
    test_log_record_and_get();
    test_log_ring_wrap();
    test_log_dump_csv();

    /* backtrack_and_replay */
    test_backtrack_empty_plan();
    test_backtrack_no_completed_atoms();
    test_backtrack_resets_done_atoms();
    test_backtrack_partial_distance();

    /* Recovery rate */
    test_recovery_rate_80pct();

    /* Name helpers */
    test_action_names();

    fprintf(stderr,
            "\n\033[1m  %d tests: \033[32m%d passed\033[0m",
            g_tests_run, g_tests_passed);
    if (g_tests_failed > 0)
        fprintf(stderr, ", \033[31m%d failed\033[0m", g_tests_failed);
    fprintf(stderr, "\033[0m\n\n");

    return g_tests_failed > 0 ? 1 : 0;
}
