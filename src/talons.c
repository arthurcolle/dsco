#include "talons.h"
#include "vfs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static vfs_db_t *g_talons_vfs = NULL;
static const talons_engine_t *s_last_engine = NULL;
static void persist_strategy_from(const talons_engine_t *t, int si);

/* ═══════════════════════════════════════════════════════════════════════════
 * Talons — Competitive Execution Engine
 *
 * The grip. The capture. The win.
 * Not safety. Not guardrails. Pure execution power.
 * ═══════════════════════════════════════════════════════════════════════════ */

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* ── Name Tables ──────────────────────────────────────────────────────── */

static const char *GOAL_STATE_NAMES[] = {
    "nascent", "stalking", "striking", "gripping",
    "captured", "escaped", "abandoned"
};

static const char *STRATEGY_NAMES[] = {
    "direct", "flanking", "tournament", "escalation", "divide", "ambush"
};

static const char *GRIP_NAMES[] = {
    "tentative", "holding", "locked", "death_grip"
};

const char *talons_goal_state_name(goal_state_t s) {
    return (s >= 0 && s <= GOAL_ABANDONED) ? GOAL_STATE_NAMES[s] : "unknown";
}

const char *talons_strategy_name(strategy_type_t s) {
    return (s >= 0 && s <= STRATEGY_AMBUSH) ? STRATEGY_NAMES[s] : "unknown";
}

const char *talons_grip_name(grip_strength_t g) {
    return (g >= 0 && g <= GRIP_DEATH_GRIP) ? GRIP_NAMES[g] : "unknown";
}

/* ── Max retries per grip strength ────────────────────────────────────── */

static int grip_max_retries(grip_strength_t g) {
    switch (g) {
    case GRIP_TENTATIVE:  return 1;
    case GRIP_HOLDING:    return 3;
    case GRIP_LOCKED:     return 7;
    case GRIP_DEATH_GRIP: return 20;
    default: return 1;
    }
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void talons_init(talons_engine_t *t) {
    memset(t, 0, sizeof(*t));
    t->initialized = true;
    /* Initialize strategy success rates with priors */
    for (int i = 0; i < 6; i++) {
        t->strategy_success[i] = 0.5; /* neutral prior */
        t->strategy_attempts[i] = 1;  /* avoid div-by-zero */
    }
}

/* ── Goal helpers ─────────────────────────────────────────────────────── */

static goal_t *find_goal(talons_engine_t *t, int goal_id) {
    for (int i = 0; i < t->goal_count; i++) {
        if (t->goals[i].id == goal_id) return &t->goals[i];
    }
    return NULL;
}

static void record_hunt(talons_engine_t *t, const goal_t *g, bool won) {
    s_last_engine = t;
    if (t->history_count >= TALONS_MAX_HISTORY) {
        /* Shift out oldest half */
        int half = TALONS_MAX_HISTORY / 2;
        memmove(&t->history[0], &t->history[half],
                (TALONS_MAX_HISTORY - half) * sizeof(hunt_record_t));
        t->history_count = TALONS_MAX_HISTORY - half;
    }

    hunt_record_t *h = &t->history[t->history_count++];
    h->goal_id = g->id;
    h->won = won;
    h->strategy_used = g->strategy;
    h->attempts = g->attempts;
    h->duration = (g->completed_at > 0 ? g->completed_at : now_sec()) - g->created_at;
    h->cost = g->total_cost;
    h->confidence_at_start = g->confidence;
    h->timestamp = now_sec();

    /* Update strategy stats */
    int si = (int)g->strategy;
    if (si >= 0 && si < 6) {
        t->strategy_attempts[si]++;
        if (won) t->strategy_success[si] += 1.0;
        /* Recalc rate */
        /* Bayesian update: success_rate = successes / attempts */
    }

    /* Update engine stats */
    t->total_hunts++;
    if (won) {
        t->wins++;
        t->avg_attempts_to_win = ((t->wins - 1) * t->avg_attempts_to_win + g->attempts) / t->wins;
        t->avg_cost_per_win = ((t->wins - 1) * t->avg_cost_per_win + g->total_cost) / t->wins;
    } else {
        t->losses++;
    }
    t->win_rate = t->total_hunts > 0 ? (double)t->wins / t->total_hunts : 0;

    /* Persist updated strategy stats to VFS */
    if (g_talons_vfs && si >= 0 && si < 6) {
        persist_strategy_from(t, si);
    }
}

static void recount_active(talons_engine_t *t) {
    t->active_goals = 0;
    for (int i = 0; i < t->goal_count; i++) {
        goal_state_t s = t->goals[i].state;
        if (s == GOAL_STALKING || s == GOAL_STRIKING || s == GOAL_GRIPPING)
            t->active_goals++;
    }
}

/* ── Goal Management ──────────────────────────────────────────────────── */

int talons_goal_create(talons_engine_t *t, const char *description,
                       double priority, grip_strength_t grip,
                       strategy_type_t strategy, double deadline) {
    if (!t || !t->initialized || t->goal_count >= TALONS_MAX_GOALS) return -1;

    goal_t *g = &t->goals[t->goal_count++];
    memset(g, 0, sizeof(*g));
    g->id = t->next_goal_id++;
    snprintf(g->description, sizeof(g->description), "%s", description ? description : "");
    g->state = GOAL_NASCENT;
    g->grip = grip;
    g->strategy = strategy;
    g->priority = priority;
    g->confidence = 0.5; /* neutral starting confidence */
    g->deadline = deadline;
    g->created_at = now_sec();
    g->parent_goal_id = -1;
    g->tournament_id = -1;
    g->max_attempts = grip_max_retries(grip);

    return g->id;
}

int talons_goal_create_sub(talons_engine_t *t, int parent_id,
                           const char *description, double priority) {
    goal_t *parent = find_goal(t, parent_id);
    if (!parent) return -1;

    int id = talons_goal_create(t, description, priority,
                                 parent->grip, parent->strategy, parent->deadline);
    if (id >= 0) {
        goal_t *sub = find_goal(t, id);
        if (sub) {
            sub->parent_goal_id = parent_id;
            parent->sub_goal_count++;
        }
    }
    return id;
}

bool talons_goal_stalk(talons_engine_t *t, int goal_id) {
    goal_t *g = find_goal(t, goal_id);
    if (!g || g->state != GOAL_NASCENT) return false;
    g->state = GOAL_STALKING;
    g->started_at = now_sec();
    recount_active(t);
    return true;
}

bool talons_goal_strike(talons_engine_t *t, int goal_id) {
    goal_t *g = find_goal(t, goal_id);
    if (!g || (g->state != GOAL_STALKING && g->state != GOAL_NASCENT)) return false;
    g->state = GOAL_STRIKING;
    g->attempts++;
    if (g->started_at == 0) g->started_at = now_sec();
    recount_active(t);
    return true;
}

bool talons_goal_grip(talons_engine_t *t, int goal_id) {
    goal_t *g = find_goal(t, goal_id);
    if (!g || g->state != GOAL_STRIKING) return false;
    g->state = GOAL_GRIPPING;
    recount_active(t);
    return true;
}

bool talons_goal_capture(talons_engine_t *t, int goal_id,
                         const char *result, double cost) {
    goal_t *g = find_goal(t, goal_id);
    if (!g) return false;
    g->state = GOAL_CAPTURED;
    g->completed_at = now_sec();
    g->total_cost += cost;
    if (result) snprintf(g->last_result, sizeof(g->last_result), "%s", result);

    /* Update parent progress */
    if (g->parent_goal_id >= 0) {
        goal_t *parent = find_goal(t, g->parent_goal_id);
        if (parent) {
            parent->sub_goals_completed++;
            /* Auto-capture parent if all sub-goals done */
            if (parent->sub_goals_completed >= parent->sub_goal_count &&
                parent->sub_goal_count > 0) {
                talons_goal_capture(t, parent->id, "all sub-goals captured", 0);
            }
        }
    }

    record_hunt(t, g, true);
    recount_active(t);
    return true;
}

bool talons_goal_escaped(talons_engine_t *t, int goal_id,
                         const char *reason, double cost) {
    goal_t *g = find_goal(t, goal_id);
    if (!g) return false;

    g->total_cost += cost;
    if (reason) snprintf(g->last_result, sizeof(g->last_result), "%s", reason);

    /* Check if we should retry based on grip strength */
    if (g->attempts < g->max_attempts) {
        /* Don't give up — retry with potentially different strategy */
        g->state = GOAL_STALKING; /* back to stalking for next attempt */
        g->confidence *= 0.9;     /* slightly decrease confidence */
        return true; /* returning true = retrying, not truly escaped */
    }

    /* Grip exhausted — goal truly escaped */
    g->state = GOAL_ESCAPED;
    g->completed_at = now_sec();
    record_hunt(t, g, false);
    recount_active(t);
    return false; /* returning false = truly escaped */
}

bool talons_goal_abandon(talons_engine_t *t, int goal_id, const char *reason) {
    goal_t *g = find_goal(t, goal_id);
    if (!g) return false;
    g->state = GOAL_ABANDONED;
    g->completed_at = now_sec();
    if (reason) snprintf(g->last_result, sizeof(g->last_result), "%s", reason);
    record_hunt(t, g, false);
    recount_active(t);
    return true;
}

bool talons_goal_update_confidence(talons_engine_t *t, int goal_id,
                                    double new_confidence) {
    goal_t *g = find_goal(t, goal_id);
    if (!g) return false;
    if (new_confidence < 0) new_confidence = 0;
    if (new_confidence > 1) new_confidence = 1;
    g->confidence = new_confidence;
    return true;
}

const goal_t *talons_goal_get(const talons_engine_t *t, int goal_id) {
    for (int i = 0; i < t->goal_count; i++) {
        if (t->goals[i].id == goal_id) return &t->goals[i];
    }
    return NULL;
}

int talons_active_goals(const talons_engine_t *t, const goal_t **out, int max) {
    if (!t || !out) return 0;
    int n = 0;
    /* Collect active goals */
    for (int i = 0; i < t->goal_count && n < max; i++) {
        goal_state_t s = t->goals[i].state;
        if (s == GOAL_NASCENT || s == GOAL_STALKING ||
            s == GOAL_STRIKING || s == GOAL_GRIPPING) {
            out[n++] = &t->goals[i];
        }
    }
    /* Sort by priority (simple bubble — max 64 goals) */
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (out[j]->priority > out[i]->priority) {
                const goal_t *tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }
    return n;
}

/* ── Tournament ───────────────────────────────────────────────────────── */

int talons_tournament_begin(talons_engine_t *t, const char *objective,
                            double deadline,
                            double weight_quality, double weight_speed,
                            double weight_cost) {
    if (!t || !t->initialized || t->tournament_count >= TALONS_MAX_STRATEGIES)
        return -1;

    tournament_t *tr = &t->tournaments[t->tournament_count++];
    memset(tr, 0, sizeof(*tr));
    tr->id = t->next_tournament_id++;
    snprintf(tr->objective, sizeof(tr->objective), "%s", objective ? objective : "");
    tr->active = true;
    tr->started_at = now_sec();
    tr->deadline = deadline;
    tr->winner_id = -1;
    tr->weight_quality = weight_quality > 0 ? weight_quality : 0.5;
    tr->weight_speed = weight_speed > 0 ? weight_speed : 0.3;
    tr->weight_cost = weight_cost > 0 ? weight_cost : 0.2;

    return tr->id;
}

static tournament_t *find_tournament(talons_engine_t *t, int tid) {
    for (int i = 0; i < t->tournament_count; i++) {
        if (t->tournaments[i].id == tid) return &t->tournaments[i];
    }
    return NULL;
}

int talons_tournament_add(talons_engine_t *t, int tournament_id,
                          const char *label, strategy_type_t strategy) {
    tournament_t *tr = find_tournament(t, tournament_id);
    if (!tr || !tr->active || tr->competitor_count >= TALONS_MAX_COMPETITORS)
        return -1;

    tournament_competitor_t *c = &tr->competitors[tr->competitor_count];
    memset(c, 0, sizeof(*c));
    c->id = tr->competitor_count++;
    snprintf(c->label, sizeof(c->label), "%s", label ? label : "");
    c->strategy = strategy;
    c->started_at = now_sec();
    return c->id;
}

bool talons_tournament_result(talons_engine_t *t, int tournament_id,
                              int competitor_id, double score,
                              double cost, const char *result) {
    tournament_t *tr = find_tournament(t, tournament_id);
    if (!tr || competitor_id < 0 || competitor_id >= tr->competitor_count)
        return false;

    tournament_competitor_t *c = &tr->competitors[competitor_id];
    c->score = score;
    c->cost = cost;
    c->finished = true;
    c->finished_at = now_sec();
    c->speed = c->finished_at - c->started_at;
    if (result) snprintf(c->result, sizeof(c->result), "%s", result);

    /* Check if all competitors done — auto-decide if so */
    bool all_done = true;
    for (int i = 0; i < tr->competitor_count; i++) {
        if (!tr->competitors[i].finished) { all_done = false; break; }
    }
    if (all_done && !tr->decided) {
        talons_tournament_decide(t, tournament_id);
    }

    return true;
}

int talons_tournament_decide(talons_engine_t *t, int tournament_id) {
    tournament_t *tr = find_tournament(t, tournament_id);
    if (!tr || tr->decided) return tr ? tr->winner_id : -1;

    /* Score each competitor: weighted sum of quality, speed, cost */
    double best_score = -1;
    int best_id = -1;

    /* Find max values for normalization */
    double max_speed = 0.001, max_cost = 0.001;
    for (int i = 0; i < tr->competitor_count; i++) {
        if (!tr->competitors[i].finished) continue;
        if (tr->competitors[i].speed > max_speed) max_speed = tr->competitors[i].speed;
        if (tr->competitors[i].cost > max_cost) max_cost = tr->competitors[i].cost;
    }

    for (int i = 0; i < tr->competitor_count; i++) {
        tournament_competitor_t *c = &tr->competitors[i];
        if (!c->finished) continue;

        /* Quality is raw score, speed and cost are inverted (lower = better) */
        double norm_speed = 1.0 - (c->speed / max_speed);
        double norm_cost = 1.0 - (c->cost / max_cost);
        double composite = tr->weight_quality * c->score +
                          tr->weight_speed * norm_speed +
                          tr->weight_cost * norm_cost;

        if (composite > best_score) {
            best_score = composite;
            best_id = c->id;
        }
    }

    if (best_id >= 0) {
        tr->winner_id = best_id;
        tr->competitors[best_id].winner = true;
    }
    tr->decided = true;
    tr->decided_at = now_sec();
    tr->active = false;

    /* Log tournament result to VFS events */
    if (g_talons_vfs && best_id >= 0) {
        vfs_log_event(g_talons_vfs, "tournament",
                      tr->objective, tr->competitors[best_id].label);
    }

    return best_id;
}

const tournament_t *talons_tournament_get(const talons_engine_t *t,
                                           int tournament_id) {
    for (int i = 0; i < t->tournament_count; i++) {
        if (t->tournaments[i].id == tournament_id) return &t->tournaments[i];
    }
    return NULL;
}

/* ── Strategy Selection ───────────────────────────────────────────────── */

strategy_type_t talons_recommend_strategy(const talons_engine_t *t,
                                           double time_pressure,
                                           double resource_budget,
                                           double complexity) {
    if (!t) return STRATEGY_DIRECT;

    /* Score each strategy */
    double scores[6];
    for (int i = 0; i < 6; i++) {
        /* Base: historical success rate */
        double rate = t->strategy_success[i] / (double)t->strategy_attempts[i];
        scores[i] = rate;
    }

    /* Adjust for context */
    /* High time pressure: favor direct, penalize ambush/tournament */
    scores[STRATEGY_DIRECT] += time_pressure * 0.3;
    scores[STRATEGY_AMBUSH] -= time_pressure * 0.3;
    scores[STRATEGY_TOURNAMENT] -= time_pressure * 0.2;

    /* Low resources: favor direct, penalize tournament/divide */
    double resource_scarcity = 1.0 - resource_budget;
    scores[STRATEGY_DIRECT] += resource_scarcity * 0.2;
    scores[STRATEGY_TOURNAMENT] -= resource_scarcity * 0.4;
    scores[STRATEGY_DIVIDE] -= resource_scarcity * 0.2;

    /* High complexity: favor divide/tournament, penalize direct */
    scores[STRATEGY_DIVIDE] += complexity * 0.3;
    scores[STRATEGY_TOURNAMENT] += complexity * 0.2;
    scores[STRATEGY_FLANKING] += complexity * 0.15;
    scores[STRATEGY_DIRECT] -= complexity * 0.2;

    /* Pick best */
    double best = -999;
    int best_idx = 0;
    for (int i = 0; i < 6; i++) {
        if (scores[i] > best) { best = scores[i]; best_idx = i; }
    }
    return (strategy_type_t)best_idx;
}

/* ── Retry ────────────────────────────────────────────────────────────── */

bool talons_should_retry(const talons_engine_t *t, int goal_id,
                         strategy_type_t *suggested_strategy) {
    const goal_t *g = talons_goal_get(t, goal_id);
    if (!g) return false;
    if (g->state == GOAL_CAPTURED || g->state == GOAL_ABANDONED) return false;
    if (g->attempts >= g->max_attempts) return false;

    /* Suggest a different strategy than what failed */
    if (suggested_strategy) {
        /* Try the next strategy type, wrapping around */
        *suggested_strategy = (strategy_type_t)((g->strategy + 1) % 6);
        /* Or use the recommendation engine */
        double time_left = (g->deadline > 0) ? (g->deadline - now_sec()) / 60.0 : 0.5;
        double time_pressure = (g->deadline > 0) ? (1.0 - time_left) : 0.3;
        if (time_pressure < 0) time_pressure = 0;
        if (time_pressure > 1) time_pressure = 1;
        *suggested_strategy = talons_recommend_strategy(t, time_pressure, 0.5, 0.5);
    }
    return true;
}

bool talons_escalate_grip(talons_engine_t *t, int goal_id) {
    goal_t *g = find_goal(t, goal_id);
    if (!g || g->grip >= GRIP_DEATH_GRIP) return false;
    g->grip++;
    g->max_attempts = grip_max_retries(g->grip);
    return true;
}

/* ── Analytics ────────────────────────────────────────────────────────── */

double talons_win_rate(const talons_engine_t *t, int last_n) {
    if (!t || t->history_count == 0 || last_n <= 0) return 0;
    int start = t->history_count > last_n ? t->history_count - last_n : 0;
    int count = t->history_count - start;
    int wins = 0;
    for (int i = start; i < t->history_count; i++) {
        if (t->history[i].won) wins++;
    }
    return (double)wins / count;
}

strategy_type_t talons_best_strategy(const talons_engine_t *t) {
    if (!t) return STRATEGY_DIRECT;
    double best = -1;
    int best_idx = 0;
    for (int i = 0; i < 6; i++) {
        double rate = t->strategy_success[i] / (double)t->strategy_attempts[i];
        if (rate > best) { best = rate; best_idx = i; }
    }
    return (strategy_type_t)best_idx;
}

/* ── VFS Persistence ──────────────────────────────────────────────────── */

void talons_set_vfs(vfs_db_t *vfs) {
    g_talons_vfs = vfs;
}

/* Internal: persist strategy stats given engine pointer */
static void persist_strategy_from(const talons_engine_t *t, int si) {
    if (!g_talons_vfs || !t || si < 0 || si >= 6) return;
    const char *name = talons_strategy_name((strategy_type_t)si);
    double rate = t->strategy_success[si] / (double)t->strategy_attempts[si];
    char json[256];
    snprintf(json, sizeof(json),
             "{\"wins\":%.0f,\"attempts\":%d,\"rate\":%.4f}",
             t->strategy_success[si], t->strategy_attempts[si], rate);
    vfs_kv_put_str(g_talons_vfs, "talons_strategy", name, json);
}

void talons_persist_strategy_history(void) {
    if (!g_talons_vfs || !s_last_engine) return;
    for (int i = 0; i < 6; i++)
        persist_strategy_from(s_last_engine, i);
}

void talons_restore_strategy_history(talons_engine_t *t) {
    if (!g_talons_vfs || !t) return;
    for (int i = 0; i < 6; i++) {
        const char *name = talons_strategy_name((strategy_type_t)i);
        char *json = vfs_kv_get_str(g_talons_vfs, "talons_strategy", name);
        if (!json) continue;
        /* Parse wins and attempts from JSON */
        double wins = 0;
        int attempts = 0;
        if (sscanf(json, "{\"wins\":%lf,\"attempts\":%d", &wins, &attempts) == 2) {
            if (attempts > 0) {
                t->strategy_success[i] = wins;
                t->strategy_attempts[i] = attempts;
            }
        }
        free(json);
    }
}

/* ── Serialization ────────────────────────────────────────────────────── */

int talons_goal_to_json(const goal_t *g, char *buf, size_t len) {
    if (!g || !buf) return 0;
    return snprintf(buf, len,
        "{\"id\":%d,\"description\":\"%.*s\",\"state\":\"%s\","
        "\"grip\":\"%s\",\"strategy\":\"%s\","
        "\"priority\":%.2f,\"confidence\":%.3f,"
        "\"attempts\":%d,\"max_attempts\":%d,"
        "\"cost\":%.4f,\"parent\":%d,\"subs\":%d/%d}",
        g->id, 80, g->description,
        talons_goal_state_name(g->state),
        talons_grip_name(g->grip),
        talons_strategy_name(g->strategy),
        g->priority, g->confidence,
        g->attempts, g->max_attempts,
        g->total_cost, g->parent_goal_id,
        g->sub_goals_completed, g->sub_goal_count);
}

int talons_status_json(const talons_engine_t *t, char *buf, size_t len) {
    if (!t || !buf) return 0;
    int n = snprintf(buf, len,
        "{\"total_hunts\":%d,\"wins\":%d,\"losses\":%d,"
        "\"win_rate\":%.3f,\"active_goals\":%d,"
        "\"avg_attempts_to_win\":%.1f,\"avg_cost_per_win\":%.2f,"
        "\"strategy_success\":{",
        t->total_hunts, t->wins, t->losses,
        t->win_rate, t->active_goals,
        t->avg_attempts_to_win, t->avg_cost_per_win);

    for (int i = 0; i < 6; i++) {
        double rate = t->strategy_success[i] / (double)t->strategy_attempts[i];
        n += snprintf(buf + n, len - n, "%s\"%s\":%.3f",
                      i ? "," : "", talons_strategy_name((strategy_type_t)i), rate);
    }
    n += snprintf(buf + n, len - n, "}}");
    return n;
}

int talons_to_json(const talons_engine_t *t, char *buf, size_t len) {
    if (!t || !buf) return 0;
    int n = snprintf(buf, len, "{\"status\":");
    n += talons_status_json(t, buf + n, len - n);

    /* Active goals */
    n += snprintf(buf + n, len - n, ",\"active_goals\":[");
    bool first = true;
    for (int i = 0; i < t->goal_count && (size_t)n < len - 512; i++) {
        goal_state_t s = t->goals[i].state;
        if (s == GOAL_CAPTURED || s == GOAL_ESCAPED || s == GOAL_ABANDONED) continue;
        if (!first) n += snprintf(buf + n, len - n, ",");
        n += talons_goal_to_json(&t->goals[i], buf + n, len - n);
        first = false;
    }
    n += snprintf(buf + n, len - n, "]");

    /* Active tournaments */
    n += snprintf(buf + n, len - n, ",\"tournaments\":[");
    first = true;
    for (int i = 0; i < t->tournament_count && (size_t)n < len - 256; i++) {
        const tournament_t *tr = &t->tournaments[i];
        if (!tr->active && !tr->decided) continue;
        n += snprintf(buf + n, len - n,
            "%s{\"id\":%d,\"objective\":\"%.*s\",\"competitors\":%d,"
            "\"decided\":%s,\"winner\":%d}",
            first ? "" : ",", tr->id, 60, tr->objective,
            tr->competitor_count,
            tr->decided ? "true" : "false", tr->winner_id);
        first = false;
    }
    n += snprintf(buf + n, len - n, "]}");
    return n;
}
