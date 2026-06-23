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
#define EST_INPUT_TOKENS 700
#define EST_OUTPUT_TOKENS 600

/* ── Internal: score a single topology against a task profile ─────────── */

static double score_topology(const topology_t *t, const task_profile_t *tp) {
    /* First check if task_profile already has an opinion on this topology name */
    for (int si = 0; si < tp->suggestion_count; si++) {
        if (tp->suggestions[si].topo && strcmp(tp->suggestions[si].topo->name, t->name) == 0) {
            return tp->suggestions[si].fit_score;
        }
    }

    /* Fall back to signal-driven scoring when not in suggestions */
    double score = 0.40; /* lower base to give signals more room */

    double par = tp->parallelism_score;
    double con = tp->convergence_score;
    double cmp = tp->complexity_score;
    double lat = tp->latency_score;

    /* Parallelism */
    if (par > 0.55) {
        if (t->category == CAT_FANOUT)
            score += par * 0.40;
        else if (t->category == CAT_MESH)
            score += par * 0.30;
        else if (t->strategy == EXEC_FULL_PARALLEL)
            score += par * 0.20;
        else if (t->category == CAT_CHAIN)
            score -= par * 0.15;
    } else if (par < 0.30) {
        if (t->category == CAT_CHAIN || t->category == CAT_SPECIALIST)
            score += 0.22;
        if (t->category == CAT_FANOUT)
            score -= 0.10;
    }

    /* Convergence / iteration */
    if (con > 0.45) {
        if (t->category == CAT_FEEDBACK)
            score += con * 0.35;
        else if (t->category == CAT_COMPETITIVE)
            score += con * 0.25;
        else if (t->strategy == EXEC_ITERATIVE)
            score += con * 0.20;
        else if (t->strategy == EXEC_CONSENSUS)
            score += con * 0.15;
    }

    /* Complexity */
    if (cmp > 0.60) {
        if (t->category == CAT_HIERARCHY)
            score += cmp * 0.25;
        if (t->total_agents > 6)
            score += 0.08;
    } else if (cmp < 0.30) {
        if (t->total_agents <= 3)
            score += 0.18;
        if (t->total_agents > 8)
            score -= 0.12;
    }

    /* Latency penalty on slow topologies */
    if (lat > 0.45) {
        score -= t->est_latency_mult * 0.10;
    }

    /* Domain bonus */
    if (t->category == CAT_DOMAIN)
        score += 0.05;

    if (score > 1.0)
        score = 1.0;
    if (score < 0.05)
        score = 0.05;
    return score;
}

/* ── Public API ───────────────────────────────────────────────────────── */

plan_options_t *plan_analyze(const char *task, int budget_cents) {
    if (!task || !task[0])
        return NULL;

    plan_options_t *opts = calloc(1, sizeof(plan_options_t));
    if (!opts)
        return NULL;

    opts->task = strdup(task);
    opts->budget_cents = budget_cents;
    opts->count = 0;

    /* Build task profile */
    task_profile_t *tp = task_profile(task, NULL);

    /* Walk all 60 registered topologies, rank them */
    int topo_count = 0;
    const topology_t *reg = topology_registry(&topo_count);

    /* Score all, collect top MAX_OPTIONS */
    typedef struct {
        double score;
        int idx;
    } scored_t;
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
            scored[j + 1] = scored[j];
            j--;
        }
        scored[j + 1] = key;
    }

    int n = (topo_count < MAX_OPTIONS) ? topo_count : MAX_OPTIONS;
    for (int i = 0; i < n; i++) {
        const topology_t *t = &reg[scored[i].idx];
        plan_option_t *o = &opts->options[i];

        o->topology_name = strdup(t->name);
        o->fit_score = scored[i].score;

        /* Cost estimate: prefer learned model, fall back to static */
        double learned = cost_model_predict(t->name, EST_INPUT_TOKENS, EST_OUTPUT_TOKENS);
        if (learned > 0.0) {
            o->est_cost_usd = learned;
            o->cost_source = COST_SOURCE_LEARNED;
        } else {
            o->est_cost_usd = topology_estimate_cost(t, EST_INPUT_TOKENS, EST_OUTPUT_TOKENS);
            o->cost_source = COST_SOURCE_STATIC;
        }

        /* Latency estimate: latency_mult * base_latency (assume ~8s per serial hop) */
        o->est_latency_s = t->est_latency_mult * 8.0;

        /* Over budget? */
        o->over_budget = (budget_cents > 0 && (int)(o->est_cost_usd * 100.0) > budget_cents);

        /* Rationale */
        snprintf(o->rationale, sizeof(o->rationale),
                 "fit=%.0f%% agents=%d cat=%s lat=%.1fs cost=$%.4f%s", o->fit_score * 100.0,
                 t->total_agents, topo_category_label(t->category), o->est_latency_s,
                 o->est_cost_usd, o->over_budget ? " [OVER BUDGET]" : "");

        opts->count++;
    }

    if (tp)
        task_profile_free(tp);
    return opts;
}

void plan_options_free(plan_options_t *opts) {
    if (!opts)
        return;
    free((char *)opts->task);
    for (int i = 0; i < opts->count; i++) {
        free((char *)opts->options[i].topology_name);
    }
    free(opts);
}

/* Render options as compact JSON */
int plan_options_json(const plan_options_t *opts, char *buf, size_t buflen) {
    if (!opts || !buf || buflen < 8)
        return 0;

    char *p = buf;
    size_t rem = buflen;
    int n;

    n = snprintf(p, rem, "{\"task\":\"%s\",\"budget_cents\":%d,\"options\":[",
                 opts->task ? opts->task : "", opts->budget_cents);
    p += n;
    rem -= (size_t)n;

    for (int i = 0; i < opts->count && rem > 8; i++) {
        const plan_option_t *o = &opts->options[i];
        n = snprintf(p, rem,
                     "%s{\"topology\":\"%s\",\"fit\":%.3f,\"cost_usd\":%.5f,"
                     "\"latency_s\":%.1f,\"over_budget\":%s,"
                     "\"cost_source\":\"%s\",\"rationale\":\"%s\"}",
                     i ? "," : "", o->topology_name ? o->topology_name : "", o->fit_score,
                     o->est_cost_usd, o->est_latency_s, o->over_budget ? "true" : "false",
                     o->cost_source == COST_SOURCE_LEARNED ? "learned" : "static", o->rationale);
        p += n;
        rem -= (size_t)n;
    }
    snprintf(p, rem, "]}");
    return (int)(buflen - rem);
}

/* Return the best non-over-budget option (or best overall if all over) */
const plan_option_t *plan_options_best(const plan_options_t *opts) {
    if (!opts || opts->count == 0)
        return NULL;
    for (int i = 0; i < opts->count; i++) {
        if (!opts->options[i].over_budget)
            return &opts->options[i];
    }
    return &opts->options[0]; /* all over budget — return top fit anyway */
}

/* ── Per-tier pricing (USD per 1 K combined tokens, ~mid-2025) ─────────── */

static const double s_tier_cost_per_1k[3] = {
    [TIER_HAIKU] = 0.00025,
    [TIER_SONNET] = 0.00300,
    [TIER_OPUS] = 0.01500,
};

/*
 * topology_cost_multiplier — structural overhead factor above raw token cost.
 *
 * Category-specific values reflect real execution patterns:
 *   mesh        1.40 — O(N²) inter-node chatter
 *   competitive 1.30 — redundant work (N attempts, 1 winner)
 *   feedback    1.20 × iterations — multi-pass refinement
 *   hierarchy   1.15 — delegation + aggregation overhead
 *   fanout      1.05 — fan coordination (mostly parallel, low waste)
 *   chain/etc   1.00 — baseline
 */
double topology_cost_multiplier(const char *topology_name) {
    if (!topology_name)
        return 1.0;

    const topology_t *t = topology_find(topology_name);
    if (!t)
        return 1.0;

    double mult;
    switch (t->category) {
        case CAT_MESH:
            mult = 1.40;
            break;
        case CAT_COMPETITIVE:
            mult = 1.30;
            break;
        case CAT_FEEDBACK:
            mult = 1.20;
            break;
        case CAT_HIERARCHY:
            mult = 1.15;
            break;
        case CAT_FANOUT:
            mult = 1.05;
            break;
        default:
            mult = 1.00;
            break;
    }

    /* Iterative strategies compound the multiplier per iteration */
    if (t->strategy == EXEC_ITERATIVE && t->max_iterations > 1) {
        double iter_factor = 0.30 + 0.70 * (double)t->max_iterations;
        mult *= iter_factor;
    }

    return mult;
}

/*
 * plan_estimate_cost — tier-weighted cost using the topology's actual node mix.
 *
 * Computes: (avg_rate × total_agents × tokens/1K) × topology_cost_multiplier.
 * Falls back to opt->est_cost_usd when topology is unknown.
 *
 * Target accuracy: within 20% of actual execution cost (Priority 1 success metric).
 */
double plan_estimate_cost(const plan_option_t *opt) {
    if (!opt || !opt->topology_name)
        return 0.0;

    const topology_t *t = topology_find(opt->topology_name);
    if (!t)
        return opt->est_cost_usd;

    /* Walk nodes; accumulate tier-weighted cost × replicas */
    double weighted_rate = 0.0;
    int total_agents = 0;
    for (int i = 0; i < t->node_count; i++) {
        int tier = (int)t->nodes[i].tier;
        int reps = t->nodes[i].replicas;
        if (tier < 0 || tier > (int)TIER_OPUS)
            tier = (int)TIER_SONNET;
        if (reps < 1)
            reps = 1;
        weighted_rate += s_tier_cost_per_1k[tier] * (double)reps;
        total_agents += reps;
    }

    if (total_agents == 0)
        return opt->est_cost_usd;

    int total_tokens = EST_INPUT_TOKENS + EST_OUTPUT_TOKENS;
    double base = weighted_rate * ((double)total_tokens / 1000.0);

    return base * topology_cost_multiplier(opt->topology_name);
}
