/* swarm_cost_model.h — shared cost SUT for the optimizer AND the property harness.
 * Single source of truth: both the demo and the 250K-test harness call these,
 * so a property that passes here is a property of the real model. */
#ifndef SWARM_COST_MODEL_H
#define SWARM_COST_MODEL_H
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

typedef struct {
    const char *name;
    double in_per_m;        /* input $/1M tokens */
    double out_per_m;       /* output $/1M tokens */
    double cache_read_mult; /* cache-read cost as fraction of input price */
    int    capability;      /* 0..100 quality bar */
} scm_model_t;

static const scm_model_t SCM_CATALOG[] = {
    {"fugu",              0.50,  1.50, 0.10, 78},
    {"z-ai/glm-5.2",      0.60,  2.20, 0.10, 80},
    {"claude-sonnet-4-6", 3.00, 15.00, 0.10, 90},
    {"openai/gpt-5.5",    1.25, 10.00, 0.10, 92},
    {"claude-fable-5",    0.80,  4.00, 0.10, 84},
    {"claude-opus-4-6",   5.00, 25.00, 0.10, 95},
    {"moonshot/kimi",     0.60,  2.50, 0.10, 76},
    {"haiku",             0.25,  1.25, 0.10, 60},
};
enum { SCM_NMODELS = (int)(sizeof(SCM_CATALOG)/sizeof(SCM_CATALOG[0])) };

/* cost of one turn; prefix_cached => prefix billed at cache_read_mult × input */
static inline double scm_turn_cost(const scm_model_t *m, long prefix_tok,
                                   long unique_tok, long out_tok, bool prefix_cached) {
    double in_p = m->in_per_m / 1e6, out_p = m->out_per_m / 1e6;
    double prefix = prefix_cached ? prefix_tok * in_p * m->cache_read_mult
                                  : prefix_tok * in_p;
    return prefix + unique_tok * in_p + out_tok * out_p;
}

/* cheapest model clearing the capability bar; NULL if none qualifies */
static inline const scm_model_t *scm_route_cheapest(int min_cap, long prefix,
        long uniq, long out, bool cached) {
    const scm_model_t *best = NULL; double bc = 1e18;
    for (int i = 0; i < SCM_NMODELS; i++) {
        if (SCM_CATALOG[i].capability < min_cap) continue;
        double c = scm_turn_cost(&SCM_CATALOG[i], prefix, uniq, out, cached);
        if (c < bc) { bc = c; best = &SCM_CATALOG[i]; }
    }
    return best;
}

/* EV gate: race N lanes iff expected latency value beats wasted lane spend. */
static inline bool scm_should_race(double p_fail, double latency_value_usd,
        double lane_cost, int lanes) {
    if (lanes <= 1) return false;
    /* value of racing ≈ reduction in expected failure/latency; waste = extra lanes */
    double value = latency_value_usd * (1.0 - p_fail); /* value delivered sooner */
    double waste = (lanes - 1) * lane_cost;
    return value > waste;
}

#endif
