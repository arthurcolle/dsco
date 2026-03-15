#ifndef DSCO_ROUTER_H
#define DSCO_ROUTER_H

/*
 * router.h — Dynamic model routing and inter-model coordination engine.
 *
 * Models coordinate through the router to decide their successor on each turn,
 * optimising for cost, latency, quality, or a balance thereof.
 *
 * Architecture:
 *   1. After every streaming turn the router records per-model EMA stats.
 *   2. The router classifies the task complexity from the conversation.
 *   3. A policy-driven decision picks the optimal model for the *next* turn.
 *   4. If the chosen model differs from the current one, the agent loop
 *      applies the switch automatically (or at the user's prompt level).
 *   5. Models can also explicitly call model_recommend / model_switch tools
 *      to coordinate mid-task.
 */

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "llm.h"   /* conversation_t, session_state_t, stream_result_t */

/* ── Routing policy ───────────────────────────────────────────────────────── */

typedef enum {
    ROUTER_POLICY_FIXED    = 0,  /* never auto-switch — explicit only          */
    ROUTER_POLICY_COST,          /* minimise $/turn; degrade aggressively       */
    ROUTER_POLICY_LATENCY,       /* minimise TTFT+total_ms                      */
    ROUTER_POLICY_QUALITY,       /* maximise capability; upgrade liberally      */
    ROUTER_POLICY_BALANCED,      /* weighted cost+latency+quality score         */
    ROUTER_POLICY_ADAPTIVE,      /* EMA-learned; adjusts weights over time      */
} router_policy_t;

/* ── Task complexity ──────────────────────────────────────────────────────── */

typedef enum {
    TASK_SIMPLE  = 0,   /* lookup, format, translate, short Q&A             */
    TASK_MEDIUM,        /* multi-step code gen, moderate reasoning           */
    TASK_COMPLEX,       /* deep analysis, large codebase, extended reasoning */
    TASK_EXPERT,        /* frontier math/science, cross-domain synthesis     */
} task_complexity_t;

/* ── Switch reason ────────────────────────────────────────────────────────── */

typedef enum {
    SWITCH_REASON_NONE       = 0,
    SWITCH_REASON_EXPLICIT,       /* user / tool called model_switch explicitly */
    SWITCH_REASON_COST_BUDGET,    /* session cost approaching limit             */
    SWITCH_REASON_LATENCY,        /* TTFT or throughput out of budget           */
    SWITCH_REASON_COMPLEXITY_DOWN,/* task simpler than current model warrants   */
    SWITCH_REASON_COMPLEXITY_UP,  /* task harder; current model insufficient    */
    SWITCH_REASON_FAILURE,        /* repeated tool failures → upgrade           */
    SWITCH_REASON_CAPABILITY,     /* required feature unavailable (e.g. vision) */
    SWITCH_REASON_CONTEXT_LIMIT,  /* approaching context window of current model */
} switch_reason_t;

/* ── Per-model accumulated stats ──────────────────────────────────────────── */

#define ROUTER_STATS_MAX 24

typedef struct {
    char   model_id[128];
    int    turn_count;
    int    success_count;
    int    failure_count;
    double total_cost_usd;
    double total_latency_ms;
    double ema_latency_ms;      /* exponential moving average (α=0.25)  */
    double ema_cost_per_turn;   /* EMA cost per turn                     */
    int    total_input_tokens;
    int    total_output_tokens;
    double ema_tokens_per_sec;  /* throughput EMA                        */
} router_model_stat_t;

/* ── Routing decision ─────────────────────────────────────────────────────── */

typedef struct {
    char              model_id[128];  /* chosen model for next turn            */
    switch_reason_t   reason;
    task_complexity_t complexity;     /* classifier verdict for current turn    */
    double            estimated_cost_usd;  /* per-turn estimate                */
    double            estimated_latency_ms;
    float             confidence;     /* 0.0–1.0                               */
    char              rationale[384]; /* human-readable explanation             */
    bool              should_switch;  /* false → keep current model            */
} router_decision_t;

/* ── History entry ────────────────────────────────────────────────────────── */

#define ROUTER_HISTORY_MAX 64

typedef struct {
    char              model_id[128];
    task_complexity_t complexity;
    switch_reason_t   reason;
    double            cost_usd;
    double            latency_ms;
    bool              success;
} router_history_entry_t;

/* ── Router instance ──────────────────────────────────────────────────────── */

typedef struct {
    router_policy_t    policy;

    /* Cost/latency budgets (0 = unlimited) */
    double             cost_budget_usd;
    double             latency_budget_ms;

    /* Session accumulator */
    double             session_cost_usd;

    /* Per-model EMA stats */
    router_model_stat_t stats[ROUTER_STATS_MAX];
    int                 stats_count;

    /* Turn history */
    router_history_entry_t history[ROUTER_HISTORY_MAX];
    int                    history_count;

    /* Preferred model chain: index 0 = cheapest/fastest, last = strongest */
    char preferred_chain[8][128];
    int  chain_count;

    /* Adaptive weights (only used by ADAPTIVE policy) */
    float weight_cost;      /* 0–1 */
    float weight_latency;   /* 0–1 */
    float weight_quality;   /* 0–1 */

    /* Consecutive failure counter (reset on success) */
    int consecutive_failures;

    pthread_mutex_t lock;
} router_t;

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

void router_init(router_t *r, router_policy_t policy);
void router_destroy(router_t *r);

/* ── Task classification ──────────────────────────────────────────────────── */

/*
 * Classify task complexity from the latest user message + conversation context.
 *   tool_calls_last_turn: number of tool_use blocks in the last assistant turn.
 */
task_complexity_t router_classify_task(const char *user_msg,
                                        int conversation_turns,
                                        int tool_calls_last_turn,
                                        int context_token_pct);  /* 0-100 */

/* ── Core routing ─────────────────────────────────────────────────────────── */

/*
 * Decide the best model for the next turn.
 * Does NOT mutate session — caller applies the switch.
 */
router_decision_t router_decide(router_t *r,
                                 const char *current_model,
                                 task_complexity_t complexity,
                                 double session_cost_so_far,
                                 double last_latency_ms,
                                 int consecutive_failures);

/*
 * Record metrics after a completed streaming turn.
 * input/output_tokens: from sr.usage.
 * latency_ms: sr.telemetry.total_ms.
 * success: sr.ok.
 * cost_usd: computed externally (e.g. from model pricing * tokens).
 */
void router_record_turn(router_t *r,
                         const char *model_id,
                         int input_tokens, int output_tokens,
                         double latency_ms,
                         double cost_usd,
                         double tokens_per_sec,
                         bool success);

/* ── Coordination helpers ─────────────────────────────────────────────────── */

/* Return the best cheaper model in the registry below current complexity. */
const char *router_cheaper_model(const char *current_model);

/* Return the best stronger model in the registry above current complexity. */
const char *router_stronger_model(const char *current_model);

/* Return the fastest model registered that can handle the given complexity. */
const char *router_fastest_model(task_complexity_t min_complexity);

/* ── Stats & inspection ───────────────────────────────────────────────────── */

router_model_stat_t *router_get_stats(router_t *r, const char *model_id);
int  router_to_json(router_t *r, char *buf, size_t len);
int  router_history_to_json(router_t *r, char *buf, size_t len);

/* ── Policy/complexity name helpers ──────────────────────────────────────── */

const char        *router_policy_name(router_policy_t p);
router_policy_t    router_policy_parse(const char *s);
const char        *task_complexity_name(task_complexity_t c);
task_complexity_t  task_complexity_parse(const char *s);
const char        *switch_reason_name(switch_reason_t r);

/* ── Model tier (1=simple .. 4=expert) ───────────────────────────────────── */

int model_tier(const char *model_id);

/* ── Global instance (owned by agent.c, exposed for tools) ───────────────── */

extern router_t g_router;

#endif /* DSCO_ROUTER_H */
