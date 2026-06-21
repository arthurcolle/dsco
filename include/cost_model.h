#ifndef DSCO_COST_MODEL_H
#define DSCO_COST_MODEL_H

/* ── Learned per-topology cost model ─────────────────────────────────────
 *
 * Priority 3: Learn from actual execution costs, improve estimates over time.
 *
 * Usage:
 *   cost_model_init();
 *
 *   // After topology_run():
 *   cost_model_learn("fanout_balance", 3200, 1.23, 14.5);
 *
 *   // Before topology_run():
 *   double est = cost_model_predict("fanout_balance", 700, 600);
 *   // Returns -1.0 if <2 observations (fall back to static estimate)
 */

#include <stdbool.h>
#include <stddef.h>

void   cost_model_init(void);
void   cost_model_learn(const char *topology_name, int total_tokens,
                        double actual_cost_usd, double actual_latency_s);
double cost_model_predict(const char *topology_name,
                          int input_tokens, int output_tokens);
double cost_model_predict_latency(const char *topology_name);
int    cost_model_stats_json(char *buf, size_t buflen);
void   cost_model_flush(void);

/* ── Prediction with confidence interval ──────────────────────────────── */

typedef struct {
    double cost_usd;           /* point estimate */
    double cost_lo;            /* 10th-percentile lower bound */
    double cost_hi;            /* 90th-percentile upper bound */
    double latency_s;          /* estimated latency */
    double confidence;         /* 0.0-1.0 (0=no data, 1=well-calibrated) */
    int    observations;
} cost_prediction_t;

bool   cost_model_predict_full(const char *topology_name,
                               int input_tokens, int output_tokens,
                               cost_prediction_t *out);


#endif /* DSCO_COST_MODEL_H */
