/* plan_optimizer.c — Plan-time topology selection with cost/latency preview.
 *
 * Implements Priority 1 from the roadmap:
 *   plan_options_t *plan_analyze(const char *task, int budget_cents)
 *
 * Returns a ranked list of topology options with:
 *   - estimated cost (USD)
 *   - estimated latency (seconds)
 *   - fit score (0-1)
 *   - rationale string
 *
 * Integrates with topology.c's task_profile + topology_estimate_cost.
 */

#include "plan_optimizer.h"
#include "topology.h"
#include "task_profile.h"
#include "cost_model.h"
#include "json_util.h"
#include "audit_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define MAX_OPTIONS 8

/* Representative input token count for estimation (~512 words) */
#define EST_INPUT_TOKENS  700
#define EST_OUTPUT_TOKENS 600

/* ── Internal: score a single topology against a task profile ─────────── */

static double score_topology(const topology_t *t, const task_profile_t *tp) {
    double score = 0.5; /* base */

    /* Parallelism alignment */
    if (tp->parallelism_score > 0.6) {
        if (t->category == CAT_FANOUT || t->category == CAT_MESH)  score += 0.25;
        if (t->strategy == EXEC_FULL_PARALLEL)                      score += 0.10;
    } else {
        if (t->category == CAT_CHAIN || t->category == CAT_SPECIALIST) score += 0.20;
    }

    /* Convergence / review alignment */
    if (tp->convergence_score > 0.5) {
        if (t->category == CAT_FEEDBACK || t->category == CAT_COMPETITIVE) score += 0.20;
        if (t->strategy == EXEC_ITERATIVE || t->strategy == EXEC_CONSENSUS) score += 0.10;
    }

    /* Complexity alignment */
    if (tp->complexity_score > 0.7) {
        if (t->category == CAT_HIERARCHY) score += 0.15;
        if (t->total_agents > 6)          score += 0.10;
    } else if (tp->complexity_score < 0.3) {
        if (t->total_agents <= 3)         score += 0.15;
    }

    /* Latency sensitivity */
    if (tp->latency_score > 0.5) {
        score -= t->est_latency_mult * 0.08;
    }

    /* Cap */
    if (score > 1.0) score = 1.0;
    if (score < 0.0) score = 0.0;
    return score;
}

/* ── Public API ───────────────────────────────────────────────────────── */

plan_options_t *plan_analyze(const char *task, int budget_cents) {
    if (!task || !task[0]) return NULL;

    plan_options_t *opts = calloc(1, sizeof(plan_options_t));
    if (!opts) return NULL;

    opts->task = strdup(task);
    opts->budget_cents = budget_cents;
    opts->count = 0;

    /* Build task profile */
    task_profile_t *tp = task_profile(task, NULL);

    /* Walk all 60 registered topologies, rank them */
    int topo_count = 0;
    const topology_t *reg = topology_registry(&topo_count);

    /* Score all, collect top MAX_OPTIONS */
    typedef struct { double score; int idx; } scored_t;
    scored_t scored[TOPOLOGY_COUNT];
    for (int i = 0; i < topo_count && i < TOPOLOGY_COUNT; i++) {
        scored[i].idx = i;
        scored[i].score = tp ? score_topology(&reg[i], tp) : 0.5;
    }

    /* Sort descending by score (simple insertion sort — 60 items) */
    for (int i = 1; i < topo_count && i < TOPOLOGY_COUNT; i++) {
        scored_t key = scored[i];
        int j = i - 1;
        while (j >= 0 && scored[j].score < key.score) {
            scored[j+1] = scored[j];
            j--;
        }
        scored[j+1] = key;
    }

    int n = (topo_count < MAX_OPTIONS) ? topo_count : MAX_OPTIONS;
    for (int i = 0; i < n; i++) {
        const topology_t *t = &reg[scored[i].idx];
        plan_option_t *o = &opts->options[i];

        o->topology_name = strdup(t->name);
        o->fit_score     = scored[i].score;

        /* Cost estimate: prefer learned model, fall back to static */
        double learned = cost_model_predict(t->name, EST_INPUT_TOKENS, EST_OUTPUT_TOKENS);
        if (learned > 0.0) {
            o->est_cost_usd = learned;
            o->cost_source  = COST_SOURCE_LEARNED;
        } else {
            o->est_cost_usd = topology_estimate_cost(t, EST_INPUT_TOKENS, EST_OUTPUT_TOKENS);
            o->cost_source  = COST_SOURCE_STATIC;
        }

        /* Latency estimate: latency_mult * base_latency (assume ~8s per serial hop) */
        o->est_latency_s = t->est_latency_mult * 8.0;

        /* Over budget? */
        o->over_budget = (budget_cents > 0 &&
                          (int)(o->est_cost_usd * 100.0) > budget_cents);

        /* Rationale */
        snprintf(o->rationale, sizeof(o->rationale),
                 "fit=%.0f%% agents=%d cat=%s lat=%.1fs cost=$%.4f%s",
                 o->fit_score * 100.0,
                 t->total_agents,
                 topo_category_label(t->category),
                 o->est_latency_s,
                 o->est_cost_usd,
                 o->over_budget ? " [OVER BUDGET]" : "");

        opts->count++;
    }

    if (tp) task_profile_free(tp);
    return opts;
}

void plan_options_free(plan_options_t *opts) {
    if (!opts) return;
    free((char*)opts->task);
    for (int i = 0; i < opts->count; i++) {
        free((char*)opts->options[i].topology_name);
    }
    free(opts);
}

/* Render options as compact JSON */
int plan_options_json(const plan_options_t *opts, char *buf, size_t buflen) {
    if (!opts || !buf || buflen < 8) return 0;

    char *p = buf;
    size_t rem = buflen;
    int n;

    n = snprintf(p, rem, "{\"task\":\"%s\",\"budget_cents\":%d,\"options\":[",
                 opts->task ? opts->task : "", opts->budget_cents);
    p += n; rem -= (size_t)n;

    for (int i = 0; i < opts->count && rem > 8; i++) {
        const plan_option_t *o = &opts->options[i];
        n = snprintf(p, rem,
            "%s{\"topology\":\"%s\",\"fit\":%.3f,\"cost_usd\":%.5f,"
            "\"latency_s\":%.1f,\"over_budget\":%s,"
            "\"cost_source\":\"%s\",\"rationale\":\"%s\"}",
            i ? "," : "",
            o->topology_name ? o->topology_name : "",
            o->fit_score, o->est_cost_usd, o->est_latency_s,
            o->over_budget ? "true" : "false",
            o->cost_source == COST_SOURCE_LEARNED ? "learned" : "static",
            o->rationale);
        p += n; rem -= (size_t)n;
    }
    snprintf(p, rem, "]}");
    return (int)(buflen - rem);
}

/* Return the best non-over-budget option (or best overall if all over) */
const plan_option_t *plan_options_best(const plan_options_t *opts) {
    if (!opts || opts->count == 0) return NULL;
    for (int i = 0; i < opts->count; i++) {
        if (!opts->options[i].over_budget) return &opts->options[i];
    }
    return &opts->options[0]; /* all over budget — return top fit anyway */
}
