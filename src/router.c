/*
 * router.c — Dynamic model routing and inter-model coordination engine.
 *
 * Each streaming turn feeds observed cost, latency, and success/failure data
 * back into per-model EMA accumulators.  On the *following* turn the router's
 * policy function scores every registered model against the classified task
 * complexity and returns a router_decision_t — including a human-readable
 * rationale so the agent can explain switches to the user.
 */

#include "router.h"
#include "config.h"
#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <pthread.h>

/* ── Global instance ──────────────────────────────────────────────────────── */

router_t g_router;

/* ── EMA smoothing factor ─────────────────────────────────────────────────── */

#define EMA_ALPHA 0.25

static double ema_update(double prev, double sample) {
    if (prev <= 0.0) return sample;
    return EMA_ALPHA * sample + (1.0 - EMA_ALPHA) * prev;
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

void router_init(router_t *r, router_policy_t policy) {
    memset(r, 0, sizeof(*r));
    r->policy          = policy;
    r->weight_cost     = 0.4f;
    r->weight_latency  = 0.3f;
    r->weight_quality  = 0.3f;
    pthread_mutex_init(&r->lock, NULL);
}

void router_destroy(router_t *r) {
    pthread_mutex_destroy(&r->lock);
    memset(r, 0, sizeof(*r));
}

/* ── Name helpers ─────────────────────────────────────────────────────────── */

const char *router_policy_name(router_policy_t p) {
    switch (p) {
        case ROUTER_POLICY_FIXED:    return "fixed";
        case ROUTER_POLICY_COST:     return "cost";
        case ROUTER_POLICY_LATENCY:  return "latency";
        case ROUTER_POLICY_QUALITY:  return "quality";
        case ROUTER_POLICY_BALANCED: return "balanced";
        case ROUTER_POLICY_ADAPTIVE: return "adaptive";
        default:                     return "unknown";
    }
}

router_policy_t router_policy_parse(const char *s) {
    if (!s) return ROUTER_POLICY_FIXED;
    if (strcmp(s, "cost")     == 0) return ROUTER_POLICY_COST;
    if (strcmp(s, "latency")  == 0) return ROUTER_POLICY_LATENCY;
    if (strcmp(s, "quality")  == 0) return ROUTER_POLICY_QUALITY;
    if (strcmp(s, "balanced") == 0) return ROUTER_POLICY_BALANCED;
    if (strcmp(s, "adaptive") == 0) return ROUTER_POLICY_ADAPTIVE;
    return ROUTER_POLICY_FIXED;
}

const char *task_complexity_name(task_complexity_t c) {
    switch (c) {
        case TASK_SIMPLE:  return "simple";
        case TASK_MEDIUM:  return "medium";
        case TASK_COMPLEX: return "complex";
        case TASK_EXPERT:  return "expert";
        default:           return "unknown";
    }
}

task_complexity_t task_complexity_parse(const char *s) {
    if (!s) return TASK_MEDIUM;
    if (strcmp(s, "simple")  == 0) return TASK_SIMPLE;
    if (strcmp(s, "medium")  == 0) return TASK_MEDIUM;
    if (strcmp(s, "complex") == 0) return TASK_COMPLEX;
    if (strcmp(s, "expert")  == 0) return TASK_EXPERT;
    return TASK_MEDIUM;
}

const char *switch_reason_name(switch_reason_t r) {
    switch (r) {
        case SWITCH_REASON_NONE:            return "none";
        case SWITCH_REASON_EXPLICIT:        return "explicit";
        case SWITCH_REASON_COST_BUDGET:     return "cost_budget";
        case SWITCH_REASON_LATENCY:         return "latency";
        case SWITCH_REASON_COMPLEXITY_DOWN: return "complexity_down";
        case SWITCH_REASON_COMPLEXITY_UP:   return "complexity_up";
        case SWITCH_REASON_FAILURE:         return "failure";
        case SWITCH_REASON_CAPABILITY:      return "capability";
        case SWITCH_REASON_CONTEXT_LIMIT:   return "context_limit";
        default:                            return "unknown";
    }
}

/* ── Task complexity classifier ───────────────────────────────────────────── */

/*
 * Heuristic signals:
 *   - word count of user message
 *   - reasoning / analysis keywords → up
 *   - formatting / translation keywords → down
 *   - number of tool calls last turn → up
 *   - context token pressure → up
 *   - conversation depth → moderate up
 */

typedef struct { const char *word; int score; } kw_t;

static const kw_t KEYWORDS_UP[] = {
    {"analyze", 3}, {"analyse", 3}, {"reason", 3}, {"explain", 2},
    {"compare", 2}, {"evaluate", 3}, {"synthesize", 4}, {"synthesise", 4},
    {"architecture", 3}, {"design", 2}, {"implement", 2}, {"refactor", 2},
    {"debug", 2}, {"diagnose", 3}, {"security", 3}, {"optimize", 2},
    {"optimise", 2}, {"research", 3}, {"prove", 4}, {"derive", 4},
    {"theorem", 4}, {"algorithm", 3}, {"complex", 2}, {"distributed", 3},
    {"concurrency", 3}, {"performance", 2}, {"trade-off", 2}, {"tradeoff", 2},
    {"review", 2}, {"audit", 3}, {"codebase", 2}, {"entire", 2},
    {NULL, 0}
};

static const kw_t KEYWORDS_DOWN[] = {
    {"translate", -2}, {"format", -2}, {"convert", -1}, {"list", -1},
    {"summarize", -1}, {"summarise", -1}, {"spell", -2}, {"grammar", -2},
    {"hello", -3}, {"hi", -3}, {"thanks", -3}, {"thank", -2},
    {"yes", -3}, {"no", -3}, {"ok", -3}, {"sure", -2},
    {"what is", -1}, {"what's", -1}, {"who is", -2}, {"define", -1},
    {NULL, 0}
};

static int keyword_score(const char *msg) {
    if (!msg) return 0;
    int score = 0;
    char lower[4096];
    size_t i = 0;
    while (msg[i] && i < sizeof(lower) - 1) {
        lower[i] = (char)tolower((unsigned char)msg[i]);
        i++;
    }
    lower[i] = '\0';

    for (int k = 0; KEYWORDS_UP[k].word; k++)
        if (strstr(lower, KEYWORDS_UP[k].word)) score += KEYWORDS_UP[k].score;
    for (int k = 0; KEYWORDS_DOWN[k].word; k++)
        if (strstr(lower, KEYWORDS_DOWN[k].word)) score += KEYWORDS_DOWN[k].score;
    return score;
}

task_complexity_t router_classify_task(const char *user_msg,
                                        int conversation_turns,
                                        int tool_calls_last_turn,
                                        int context_token_pct) {
    int score = 0;

    /* Word count */
    int words = 0;
    if (user_msg) {
        bool in_word = false;
        for (const char *p = user_msg; *p; p++) {
            if (isspace((unsigned char)*p)) { in_word = false; }
            else if (!in_word)             { in_word = true; words++; }
        }
    }
    if (words > 200) score += 4;
    else if (words > 80)  score += 2;
    else if (words > 30)  score += 1;
    else if (words < 8)   score -= 2;

    /* Keyword heuristics */
    score += keyword_score(user_msg);

    /* Tool usage pressure */
    if (tool_calls_last_turn > 5)      score += 3;
    else if (tool_calls_last_turn > 2) score += 2;
    else if (tool_calls_last_turn > 0) score += 1;

    /* Conversation depth */
    if (conversation_turns > 20) score += 2;
    else if (conversation_turns > 8)  score += 1;

    /* Context pressure */
    if (context_token_pct > 70)      score += 3;
    else if (context_token_pct > 50) score += 2;
    else if (context_token_pct > 30) score += 1;

    if (score <= 0)  return TASK_SIMPLE;
    if (score <= 4)  return TASK_MEDIUM;
    if (score <= 9)  return TASK_COMPLEX;
    return TASK_EXPERT;
}

/* ── Model tier (for routing decisions) ───────────────────────────────────── */

/*
 * Assigns a capability tier to each registered model.
 * Higher tier = more capable / more expensive.
 */
int model_tier(const char *model_id) {
    if (!model_id) return 1;
    /* Expert tier */
    if (strstr(model_id, "opus"))           return 4;
    if (strstr(model_id, "o1"))             return 4;
    if (strstr(model_id, "deepseek-reasoner")) return 4;
    if (strstr(model_id, "o3"))             return 4;
    /* Complex tier */
    if (strstr(model_id, "sonnet"))         return 3;
    if (strstr(model_id, "gpt-4o")
        && !strstr(model_id, "mini"))       return 3;
    if (strstr(model_id, "mistral-large")) return 3;
    if (strstr(model_id, "llama-3.3-70b")) return 3;
    if (strstr(model_id, "deepseek-chat")) return 3;
    if (strstr(model_id, "grok"))          return 3;
    if (strstr(model_id, "command-r"))     return 3;
    if (strstr(model_id, "sonar-pro"))     return 3;
    /* Medium tier */
    if (strstr(model_id, "gpt-4o-mini"))  return 2;
    if (strstr(model_id, "mistral-small")) return 2;
    if (strstr(model_id, "mixtral"))       return 2;
    if (strstr(model_id, "Qwen"))          return 2;
    if (strstr(model_id, "llama3.1-70b")) return 2;  /* cerebras */
    if (strstr(model_id, "sonar"))        return 2;
    /* Simple tier */
    if (strstr(model_id, "haiku"))         return 1;
    if (strstr(model_id, "llama-3.1-8b")) return 1;
    if (strstr(model_id, "mistral-s"))    return 1;
    return 2;   /* default: medium */
}

/* Minimum tier needed for a given complexity level */
static int min_tier_for_complexity(task_complexity_t c) {
    switch (c) {
        case TASK_SIMPLE:  return 1;
        case TASK_MEDIUM:  return 2;
        case TASK_COMPLEX: return 3;
        case TASK_EXPERT:  return 4;
        default:           return 2;
    }
}

/* ── Per-model stat lookup / create ───────────────────────────────────────── */

router_model_stat_t *router_get_stats(router_t *r, const char *model_id) {
    if (!model_id) return NULL;
    for (int i = 0; i < r->stats_count; i++) {
        if (strcmp(r->stats[i].model_id, model_id) == 0)
            return &r->stats[i];
    }
    if (r->stats_count >= ROUTER_STATS_MAX) return NULL;
    router_model_stat_t *s = &r->stats[r->stats_count++];
    memset(s, 0, sizeof(*s));
    snprintf(s->model_id, sizeof(s->model_id), "%s", model_id);
    return s;
}

/* ── Record a completed turn ──────────────────────────────────────────────── */

void router_record_turn(router_t *r,
                         const char *model_id,
                         int input_tokens, int output_tokens,
                         double latency_ms,
                         double cost_usd,
                         double tokens_per_sec,
                         bool success) {
    pthread_mutex_lock(&r->lock);

    router_model_stat_t *s = router_get_stats(r, model_id);
    if (s) {
        s->turn_count++;
        if (success) s->success_count++; else s->failure_count++;
        s->total_cost_usd     += cost_usd;
        s->total_latency_ms   += latency_ms;
        s->total_input_tokens  += input_tokens;
        s->total_output_tokens += output_tokens;
        s->ema_latency_ms      = ema_update(s->ema_latency_ms, latency_ms);
        s->ema_cost_per_turn   = ema_update(s->ema_cost_per_turn, cost_usd);
        s->ema_tokens_per_sec  = ema_update(s->ema_tokens_per_sec, tokens_per_sec);
    }

    r->session_cost_usd += cost_usd;

    /* Update history */
    if (r->history_count < ROUTER_HISTORY_MAX) {
        router_history_entry_t *h = &r->history[r->history_count++];
        snprintf(h->model_id, sizeof(h->model_id), "%s", model_id ? model_id : "");
        h->cost_usd    = cost_usd;
        h->latency_ms  = latency_ms;
        h->success     = success;
    }

    /* Adaptive weight update: if turn succeeded and was cheap, reinforce cost
       weight; if latency was high, nudge latency weight up. */
    if (r->policy == ROUTER_POLICY_ADAPTIVE) {
        float total = r->weight_cost + r->weight_latency + r->weight_quality;
        if (total < 0.01f) total = 1.0f;
        if (success) {
            if (cost_usd < 0.001)
                r->weight_cost = fminf(0.7f, r->weight_cost + 0.01f);
            if (latency_ms > 8000)
                r->weight_latency = fminf(0.7f, r->weight_latency + 0.01f);
        }
        /* Re-normalise */
        total = r->weight_cost + r->weight_latency + r->weight_quality;
        if (total > 0.01f) {
            r->weight_cost    /= total;
            r->weight_latency /= total;
            r->weight_quality /= total;
        }
    }

    pthread_mutex_unlock(&r->lock);
}

/* ── Core routing decision ────────────────────────────────────────────────── */

/*
 * Score a candidate model for a given policy.
 * Lower score = more preferable.
 */
static double score_model_for_policy(router_t *r,
                                      const model_info_t *mi,
                                      router_policy_t policy,
                                      double latency_budget_ms) {
    if (!mi) return 1e9;

    /* Gather historical EMA if available */
    router_model_stat_t *st = router_get_stats(r, mi->model_id);
    double ema_lat  = st && st->ema_latency_ms  > 0 ? st->ema_latency_ms  : 3000.0;
    double ema_cost = st && st->ema_cost_per_turn > 0 ? st->ema_cost_per_turn : mi->input_price * 1000 / 1e6;

    /* Normalise: cost relative to opus ($75/1M out), latency relative to 10s */
    double norm_cost    = ema_cost / 0.075;  /* opus output = 1.0 */
    double norm_latency = ema_lat  / 10000.0;
    double norm_quality = 1.0 - ((double)model_tier(mi->model_id) - 1.0) / 3.0; /* invert: low tier = high cost */

    if (latency_budget_ms > 0 && ema_lat > latency_budget_ms)
        return 1e9;  /* exceeds latency budget — disqualify */

    switch (policy) {
        case ROUTER_POLICY_COST:
            return norm_cost;
        case ROUTER_POLICY_LATENCY:
            return norm_latency;
        case ROUTER_POLICY_QUALITY:
            return norm_quality;
        case ROUTER_POLICY_BALANCED:
            return 0.4 * norm_cost + 0.3 * norm_latency + 0.3 * norm_quality;
        case ROUTER_POLICY_ADAPTIVE:
            return (double)r->weight_cost    * norm_cost
                 + (double)r->weight_latency * norm_latency
                 + (double)r->weight_quality * norm_quality;
        default:
            return 0.0; /* FIXED: don't switch */
    }
}

router_decision_t router_decide(router_t *r,
                                 const char *current_model,
                                 task_complexity_t complexity,
                                 double session_cost_so_far,
                                 double last_latency_ms __attribute__((unused)),
                                 int consecutive_failures) {
    router_decision_t d;
    memset(&d, 0, sizeof(d));
    d.complexity    = complexity;
    d.confidence    = 0.5f;
    d.should_switch = false;
    snprintf(d.model_id, sizeof(d.model_id), "%s", current_model ? current_model : "");

    pthread_mutex_lock(&r->lock);

    /* FIXED policy — never switch */
    if (r->policy == ROUTER_POLICY_FIXED) {
        snprintf(d.rationale, sizeof(d.rationale),
                 "policy=fixed; staying on %s", current_model ? current_model : "?");
        pthread_mutex_unlock(&r->lock);
        return d;
    }

    /* Override: cost budget exceeded → pick cheapest capable model */
    if (r->cost_budget_usd > 0 && session_cost_so_far >= r->cost_budget_usd * 0.9) {
        const char *cheaper = router_cheaper_model(current_model);
        if (cheaper && strcmp(cheaper, current_model) != 0) {
            snprintf(d.model_id, sizeof(d.model_id), "%s", cheaper);
            d.reason       = SWITCH_REASON_COST_BUDGET;
            d.should_switch = true;
            d.confidence   = 0.95f;
            snprintf(d.rationale, sizeof(d.rationale),
                     "session cost $%.4f approaching budget $%.4f; downgrading to %s",
                     session_cost_so_far, r->cost_budget_usd, cheaper);
            pthread_mutex_unlock(&r->lock);
            return d;
        }
    }

    /* Override: consecutive failures → escalate */
    if (consecutive_failures >= 2) {
        const char *stronger = router_stronger_model(current_model);
        if (stronger && strcmp(stronger, current_model) != 0) {
            snprintf(d.model_id, sizeof(d.model_id), "%s", stronger);
            d.reason       = SWITCH_REASON_FAILURE;
            d.should_switch = true;
            d.confidence   = 0.80f;
            snprintf(d.rationale, sizeof(d.rationale),
                     "%d consecutive failures on %s; escalating to %s",
                     consecutive_failures, current_model ? current_model : "?", stronger);
            pthread_mutex_unlock(&r->lock);
            return d;
        }
    }

    int current_tier = model_tier(current_model);
    int min_tier     = min_tier_for_complexity(complexity);

    /* Scan registry for best model matching policy + complexity floor */
    const char *best_id    = current_model;
    double      best_score = 1e9;
    bool        found      = false;

    for (int i = 0; MODEL_REGISTRY[i].alias; i++) {
        const model_info_t *mi = &MODEL_REGISTRY[i];
        int tier = model_tier(mi->model_id);
        if (tier < min_tier) continue;  /* too weak */

        double score = score_model_for_policy(r, mi, r->policy, r->latency_budget_ms);
        if (!found || score < best_score) {
            best_score = score;
            best_id    = mi->model_id;
            found      = true;
        }
    }

    if (!best_id || !current_model) {
        snprintf(d.rationale, sizeof(d.rationale), "no suitable model found; keeping current");
        pthread_mutex_unlock(&r->lock);
        return d;
    }

    /* Fill estimate fields */
    const model_info_t *best_mi = model_lookup(best_id);
    if (best_mi) {
        d.estimated_cost_usd     = best_mi->input_price * 2000 / 1e6
                                 + best_mi->output_price * 500 / 1e6; /* rough 2k in / 500 out */
        router_model_stat_t *bst = router_get_stats(r, best_id);
        d.estimated_latency_ms   = bst && bst->ema_latency_ms > 0
                                 ? bst->ema_latency_ms : 3000.0;
    }

    bool differs = current_model && (strcmp(best_id, current_model) != 0);

    if (differs) {
        int best_tier = model_tier(best_id);
        d.should_switch = true;
        snprintf(d.model_id, sizeof(d.model_id), "%s", best_id);

        if (best_tier > current_tier) {
            d.reason    = SWITCH_REASON_COMPLEXITY_UP;
            d.confidence = 0.70f;
            snprintf(d.rationale, sizeof(d.rationale),
                     "task=%s requires tier %d; upgrading %s→%s (policy=%s score=%.3f)",
                     task_complexity_name(complexity), min_tier,
                     current_model, best_id,
                     router_policy_name(r->policy), best_score);
        } else {
            d.reason    = SWITCH_REASON_COMPLEXITY_DOWN;
            d.confidence = 0.65f;
            snprintf(d.rationale, sizeof(d.rationale),
                     "task=%s; downgrading %s→%s saves cost (policy=%s score=%.3f)",
                     task_complexity_name(complexity),
                     current_model, best_id,
                     router_policy_name(r->policy), best_score);
        }
    } else {
        snprintf(d.rationale, sizeof(d.rationale),
                 "%s already optimal for task=%s (policy=%s score=%.3f)",
                 current_model, task_complexity_name(complexity),
                 router_policy_name(r->policy), best_score);
    }

    pthread_mutex_unlock(&r->lock);
    return d;
}

/* ── Coordination helpers ─────────────────────────────────────────────────── */

const char *router_cheaper_model(const char *current_model) {
    int cur_tier = model_tier(current_model);
    const char *best = NULL;
    double best_price = 1e9;

    for (int i = 0; MODEL_REGISTRY[i].alias; i++) {
        const model_info_t *mi = &MODEL_REGISTRY[i];
        int tier = model_tier(mi->model_id);
        if (tier >= cur_tier) continue;
        double price = mi->input_price + mi->output_price;
        if (price < best_price) {
            best_price = price;
            best = mi->model_id;
        }
    }
    return best ? best : current_model;
}

const char *router_stronger_model(const char *current_model) {
    int cur_tier = model_tier(current_model);
    const char *best = NULL;
    double best_output = 0;

    for (int i = 0; MODEL_REGISTRY[i].alias; i++) {
        const model_info_t *mi = &MODEL_REGISTRY[i];
        int tier = model_tier(mi->model_id);
        if (tier <= cur_tier) continue;
        if (mi->output_price > best_output) {
            best_output = mi->output_price;
            best = mi->model_id;
        }
    }
    return best ? best : current_model;
}

const char *router_fastest_model(task_complexity_t min_complexity) {
    int min_tier = min_tier_for_complexity(min_complexity);
    /* Use registry order; haiku/8b are first cheap models.
       Without live latency data, proxy with lowest output_price among capable. */
    const char *best = NULL;
    double best_price = 1e9;
    for (int i = 0; MODEL_REGISTRY[i].alias; i++) {
        const model_info_t *mi = &MODEL_REGISTRY[i];
        if (model_tier(mi->model_id) < min_tier) continue;
        double price = mi->input_price + mi->output_price;
        if (price < best_price) {
            best_price = price;
            best = mi->model_id;
        }
    }
    return best ? best : DEFAULT_MODEL;
}

/* ── Serialisation ────────────────────────────────────────────────────────── */

int router_to_json(router_t *r, char *buf, size_t len) {
    pthread_mutex_lock(&r->lock);
    jbuf_t b;
    jbuf_init(&b, 4096);

    jbuf_append(&b, "{");
    jbuf_appendf(&b, "\"policy\":\"%s\"", router_policy_name(r->policy));
    jbuf_appendf(&b, ",\"session_cost_usd\":%.6f", r->session_cost_usd);
    jbuf_appendf(&b, ",\"cost_budget_usd\":%.6f", r->cost_budget_usd);
    jbuf_appendf(&b, ",\"latency_budget_ms\":%.0f", r->latency_budget_ms);
    jbuf_appendf(&b, ",\"consecutive_failures\":%d", r->consecutive_failures);
    jbuf_appendf(&b, ",\"weights\":{\"cost\":%.3f,\"latency\":%.3f,\"quality\":%.3f}",
                 (double)r->weight_cost, (double)r->weight_latency, (double)r->weight_quality);

    jbuf_append(&b, ",\"models\":[");
    for (int i = 0; i < r->stats_count; i++) {
        router_model_stat_t *s = &r->stats[i];
        if (i > 0) jbuf_append(&b, ",");
        jbuf_append(&b, "{");
        jbuf_appendf(&b, "\"model\":\"%s\"", s->model_id);
        jbuf_appendf(&b, ",\"turns\":%d", s->turn_count);
        jbuf_appendf(&b, ",\"successes\":%d", s->success_count);
        jbuf_appendf(&b, ",\"failures\":%d", s->failure_count);
        jbuf_appendf(&b, ",\"total_cost_usd\":%.6f", s->total_cost_usd);
        jbuf_appendf(&b, ",\"ema_latency_ms\":%.0f", s->ema_latency_ms);
        jbuf_appendf(&b, ",\"ema_cost_per_turn\":%.6f", s->ema_cost_per_turn);
        jbuf_appendf(&b, ",\"ema_tokens_per_sec\":%.1f", s->ema_tokens_per_sec);
        jbuf_append(&b, "}");
    }
    jbuf_append(&b, "]}");

    int n = snprintf(buf, len, "%s", b.data ? b.data : "{}");
    jbuf_free(&b);
    pthread_mutex_unlock(&r->lock);
    return n;
}

int router_history_to_json(router_t *r, char *buf, size_t len) {
    pthread_mutex_lock(&r->lock);
    jbuf_t b;
    jbuf_init(&b, 4096);
    jbuf_append(&b, "[");
    for (int i = 0; i < r->history_count; i++) {
        router_history_entry_t *h = &r->history[i];
        if (i > 0) jbuf_append(&b, ",");
        jbuf_appendf(&b, "{\"model\":\"%s\",\"reason\":\"%s\","
                         "\"complexity\":\"%s\","
                         "\"cost_usd\":%.6f,\"latency_ms\":%.0f,\"ok\":%s}",
                     h->model_id,
                     switch_reason_name(h->reason),
                     task_complexity_name(h->complexity),
                     h->cost_usd, h->latency_ms,
                     h->success ? "true" : "false");
    }
    jbuf_append(&b, "]");
    int n = snprintf(buf, len, "%s", b.data ? b.data : "[]");
    jbuf_free(&b);
    pthread_mutex_unlock(&r->lock);
    return n;
}
