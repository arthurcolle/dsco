#include "talons.h"
#include "vfs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
    "captured", "escaped", "abandoned", "blocked", "regrouping"
};

static const char *STRATEGY_NAMES[] = {
    "direct", "flanking", "tournament", "escalation", "divide", "ambush",
    "attrition", "pincer", "blitz", "siege", "feint", "opportunistic",
    /* military-history canon — order must match strategy_type_t */
    "envelopment", "encirclement", "guerrilla", "scorched_earth", "fabian",
    "defense_in_depth", "oblique", "infiltration", "interior_lines",
    "defeat_in_detail", "turning_movement", "breakthrough", "shock",
    "decapitation", "blockade", "raid", "indirect", "tempo", "deterrence",
    "counterattack", "maneuver", "hedgehog", "screen", "asymmetric",
    /* extended canon — order must match strategy_type_t */
    "othismos", "hammer_and_anvil", "peltast_softening", "manipular_tactics",
    "circumvallation", "castrametation", "win_without_fighting",
    "besiege_wei_to_rescue_zhao", "feigned_retreat", "tulughma", "chevauchee",
    "castellation", "fleet_in_being", "guerre_de_course", "crossing_the_t",
    "breaking_the_line", "wolfpack", "central_position", "grand_battery", "hard_war",
    "anaconda_plan", "stormtrooper_tactics", "elastic_defense", "bite_and_hold",
    "auftragstaktik", "deep_operation", "kesselschlacht", "maskirovka", "douhet_doctrine",
    "maoist_protracted_war", "clear_hold_build", "mutual_assured_destruction",
    "flexible_response", "reflexive_control", "hybrid_warfare", "network_centric_warfare"
};

static const char *GRIP_NAMES[] = {
    "tentative", "holding", "locked", "death_grip"
};

/* Name tables must stay 1:1 with their enums (ordering + count). */
_Static_assert(sizeof(GOAL_STATE_NAMES) / sizeof(GOAL_STATE_NAMES[0]) == GOAL_STATE_COUNT,
               "GOAL_STATE_NAMES out of sync with goal_state_t");
_Static_assert(sizeof(STRATEGY_NAMES) / sizeof(STRATEGY_NAMES[0]) == STRATEGY_COUNT,
               "STRATEGY_NAMES out of sync with strategy_type_t");

const char *talons_goal_state_name(goal_state_t s) {
    return (s >= 0 && s < GOAL_STATE_COUNT) ? GOAL_STATE_NAMES[s] : "unknown";
}

const char *talons_strategy_name(strategy_type_t s) {
    return (s >= 0 && s < STRATEGY_COUNT) ? STRATEGY_NAMES[s] : "unknown";
}

strategy_type_t talons_strategy_from_name(const char *name, strategy_type_t fallback) {
    if (!name || !*name) return fallback;
    for (int i = 0; i < STRATEGY_COUNT; i++)
        if (strcasecmp(name, STRATEGY_NAMES[i]) == 0) return (strategy_type_t)i;
    return fallback;
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
    for (int i = 0; i < STRATEGY_COUNT; i++) {
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
    if (si >= 0 && si < STRATEGY_COUNT) {
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
    if (g_talons_vfs && si >= 0 && si < STRATEGY_COUNT) {
        persist_strategy_from(t, si);
    }
}

static void recount_active(talons_engine_t *t) {
    t->active_goals = 0;
    for (int i = 0; i < t->goal_count; i++) {
        goal_state_t s = t->goals[i].state;
        /* Anything in flight (not yet started → NASCENT, and not terminal). */
        if (!goal_state_is_terminal(s) && s != GOAL_NASCENT)
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
    if (!g || (g->state != GOAL_NASCENT &&
               g->state != GOAL_REGROUPING && g->state != GOAL_BLOCKED)) return false;
    g->state = GOAL_STALKING;
    if (g->started_at == 0) g->started_at = now_sec();
    recount_active(t);
    return true;
}

bool talons_goal_strike(talons_engine_t *t, int goal_id) {
    goal_t *g = find_goal(t, goal_id);
    if (!g || (g->state != GOAL_STALKING && g->state != GOAL_NASCENT &&
               g->state != GOAL_REGROUPING)) return false;
    /* Hold at the gate until every prerequisite goal is captured. */
    if (!talons_goal_deps_met(t, goal_id)) {
        g->state = GOAL_BLOCKED;
        recount_active(t);
        return false;
    }
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
        /* Don't give up — regroup, then re-stalk/strike for the next attempt. */
        g->state = GOAL_REGROUPING;
        g->confidence *= 0.9;     /* slightly decrease confidence */
        recount_active(t);
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
    /* Collect active goals (everything not in a terminal end-state). */
    for (int i = 0; i < t->goal_count && n < max; i++) {
        if (!goal_state_is_terminal(t->goals[i].state))
            out[n++] = &t->goals[i];
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

/* ── Dependencies & scheduling ────────────────────────────────────────── */

bool talons_goal_depends_on(talons_engine_t *t, int goal_id, int dep_goal_id) {
    if (!t || goal_id == dep_goal_id) return false;
    goal_t *g = find_goal(t, goal_id);
    if (!g || !find_goal(t, dep_goal_id)) return false;
    if (g->dep_count >= TALONS_MAX_DEPS) return false;
    for (int i = 0; i < g->dep_count; i++)
        if (g->deps[i] == dep_goal_id) return true;  /* already a dep */
    g->deps[g->dep_count++] = dep_goal_id;
    return true;
}

bool talons_goal_deps_met(const talons_engine_t *t, int goal_id) {
    if (!t) return false;
    /* find_goal is non-const; scan directly to keep this const-correct. */
    const goal_t *g = talons_goal_get(t, goal_id);
    if (!g) return false;
    for (int i = 0; i < g->dep_count; i++) {
        const goal_t *dep = talons_goal_get(t, g->deps[i]);
        if (!dep || dep->state != GOAL_CAPTURED) return false;
    }
    return true;
}

int talons_resolve_blocked(talons_engine_t *t) {
    if (!t) return 0;
    int unblocked = 0;
    for (int i = 0; i < t->goal_count; i++) {
        goal_t *g = &t->goals[i];
        if (g->state == GOAL_BLOCKED && talons_goal_deps_met(t, g->id)) {
            g->state = GOAL_STALKING;
            if (g->started_at == 0) g->started_at = now_sec();
            unblocked++;
        }
    }
    if (unblocked) recount_active(t);
    return unblocked;
}

int talons_tick(talons_engine_t *t, double now) {
    if (!t || !t->initialized) return 0;
    if (now <= 0) now = now_sec();
    int changes = talons_resolve_blocked(t);

    for (int i = 0; i < t->goal_count; i++) {
        goal_t *g = &t->goals[i];
        if (goal_state_is_terminal(g->state) || g->deadline <= 0) continue;

        if (now >= g->deadline) {
            /* Deadline blown — force a true escape regardless of retries left. */
            g->attempts = g->max_attempts > 0 ? g->max_attempts : g->attempts;
            talons_goal_escaped(t, g->id, "deadline exceeded", 0);
            changes++;
            continue;
        }

        /* Inside the final quarter of the window: tighten grip to push harder. */
        if (g->created_at > 0 && g->grip < GRIP_DEATH_GRIP) {
            double window = g->deadline - g->created_at;
            if (window > 0 && (now - g->created_at) / window >= 0.75) {
                if (talons_escalate_grip(t, g->id)) changes++;
            }
        }
    }
    return changes;
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
    double scores[STRATEGY_COUNT];
    for (int i = 0; i < STRATEGY_COUNT; i++) {
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

    /* Attrition: cheap, relentless — best when time is plentiful but the
     * budget is tight (grind it down with many small attempts). */
    scores[STRATEGY_ATTRITION] += resource_scarcity * 0.25 + (1.0 - time_pressure) * 0.2;

    /* Pincer: run two fronts at once — worth it only with budget to spare,
     * and pays off most on complex targets. */
    scores[STRATEGY_PINCER] += complexity * 0.25 + resource_budget * 0.2;

    /* Blitz: spend everything, fast — for urgent goals you can fully fund. */
    scores[STRATEGY_BLITZ] += time_pressure * 0.3 + resource_budget * 0.2;

    /* Siege: encircle and outlast — patient, resource-heavy, beats complexity. */
    scores[STRATEGY_SIEGE] += (1.0 - time_pressure) * 0.2 + complexity * 0.15
                            + resource_budget * 0.1;

    /* Feint: misdirection — a middle-complexity play when a head-on push stalls. */
    scores[STRATEGY_FEINT] += complexity * 0.15 + (1.0 - time_pressure) * 0.1;

    /* Opportunistic: patient and cheap — wait for an opening when nothing presses. */
    scores[STRATEGY_OPPORTUNISTIC] += (1.0 - time_pressure) * 0.2 + resource_scarcity * 0.15;

    /* ── Military-history canon: bias along time / resources / complexity ──
     * Fast & resourced (urgent, well-funded pushes): */
    scores[STRATEGY_BREAKTHROUGH] += time_pressure * 0.2 + resource_budget * 0.15;
    scores[STRATEGY_SHOCK]        += time_pressure * 0.25 + resource_budget * 0.2;
    scores[STRATEGY_TEMPO]        += time_pressure * 0.3;
    /* Patient & resourced (encircle, sever, outlast): */
    scores[STRATEGY_ENCIRCLEMENT] += resource_budget * 0.2 + (1.0 - time_pressure) * 0.15;
    scores[STRATEGY_BLOCKADE]     += resource_budget * 0.1 + (1.0 - time_pressure) * 0.2;
    scores[STRATEGY_FABIAN]       += (1.0 - time_pressure) * 0.3;
    scores[STRATEGY_DETERRENCE]   += resource_budget * 0.1 + (1.0 - time_pressure) * 0.15;
    /* Cheap & asymmetric (refuse strength, fight elsewhere): */
    scores[STRATEGY_GUERRILLA]    += resource_scarcity * 0.3 + (1.0 - time_pressure) * 0.1;
    scores[STRATEGY_ASYMMETRIC]   += resource_scarcity * 0.25 + complexity * 0.15;
    scores[STRATEGY_RAID]         += resource_scarcity * 0.15 + time_pressure * 0.15;
    scores[STRATEGY_SCORCHED_EARTH] += resource_scarcity * 0.2;
    scores[STRATEGY_HEDGEHOG]     += resource_scarcity * 0.15;
    /* Skilled maneuver (beats complexity via position, not mass): */
    scores[STRATEGY_INDIRECT]     += complexity * 0.2 + (1.0 - time_pressure) * 0.1;
    scores[STRATEGY_MANEUVER]     += complexity * 0.2 + resource_budget * 0.1;
    scores[STRATEGY_ENVELOPMENT]  += complexity * 0.15 + (1.0 - time_pressure) * 0.1;
    scores[STRATEGY_TURNING_MOVEMENT] += complexity * 0.15 + resource_budget * 0.1;
    scores[STRATEGY_INFILTRATION] += complexity * 0.2 + resource_scarcity * 0.1;
    scores[STRATEGY_DECAPITATION] += complexity * 0.2 + resource_scarcity * 0.1;
    scores[STRATEGY_INTERIOR_LINES]   += complexity * 0.15;
    scores[STRATEGY_DEFEAT_IN_DETAIL] += complexity * 0.2;
    scores[STRATEGY_OBLIQUE]      += complexity * 0.1;
    scores[STRATEGY_SCREEN]       += complexity * 0.1;
    /* Defensive postures (absorb, then turn): */
    scores[STRATEGY_DEFENSE_IN_DEPTH] += complexity * 0.15 + resource_budget * 0.1;
    scores[STRATEGY_COUNTERATTACK]    += complexity * 0.1 + (1.0 - time_pressure) * 0.1;

    /* ── Extended canon: signed weights from research, centered on each axis
     * (positive weight → favored when that axis runs high, negative → low). */
    scores[STRATEGY_OTHISMOS] += +0.60*(time_pressure-0.5)*0.4 + +0.20*(resource_budget-0.5)*0.4 + -0.40*(complexity-0.5)*0.4;
    scores[STRATEGY_HAMMER_AND_ANVIL] += +0.50*(time_pressure-0.5)*0.4 + +0.40*(resource_budget-0.5)*0.4 + +0.30*(complexity-0.5)*0.4;
    scores[STRATEGY_PELTAST_SOFTENING] += +0.10*(time_pressure-0.5)*0.4 + -0.20*(resource_budget-0.5)*0.4 + +0.20*(complexity-0.5)*0.4;
    scores[STRATEGY_MANIPULAR_TACTICS] += -0.10*(time_pressure-0.5)*0.4 + +0.30*(resource_budget-0.5)*0.4 + +0.60*(complexity-0.5)*0.4;
    scores[STRATEGY_CIRCUMVALLATION] += -0.70*(time_pressure-0.5)*0.4 + +0.80*(resource_budget-0.5)*0.4 + +0.20*(complexity-0.5)*0.4;
    scores[STRATEGY_CASTRAMETATION] += -0.40*(time_pressure-0.5)*0.4 + +0.50*(resource_budget-0.5)*0.4 + +0.10*(complexity-0.5)*0.4;
    scores[STRATEGY_WIN_WITHOUT_FIGHTING] += -0.60*(time_pressure-0.5)*0.4 + -0.30*(resource_budget-0.5)*0.4 + +0.70*(complexity-0.5)*0.4;
    scores[STRATEGY_BESIEGE_WEI_TO_RESCUE_ZHAO] += +0.40*(time_pressure-0.5)*0.4 + +0.20*(resource_budget-0.5)*0.4 + +0.60*(complexity-0.5)*0.4;
    scores[STRATEGY_FEIGNED_RETREAT] += +0.50*(time_pressure-0.5)*0.4 + -0.10*(resource_budget-0.5)*0.4 + +0.30*(complexity-0.5)*0.4;
    scores[STRATEGY_TULUGHMA] += +0.60*(time_pressure-0.5)*0.4 + +0.30*(resource_budget-0.5)*0.4 + +0.40*(complexity-0.5)*0.4;
    scores[STRATEGY_CHEVAUCHEE] += +0.20*(time_pressure-0.5)*0.4 + -0.10*(resource_budget-0.5)*0.4 + -0.30*(complexity-0.5)*0.4;
    scores[STRATEGY_CASTELLATION] += -0.80*(time_pressure-0.5)*0.4 + +0.90*(resource_budget-0.5)*0.4 + +0.20*(complexity-0.5)*0.4;
    scores[STRATEGY_FLEET_IN_BEING] += -0.50*(time_pressure-0.5)*0.4 + +0.10*(resource_budget-0.5)*0.4 + +0.40*(complexity-0.5)*0.4;
    scores[STRATEGY_GUERRE_DE_COURSE] += -0.30*(time_pressure-0.5)*0.4 + -0.40*(resource_budget-0.5)*0.4 + +0.30*(complexity-0.5)*0.4;
    scores[STRATEGY_CROSSING_THE_T] += +0.70*(time_pressure-0.5)*0.4 + +0.30*(resource_budget-0.5)*0.4 + +0.20*(complexity-0.5)*0.4;
    scores[STRATEGY_BREAKING_THE_LINE] += +0.60*(time_pressure-0.5)*0.4 + +0.20*(resource_budget-0.5)*0.4 + +0.40*(complexity-0.5)*0.4;
    scores[STRATEGY_WOLFPACK] += +0.50*(time_pressure-0.5)*0.4 + +0.40*(resource_budget-0.5)*0.4 + +0.50*(complexity-0.5)*0.4;
    scores[STRATEGY_CENTRAL_POSITION] += +0.60*(time_pressure-0.5)*0.4 + -0.10*(resource_budget-0.5)*0.4 + +0.50*(complexity-0.5)*0.4;
    scores[STRATEGY_GRAND_BATTERY] += +0.30*(time_pressure-0.5)*0.4 + +0.70*(resource_budget-0.5)*0.4 + +0.10*(complexity-0.5)*0.4;
    scores[STRATEGY_HARD_WAR] += -0.20*(time_pressure-0.5)*0.4 + +0.30*(resource_budget-0.5)*0.4 + +0.20*(complexity-0.5)*0.4;
    scores[STRATEGY_ANACONDA_PLAN] += -0.80*(time_pressure-0.5)*0.4 + +0.70*(resource_budget-0.5)*0.4 + +0.30*(complexity-0.5)*0.4;
    scores[STRATEGY_STORMTROOPER_TACTICS] += +0.70*(time_pressure-0.5)*0.4 + +0.10*(resource_budget-0.5)*0.4 + +0.50*(complexity-0.5)*0.4;
    scores[STRATEGY_ELASTIC_DEFENSE] += +0.10*(time_pressure-0.5)*0.4 + +0.40*(resource_budget-0.5)*0.4 + +0.70*(complexity-0.5)*0.4;
    scores[STRATEGY_BITE_AND_HOLD] += -0.40*(time_pressure-0.5)*0.4 + +0.60*(resource_budget-0.5)*0.4 + +0.30*(complexity-0.5)*0.4;
    scores[STRATEGY_AUFTRAGSTAKTIK] += +0.80*(time_pressure-0.5)*0.4 + -0.20*(resource_budget-0.5)*0.4 + +0.80*(complexity-0.5)*0.4;
    scores[STRATEGY_DEEP_OPERATION] += +0.40*(time_pressure-0.5)*0.4 + +0.80*(resource_budget-0.5)*0.4 + +0.70*(complexity-0.5)*0.4;
    scores[STRATEGY_KESSELSCHLACHT] += +0.50*(time_pressure-0.5)*0.4 + +0.60*(resource_budget-0.5)*0.4 + +0.40*(complexity-0.5)*0.4;
    scores[STRATEGY_MASKIROVKA] += -0.30*(time_pressure-0.5)*0.4 + +0.40*(resource_budget-0.5)*0.4 + +0.90*(complexity-0.5)*0.4;
    scores[STRATEGY_DOUHET_DOCTRINE] += +0.60*(time_pressure-0.5)*0.4 + +0.80*(resource_budget-0.5)*0.4 + +0.30*(complexity-0.5)*0.4;
    scores[STRATEGY_MAOIST_PROTRACTED_WAR] += -0.90*(time_pressure-0.5)*0.4 + -0.60*(resource_budget-0.5)*0.4 + +0.50*(complexity-0.5)*0.4;
    scores[STRATEGY_CLEAR_HOLD_BUILD] += -0.70*(time_pressure-0.5)*0.4 + +0.80*(resource_budget-0.5)*0.4 + +0.70*(complexity-0.5)*0.4;
    scores[STRATEGY_MUTUAL_ASSURED_DESTRUCTION] += -0.90*(time_pressure-0.5)*0.4 + +0.90*(resource_budget-0.5)*0.4 + +0.40*(complexity-0.5)*0.4;
    scores[STRATEGY_FLEXIBLE_RESPONSE] += +0.20*(time_pressure-0.5)*0.4 + +0.70*(resource_budget-0.5)*0.4 + +0.80*(complexity-0.5)*0.4;
    scores[STRATEGY_REFLEXIVE_CONTROL] += -0.20*(time_pressure-0.5)*0.4 + -0.10*(resource_budget-0.5)*0.4 + +0.90*(complexity-0.5)*0.4;
    scores[STRATEGY_HYBRID_WARFARE] += +0.10*(time_pressure-0.5)*0.4 + +0.60*(resource_budget-0.5)*0.4 + +0.90*(complexity-0.5)*0.4;
    scores[STRATEGY_NETWORK_CENTRIC_WARFARE] += +0.70*(time_pressure-0.5)*0.4 + +0.90*(resource_budget-0.5)*0.4 + +0.80*(complexity-0.5)*0.4;

    /* Pick best */
    double best = -999;
    int best_idx = 0;
    for (int i = 0; i < STRATEGY_COUNT; i++) {
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
        *suggested_strategy = (strategy_type_t)((g->strategy + 1) % STRATEGY_COUNT);
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
    for (int i = 0; i < STRATEGY_COUNT; i++) {
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
    if (!g_talons_vfs || !t || si < 0 || si >= STRATEGY_COUNT) return;
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
    for (int i = 0; i < STRATEGY_COUNT; i++)
        persist_strategy_from(s_last_engine, i);
}

void talons_restore_strategy_history(talons_engine_t *t) {
    if (!g_talons_vfs || !t) return;
    for (int i = 0; i < STRATEGY_COUNT; i++) {
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
    int n = snprintf(buf, len,
        "{\"id\":%d,\"description\":\"%.*s\",\"state\":\"%s\","
        "\"grip\":\"%s\",\"strategy\":\"%s\","
        "\"priority\":%.2f,\"confidence\":%.3f,"
        "\"attempts\":%d,\"max_attempts\":%d,"
        "\"cost\":%.4f,\"parent\":%d,\"subs\":%d/%d,\"deps\":[",
        g->id, 80, g->description,
        talons_goal_state_name(g->state),
        talons_grip_name(g->grip),
        talons_strategy_name(g->strategy),
        g->priority, g->confidence,
        g->attempts, g->max_attempts,
        g->total_cost, g->parent_goal_id,
        g->sub_goals_completed, g->sub_goal_count);
    for (int i = 0; i < g->dep_count && n > 0 && (size_t)n < len; i++)
        n += snprintf(buf + n, len - (size_t)n, "%s%d", i ? "," : "", g->deps[i]);
    if (n > 0 && (size_t)n < len) n += snprintf(buf + n, len - (size_t)n, "]}");
    return n;
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

    for (int i = 0; i < STRATEGY_COUNT; i++) {
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
