/* Confidence calibration regression tests for the OODA decision engine. */

#include "../include/ooda.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void check_close(double actual, double expected, const char *message) {
    if (fabs(actual - expected) > 1e-9) {
        fprintf(stderr, "FAIL: %s (actual=%.12f expected=%.12f)\n",
                message, actual, expected);
        assert(0);
    }
}

static void record_outcome(ooda_engine_t *engine, double confidence, bool success) {
    assert(ooda_begin(engine) >= 0);
    assert(ooda_observe(engine, "calibration probe", "test", confidence));
    (void)ooda_decide(engine);
    assert(ooda_act_result(engine, success, success ? "success" : "failure"));
    assert(ooda_complete(engine));
}

static void test_calibration_math(void) {
    ooda_engine_t engine;
    ooda_engine_init(&engine);

    record_outcome(&engine, 0.9, true);   /* squared error 0.01 */
    record_outcome(&engine, 0.9, false);  /* squared error 0.81 */

    assert(engine.total_cycles == 2);
    check_close(ooda_success_rate(&engine, 2), 0.5, "success rate");
    check_close(ooda_brier_score(&engine, 2), 0.41, "Brier score");
    check_close(ooda_calibration_gap(&engine, 2), 0.4, "overconfidence gap");
    check_close(engine.brier_score, 0.41, "running Brier score");
    check_close(engine.calibration_gap, 0.4, "running calibration gap");
}

static void test_rolling_window_and_json(void) {
    ooda_engine_t engine;
    ooda_engine_init(&engine);

    record_outcome(&engine, 0.2, true);   /* old event; excluded below */
    record_outcome(&engine, 0.8, true);
    record_outcome(&engine, 0.8, false);

    check_close(ooda_success_rate(&engine, 2), 0.5, "rolling success rate");
    check_close(ooda_brier_score(&engine, 2), 0.34, "rolling Brier score");
    check_close(ooda_calibration_gap(&engine, 2), 0.3, "rolling calibration gap");

    char json[4096];
    assert(ooda_to_json(&engine, json, sizeof(json)) > 0);
    assert(strstr(json, "\"brier_score\":") != NULL);
    assert(strstr(json, "\"calibration_gap\":") != NULL);
    assert(strstr(json, "\"brier_score_10\":") != NULL);
    assert(strstr(json, "\"calibration_gap_10\":") != NULL);
}

int main(void) {
    test_calibration_math();
    test_rolling_window_and_json();
    puts("PASS: OODA confidence calibration");
    return 0;
}
