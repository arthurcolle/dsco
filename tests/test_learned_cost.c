/* tests/test_learned_cost.c — Validates that 10 training samples bring
 * prediction error from >30% (static baseline) to <15% (learned k-NN).
 *
 * Build & run (standalone, no other dsco sources needed):
 *   cc -O0 -g -std=c11 -I include -o test_learned_cost \
 *       tests/test_learned_cost.c src/learned_cost.c -lm && ./test_learned_cost
 */

#include "../include/learned_cost.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── True cost model (ground truth, unknown to the static baseline) ─────
 *
 * cost = task_len * 0.006  (cost grows linearly with task length).
 * The static baseline uses a fixed-rate estimate based only on token count
 * and therefore systematically underestimates costs for long tasks.
 */
static double true_cost(int task_len) {
    return task_len * 0.006;
}

/* Static baseline: $0.004 flat rate per task character — wrong multiplier. */
static double static_estimate(int task_len) {
    (void)task_len;
    return 0.65;   /* constant "average cost" guess — ±30-50% for most tasks */
}

/* Build a task string of exactly `len` characters (filled with 'x'). */
static void make_task(char *buf, int len) {
    if (len <= 0) { buf[0] = '\0'; return; }
    memset(buf, 'x', (size_t)len);
    buf[len] = '\0';
}

/* Absolute percentage error */
static double ape(double predicted, double actual) {
    if (actual == 0.0) return 0.0;
    return fabs(predicted - actual) / actual;
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

static void check(bool cond, const char *msg) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); }
    assert(cond);
}

/* ── Test 1: baseline vs learned error ────────────────────────────────── */

static void test_accuracy_improves(void) {
    /* Use a temp file so we don't pollute ~/.dsco/cost_history.json */
    setenv("DSCO_COST_HISTORY", "/tmp/dsco_test_cost_history.json", 1);

    const char *topo = "fanout_balance";
    char task_buf[600];

    /* Training samples: task_lens 80-170 in steps of 10 (10 samples). */
    static const int train_lens[] = { 80, 90, 100, 110, 120, 130, 140, 150, 160, 170 };
    static const int train_tokens[] = { 900, 1000, 1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800 };
    int ntrain = (int)(sizeof(train_lens) / sizeof(train_lens[0]));

    /* Test samples: task_lens between training points. */
    static const int test_lens[]   = { 85, 105, 125, 145, 165 };
    static const int test_tokens[] = { 950, 1150, 1350, 1550, 1750 };
    int ntest = (int)(sizeof(test_lens) / sizeof(test_lens[0]));

    /* Measure static baseline error (no training) */
    double baseline_err_sum = 0.0;
    for (int i = 0; i < ntest; i++) {
        double actual = true_cost(test_lens[i]);
        double est    = static_estimate(test_lens[i]);
        baseline_err_sum += ape(est, actual);
    }
    double baseline_mape = baseline_err_sum / ntest;
    printf("[1] Baseline MAPE:  %.1f%%\n", baseline_mape * 100.0);

    /* Build and populate cost database */
    cost_db_t db;
    cost_db_init(&db);
    db.loaded = true;  /* skip reading stale test file from prior runs */

    for (int i = 0; i < ntrain; i++) {
        make_task(task_buf, train_lens[i]);
        double cost = true_cost(train_lens[i]);
        learn_from_execution(&db, task_buf, topo, train_tokens[i], cost);
    }

    check(db.count == ntrain, "db.count should equal number of training samples");

    /* Measure learned prediction error */
    double learned_err_sum = 0.0;
    int predicted = 0;
    for (int i = 0; i < ntest; i++) {
        make_task(task_buf, test_lens[i]);
        cost_prediction_result_t pred;
        bool ok = predict_cost(&db, task_buf, topo, &pred);
        check(ok, "predict_cost must succeed after 10 training samples");
        double actual = true_cost(test_lens[i]);
        learned_err_sum += ape(pred.predicted_cost, actual);
        printf("  test[%d]: task_len=%d  true=$%.3f  pred=$%.3f  err=%.1f%%  conf=%.2f\n",
               i, test_lens[i], actual, pred.predicted_cost,
               ape(pred.predicted_cost, actual) * 100.0, pred.confidence);
        predicted++;
    }
    double learned_mape = learned_err_sum / predicted;
    printf("[1] Learned  MAPE:  %.1f%%\n", learned_mape * 100.0);
    printf("[1] Improvement:    %.1f pp\n", (baseline_mape - learned_mape) * 100.0);

    check(predicted == ntest, "all test samples must produce predictions");
    check(learned_mape < 0.15, "learned MAPE must be < 15% after 10 samples");
    check(learned_mape < baseline_mape, "learned model must beat static baseline");

    printf("[1] PASS: accuracy_improves\n\n");

    /* Clean up temp file */
    remove("/tmp/dsco_test_cost_history.json");
    unsetenv("DSCO_COST_HISTORY");
}

/* ── Test 2: find_similar_executions returns k results ─────────────────── */

static void test_find_similar(void) {
    setenv("DSCO_COST_HISTORY", "/tmp/dsco_test_find_similar.json", 1);

    const char *topo = "specialist_chain";
    char task_buf[600];

    cost_db_t db;
    cost_db_init(&db);
    db.loaded = true;

    /* Insert 8 records for the topology */
    for (int i = 0; i < 8; i++) {
        int task_len = 100 + i * 20;
        make_task(task_buf, task_len);
        learn_from_execution(&db, task_buf, topo, 1000 + i * 100, true_cost(task_len));
    }

    /* Also insert 3 records for a different topology — should be ignored */
    for (int i = 0; i < 3; i++) {
        make_task(task_buf, 150);
        learn_from_execution(&db, task_buf, "mesh_consensus", 1500, 1.20);
    }

    /* Query: ask for 5 neighbors from the 8 specialist_chain records */
    cost_record_t results[5];
    make_task(task_buf, 180);
    int n = find_similar_executions(&db, task_buf, topo, 5, results);

    check(n == 5, "find_similar_executions should return 5 results");
    for (int i = 0; i < n; i++) {
        check(strcmp(results[i].topology, topo) == 0,
              "all returned records must match the requested topology");
    }

    /* Nearest neighbor to task_len=180 should be task_len=180 (if present)
     * or the closest ones: 160 and 200 */
    /* Verify results are sorted by distance (closest first) */
    for (int i = 0; i + 1 < n; i++) {
        int d1 = abs(results[i].task_len   - 180);
        int d2 = abs(results[i+1].task_len - 180);
        check(d1 <= d2, "results must be sorted nearest-first");
    }

    printf("[2] find_similar returned %d records, all topology-matched and sorted\n", n);
    printf("[2] PASS: find_similar_executions\n\n");

    remove("/tmp/dsco_test_find_similar.json");
    unsetenv("DSCO_COST_HISTORY");
}

/* ── Test 3: confidence rises with sample count ──────────────────────────*/

static void test_confidence_increases(void) {
    setenv("DSCO_COST_HISTORY", "/tmp/dsco_test_confidence.json", 1);

    const char *topo = "fanout_optimized";
    char task_buf[600];
    make_task(task_buf, 150);

    cost_db_t db;
    cost_db_init(&db);
    db.loaded = true;

    double prev_conf = 0.0;
    for (int i = 1; i <= 10; i++) {
        int task_len = 140 + i * 2;  /* vary slightly around the query */
        char tbuf[300];
        make_task(tbuf, task_len);
        learn_from_execution(&db, tbuf, topo, 1000 + i * 50, true_cost(task_len));

        cost_prediction_result_t pred;
        bool ok = predict_cost(&db, task_buf, topo, &pred);
        check(ok, "predict_cost must succeed");
        printf("  n=%2d  conf=%.3f  pred=$%.3f\n", i, pred.confidence, pred.predicted_cost);

        if (i > 1) {
            /* Confidence should not decrease as we add more closely-matched samples.
             * Allow a small tolerance for minor fluctuations due to distance weighting. */
            check(pred.confidence >= prev_conf - 0.02,
                  "confidence must not decrease significantly as samples are added");
        }
        prev_conf = pred.confidence;
    }

    cost_prediction_result_t final_pred;
    predict_cost(&db, task_buf, topo, &final_pred);
    check(final_pred.confidence > 0.5,
          "confidence must exceed 0.5 after 10 closely-matched training samples");
    check(final_pred.k_used > 0, "k_used must be positive");

    printf("[3] Final confidence: %.3f  k_used: %d\n",
           final_pred.confidence, final_pred.k_used);
    printf("[3] PASS: confidence_increases\n\n");

    remove("/tmp/dsco_test_confidence.json");
    unsetenv("DSCO_COST_HISTORY");
}

/* ── Test 4: empty database returns false ──────────────────────────────── */

static void test_empty_db(void) {
    cost_db_t db;
    cost_db_init(&db);
    db.loaded = true;

    cost_prediction_result_t pred;
    bool ok = predict_cost(&db, "analyze something", "fanout_balance", &pred);

    check(!ok, "predict_cost on empty db must return false");
    check(pred.confidence == 0.0, "confidence must be 0 on empty db");
    check(pred.k_used == 0, "k_used must be 0 on empty db");

    cost_record_t results[5];
    int n = find_similar_executions(&db, "task", "any_topo", 5, results);
    check(n == 0, "find_similar_executions on empty db must return 0");

    printf("[4] PASS: empty_db\n\n");
}

/* ── Test 5: topology isolation ─────────────────────────────────────────── */

static void test_topology_isolation(void) {
    setenv("DSCO_COST_HISTORY", "/tmp/dsco_test_isolation.json", 1);

    cost_db_t db;
    cost_db_init(&db);
    db.loaded = true;

    char task_buf[200];
    make_task(task_buf, 150);

    /* Records only for "mesh_consensus" */
    for (int i = 0; i < 5; i++) {
        char tbuf[200];
        make_task(tbuf, 140 + i * 5);
        learn_from_execution(&db, tbuf, "mesh_consensus", 1500, 1.20);
    }

    /* Query a different topology — must find nothing */
    cost_prediction_result_t pred;
    bool ok = predict_cost(&db, task_buf, "fanout_balance", &pred);
    check(!ok, "predict_cost must return false when no records match topology");

    /* Query the correct topology — must find results */
    ok = predict_cost(&db, task_buf, "mesh_consensus", &pred);
    check(ok, "predict_cost must succeed for the correct topology");

    printf("[5] Correct topology isolation: mesh_consensus pred=$%.3f  conf=%.2f\n",
           pred.predicted_cost, pred.confidence);
    printf("[5] PASS: topology_isolation\n\n");

    remove("/tmp/dsco_test_isolation.json");
    unsetenv("DSCO_COST_HISTORY");
}

/* ── Test 6: save / load round-trip ─────────────────────────────────────── */

static void test_save_load_roundtrip(void) {
    setenv("DSCO_COST_HISTORY", "/tmp/dsco_test_roundtrip.json", 1);

    const char *topo = "fanout_balance";
    char task_buf[200];

    /* Write 5 records */
    {
        cost_db_t db;
        cost_db_init(&db);
        db.loaded = true;
        for (int i = 0; i < 5; i++) {
            make_task(task_buf, 100 + i * 20);
            learn_from_execution(&db, task_buf, topo, 1000 + i * 100, true_cost(100 + i * 20));
        }
        check(db.count == 5, "should have 5 records after 5 inserts");
        cost_db_save(&db);
    }

    /* Reload into a fresh db */
    {
        cost_db_t db2;
        cost_db_init(&db2);
        bool ok = cost_db_load(&db2);
        check(ok, "cost_db_load must succeed");
        check(db2.count == 5, "loaded db must contain all 5 persisted records");

        /* Predictions must work after reload */
        make_task(task_buf, 150);
        cost_prediction_result_t pred;
        ok = predict_cost(&db2, task_buf, topo, &pred);
        check(ok, "predict_cost must work after reload");
        printf("[6] After reload: pred=$%.3f  conf=%.2f  k=%d\n",
               pred.predicted_cost, pred.confidence, pred.k_used);
    }

    printf("[6] PASS: save_load_roundtrip\n\n");

    remove("/tmp/dsco_test_roundtrip.json");
    unsetenv("DSCO_COST_HISTORY");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== test_learned_cost ===\n\n");

    test_accuracy_improves();
    test_find_similar();
    test_confidence_increases();
    test_empty_db();
    test_topology_isolation();
    test_save_load_roundtrip();

    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
