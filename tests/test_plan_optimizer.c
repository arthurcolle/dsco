/* test_plan_optimizer.c — Standalone tests for Priority 1: plan-time optimizer.
 *
 * Build (from repo root):
 *   make test_runner && ./test_runner   # runs as part of main suite
 *
 * Or compile standalone (requires full library objects):
 *   cc -Wall -O0 -g -std=c2y -I include \
 *      $(find build/test -name '*.o' | grep -v 'test\.o') \
 *      tests/test_plan_optimizer.c -o test_plan_optimizer -lm -lcurl -lsqlite3 -ldl
 *   ./test_plan_optimizer
 *
 * Coverage: topology_cost_multiplier, plan_estimate_cost, plan_analyze,
 *           plan_options_best, plan_options_json.
 */

#include "plan_optimizer.h"
#include "topology.h"
#include "cost_model.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Globals required by linked library objects ────────────────────────── */

volatile int g_interrupted   = 0;
vm_t         g_vm            = {0};
double       g_cost_budget   = 0.0;
int          g_cheap_mode    = 0;

/* agent.c defines this; we stub it here since agent.c is excluded from lib */
volatile int g_agent_exit_requested = 0;

/* ── Minimal test harness ──────────────────────────────────────────────── */

static int s_run = 0, s_pass = 0, s_fail = 0;

#define TEST(name) do { s_run++; fprintf(stderr, "  %-50s ", (name)); } while(0)
#define PASS()     do { s_pass++; fprintf(stderr, "\033[32mPASS\033[0m\n"); } while(0)
#define FAIL(msg)  do { s_fail++; fprintf(stderr, "\033[31mFAIL\033[0m: %s\n", (msg)); } while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { FAIL(msg); return; } \
} while(0)

#define ASSERT_DBL_RANGE(val, lo, hi, msg) do { \
    double _v = (val); \
    if (_v < (lo) || _v > (hi)) { \
        char _buf[128]; \
        snprintf(_buf, sizeof(_buf), "%s (got %.6f, want [%.6f, %.6f])", (msg), _v, (double)(lo), (double)(hi)); \
        FAIL(_buf); return; \
    } \
} while(0)

/* ── Helpers ───────────────────────────────────────────────────────────── */

/* Pick any topology we know exists in the 60-entry registry */
static const char *first_registry_name(void) {
    int count = 0;
    const topology_t *reg = topology_registry(&count);
    return (count > 0) ? reg[0].name : NULL;
}

static const char *find_topo_by_category(topo_category_t cat) {
    int count = 0;
    const topology_t *reg = topology_registry(&count);
    for (int i = 0; i < count; i++) {
        if (reg[i].category == cat) return reg[i].name;
    }
    return NULL;
}

/* ── Test cases ─────────────────────────────────────────────────────────── */

/* 1. topology_cost_multiplier: unknown name → 1.0 */
static void test_multiplier_unknown(void) {
    TEST("multiplier: unknown topology → 1.0");
    double m = topology_cost_multiplier("__no_such_topology__");
    ASSERT_DBL_RANGE(m, 0.999, 1.001, "expected exactly 1.0");
    PASS();
}

/* 2. topology_cost_multiplier: NULL → 1.0 */
static void test_multiplier_null(void) {
    TEST("multiplier: NULL → 1.0");
    double m = topology_cost_multiplier(NULL);
    ASSERT_DBL_RANGE(m, 0.999, 1.001, "expected exactly 1.0");
    PASS();
}

/* 3. topology_cost_multiplier: mesh ≥ chain (mesh has highest overhead) */
static void test_multiplier_mesh_gt_chain(void) {
    TEST("multiplier: mesh ≥ chain (overhead ordering)");
    const char *mesh_name  = find_topo_by_category(CAT_MESH);
    const char *chain_name = find_topo_by_category(CAT_CHAIN);
    if (!mesh_name || !chain_name) {
        /* Registry missing category — skip gracefully */
        s_run--;
        fprintf(stderr, "(skipped — category not found)\n");
        return;
    }
    double m_mesh  = topology_cost_multiplier(mesh_name);
    double m_chain = topology_cost_multiplier(chain_name);
    ASSERT(m_mesh >= m_chain, "mesh multiplier should be ≥ chain multiplier");
    PASS();
}

/* 4. topology_cost_multiplier: all real topologies return > 0 */
static void test_multiplier_positive_for_all(void) {
    TEST("multiplier: > 0 for every registered topology");
    int count = 0;
    const topology_t *reg = topology_registry(&count);
    for (int i = 0; i < count; i++) {
        double m = topology_cost_multiplier(reg[i].name);
        if (m <= 0.0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "topology '%s' returned multiplier %.4f", reg[i].name, m);
            FAIL(buf);
            return;
        }
    }
    PASS();
}

/* 5. plan_estimate_cost: returns positive value for any real option */
static void test_estimate_cost_positive(void) {
    TEST("plan_estimate_cost: positive for a real option");
    const char *name = first_registry_name();
    ASSERT(name != NULL, "registry is empty");

    /* Construct a minimal option pointing at the first topology */
    plan_option_t opt = {0};
    opt.topology_name = name;
    opt.est_cost_usd  = 0.01;  /* sentinel fallback value */

    double cost = plan_estimate_cost(&opt);
    ASSERT(cost > 0.0, "expected positive cost estimate");
    PASS();
}

/* 6. plan_estimate_cost: NULL opt → 0.0 */
static void test_estimate_cost_null(void) {
    TEST("plan_estimate_cost: NULL → 0.0");
    double cost = plan_estimate_cost(NULL);
    ASSERT_DBL_RANGE(cost, -1e-9, 1e-9, "expected 0.0 for NULL opt");
    PASS();
}

/* 7. plan_estimate_cost: unknown topology falls back to est_cost_usd */
static void test_estimate_cost_fallback(void) {
    TEST("plan_estimate_cost: unknown topo falls back to est_cost_usd");
    plan_option_t opt = {0};
    opt.topology_name = "__no_such_topology__";
    opt.est_cost_usd  = 3.14159;

    double cost = plan_estimate_cost(&opt);
    /* Should fall back to the stored value when topology is unknown */
    ASSERT_DBL_RANGE(cost, 3.14, 3.15, "expected fallback to est_cost_usd");
    PASS();
}

/* 8. plan_analyze: returns non-NULL for a real task */
static void test_analyze_returns_options(void) {
    TEST("plan_analyze: returns options for a real task");
    plan_options_t *opts = plan_analyze("analyze MSFT and AAPL earnings", 0);
    ASSERT(opts != NULL, "plan_analyze returned NULL");
    ASSERT(opts->count > 0, "zero options returned");
    ASSERT(opts->count <= PLAN_MAX_OPTIONS, "count exceeds PLAN_MAX_OPTIONS");
    plan_options_free(opts);
    PASS();
}

/* 9. plan_analyze: NULL task → NULL (no crash) */
static void test_analyze_null_task(void) {
    TEST("plan_analyze: NULL task → NULL");
    plan_options_t *opts = plan_analyze(NULL, 0);
    ASSERT(opts == NULL, "expected NULL for NULL task");
    PASS();
}

/* 10. plan_analyze: all returned options have valid topology names */
static void test_analyze_options_have_names(void) {
    TEST("plan_analyze: all options have non-empty topology names");
    plan_options_t *opts = plan_analyze("summarize a research paper", 0);
    ASSERT(opts != NULL, "plan_analyze returned NULL");
    for (int i = 0; i < opts->count; i++) {
        ASSERT(opts->options[i].topology_name != NULL,
               "option has NULL topology_name");
        ASSERT(opts->options[i].topology_name[0] != '\0',
               "option has empty topology_name");
        ASSERT(opts->options[i].est_cost_usd >= 0.0,
               "option has negative cost");
        ASSERT(opts->options[i].est_latency_s >= 0.0,
               "option has negative latency");
        ASSERT(opts->options[i].fit_score >= 0.0 &&
               opts->options[i].fit_score <= 1.01,
               "fit_score out of [0, 1]");
    }
    plan_options_free(opts);
    PASS();
}

/* 11. plan_options_best: returns within PLAN_MAX_OPTIONS */
static void test_options_best(void) {
    TEST("plan_options_best: returns first non-over-budget option");
    plan_options_t *opts = plan_analyze("write a poem", 0);
    ASSERT(opts != NULL, "plan_analyze returned NULL");
    const plan_option_t *best = plan_options_best(opts);
    ASSERT(best != NULL, "plan_options_best returned NULL");
    /* With budget=0 (unlimited) nothing is over-budget; best = options[0] */
    ASSERT(best == &opts->options[0], "unlimited budget should pick first option");
    plan_options_free(opts);
    PASS();
}

/* 12. plan_options_json: produces valid-looking JSON */
static void test_options_json(void) {
    TEST("plan_options_json: output contains expected keys");
    plan_options_t *opts = plan_analyze("debug a race condition", 0);
    ASSERT(opts != NULL, "plan_analyze returned NULL");

    char buf[4096];
    int n = plan_options_json(opts, buf, sizeof(buf));
    ASSERT(n > 0, "plan_options_json returned 0");
    ASSERT(strstr(buf, "\"options\"") != NULL, "missing 'options' key");
    ASSERT(strstr(buf, "\"topology\"") != NULL, "missing 'topology' key");
    ASSERT(strstr(buf, "\"cost_usd\"") != NULL, "missing 'cost_usd' key");
    ASSERT(strstr(buf, "\"latency_s\"") != NULL, "missing 'latency_s' key");

    plan_options_free(opts);
    PASS();
}

/* 13. Accuracy target: plan_estimate_cost within 5× of est_cost_usd
 *
 * The roadmap targets ≤20% error vs. actual.  est_cost_usd (static model)
 * and plan_estimate_cost (tier-weighted) use different bases, so they can
 * diverge.  We verify both stay within 5× of each other — a proxy for
 * "same order of magnitude" before real execution data is available.
 */
static void test_estimate_vs_stored_order_of_magnitude(void) {
    TEST("plan_estimate_cost: within 5× of static est_cost_usd");
    plan_options_t *opts = plan_analyze("review a pull request for security issues", 0);
    ASSERT(opts != NULL, "plan_analyze returned NULL");

    int bad = 0;
    for (int i = 0; i < opts->count; i++) {
        const plan_option_t *o = &opts->options[i];
        if (o->est_cost_usd <= 0.0) continue;  /* skip zero-cost entries */
        double refined = plan_estimate_cost(o);
        double ratio   = refined / o->est_cost_usd;
        if (ratio < 0.20 || ratio > 5.0) {
            fprintf(stderr, "\n    [%s] static=%.5f refined=%.5f ratio=%.2f",
                    o->topology_name, o->est_cost_usd, refined, ratio);
            bad++;
        }
    }
    plan_options_free(opts);
    ASSERT(bad == 0, "some topology costs diverged beyond 5× — check tier pricing");
    PASS();
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void) {
    fprintf(stderr, "\n=== test_plan_optimizer ===\n");

    /* Bootstrap registries */
    topology_registry_init();
    cost_model_init();

    test_multiplier_unknown();
    test_multiplier_null();
    test_multiplier_mesh_gt_chain();
    test_multiplier_positive_for_all();
    test_estimate_cost_positive();
    test_estimate_cost_null();
    test_estimate_cost_fallback();
    test_analyze_returns_options();
    test_analyze_null_task();
    test_analyze_options_have_names();
    test_options_best();
    test_options_json();
    test_estimate_vs_stored_order_of_magnitude();

    fprintf(stderr, "\n%d/%d passed", s_pass, s_run);
    if (s_fail) fprintf(stderr, "  (%d FAILED)", s_fail);
    fprintf(stderr, "\n");

    return (s_fail > 0) ? 1 : 0;
}
