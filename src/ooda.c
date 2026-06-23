#include "ooda.h"
#include "scheduler.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * OODA Loop Discipline — Implementation
 *
 * Every significant agent action passes through Observe → Orient →
 * Decide → Act. The OODA engine enforces structured decision-making
 * and maintains a history for learning.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── Scheduler integration (§1/§7) ───────────────────────────────────── */

static scheduler_t *g_ooda_sched = NULL;

void ooda_set_scheduler(scheduler_t *sched) {
    g_ooda_sched = sched;
}

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* ── Name Tables ──────────────────────────────────────────────────────── */

static const char *PHASE_NAMES[] = {"idle", "observe", "orient", "decide", "act", "complete"};

static const char *ACTION_NAMES[] = {"EXECUTE", "DELEGATE", "WAIT", "REST", "ESCALATE"};

static const char *CAPABILITY_NAMES[] = {"EXPERT", "PROFICIENT", "COMPETENT", "NOVICE"};

const char *ooda_phase_name(ooda_phase_t p) {
    return (p >= 0 && p <= OODA_PHASE_COMPLETE) ? PHASE_NAMES[p] : "unknown";
}

const char *ooda_action_name(ooda_action_t a) {
    return (a >= 0 && a <= OODA_ACTION_ESCALATE) ? ACTION_NAMES[a] : "unknown";
}

const char *ooda_capability_name(capability_tier_t t) {
    return (t >= 0 && t <= CAPABILITY_NOVICE) ? CAPABILITY_NAMES[t] : "unknown";
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

void ooda_engine_init(ooda_engine_t *e) {
    memset(e, 0, sizeof(*e));
    e->execute_threshold = 0.8;
    e->delegate_threshold = 0.6;
    e->escalate_threshold = 0.3;
    e->max_cycle_ms = 30000; /* 30 seconds default */
    e->initialized = true;
}

/* ── Cycle Operations ─────────────────────────────────────────────────── */

int ooda_begin(ooda_engine_t *e) {
    if (!e || !e->initialized)
        return -1;

    memset(&e->current, 0, sizeof(e->current));
    e->current.id = e->next_cycle_id++;
    e->current.phase = OODA_PHASE_OBSERVE;
    e->current.started_at = now_sec();
    e->current.phase_started_at = e->current.started_at;
    e->current.budget_remaining = -1; /* unset */
    e->current.principal_tier = 2;    /* default: agent tier */

    return e->current.id;
}

bool ooda_observe(ooda_engine_t *e, const char *content, const char *source, double confidence) {
    if (!e || !e->initialized)
        return false;
    if (e->current.phase != OODA_PHASE_OBSERVE)
        return false;
    if (e->current.observation_count >= OODA_MAX_OBSERVATIONS)
        return false;

    ooda_observation_t *obs = &e->current.observations[e->current.observation_count++];
    snprintf(obs->content, sizeof(obs->content), "%s", content ? content : "");
    snprintf(obs->source, sizeof(obs->source), "%s", source ? source : "");
    obs->confidence = confidence;
    obs->timestamp = now_sec();

    return true;
}

bool ooda_orient_add(ooda_engine_t *e, const char *factor, double weight, bool is_constraint) {
    if (!e || !e->initialized)
        return false;
    /* Auto-advance from observe to orient */
    if (e->current.phase == OODA_PHASE_OBSERVE) {
        e->current.phase = OODA_PHASE_ORIENT;
        e->current.phase_started_at = now_sec();
    }
    if (e->current.phase != OODA_PHASE_ORIENT)
        return false;
    if (e->current.factor_count >= OODA_MAX_FACTORS)
        return false;

    ooda_orientation_factor_t *f = &e->current.factors[e->current.factor_count++];
    snprintf(f->factor, sizeof(f->factor), "%s", factor ? factor : "");
    f->weight = weight;
    f->is_constraint = is_constraint;

    return true;
}

bool ooda_orient_context(ooda_engine_t *e, double budget_remaining, int principal_tier,
                         bool safety_critical) {
    if (!e || !e->initialized)
        return false;
    if (e->current.phase == OODA_PHASE_OBSERVE) {
        e->current.phase = OODA_PHASE_ORIENT;
        e->current.phase_started_at = now_sec();
    }
    if (e->current.phase != OODA_PHASE_ORIENT)
        return false;

    e->current.budget_remaining = budget_remaining;
    e->current.principal_tier = principal_tier;
    e->current.safety_critical = safety_critical;

    return true;
}

ooda_action_t ooda_decide(ooda_engine_t *e) {
    if (!e || !e->initialized)
        return OODA_ACTION_ESCALATE;

    /* Auto-advance to decide phase */
    if (e->current.phase == OODA_PHASE_OBSERVE || e->current.phase == OODA_PHASE_ORIENT) {
        e->current.phase = OODA_PHASE_DECIDE;
        e->current.phase_started_at = now_sec();
    }
    if (e->current.phase != OODA_PHASE_DECIDE)
        return OODA_ACTION_ESCALATE;

    ooda_decision_t *d = &e->current.decision;
    memset(d, 0, sizeof(*d));

    /* Compute aggregate confidence from observations */
    double total_conf = 0;
    double total_weight = 0;
    for (int i = 0; i < e->current.observation_count; i++) {
        total_conf += e->current.observations[i].confidence;
        total_weight += 1.0;
    }
    double obs_confidence = total_weight > 0 ? total_conf / total_weight : 0.5;

    /* Apply orientation factors */
    double constraint_penalty = 0;
    double factor_boost = 0;
    for (int i = 0; i < e->current.factor_count; i++) {
        ooda_orientation_factor_t *f = &e->current.factors[i];
        if (f->is_constraint) {
            constraint_penalty += (1.0 - f->weight) * 0.2;
        } else {
            factor_boost += f->weight * 0.1;
        }
    }

    d->confidence = obs_confidence + factor_boost - constraint_penalty;
    if (d->confidence > 1.0)
        d->confidence = 1.0;
    if (d->confidence < 0.0)
        d->confidence = 0.0;

    /* Safety-critical tasks get more conservative thresholds */
    double exec_thresh = e->execute_threshold;
    double deleg_thresh = e->delegate_threshold;
    double esc_thresh = e->escalate_threshold;
    if (e->current.safety_critical) {
        exec_thresh += 0.1;
        deleg_thresh += 0.1;
        esc_thresh += 0.1;
    }

    /* Budget check */
    if (e->current.budget_remaining >= 0 && e->current.budget_remaining < 1.0) {
        d->action = OODA_ACTION_REST;
        d->capability = CAPABILITY_NOVICE;
        snprintf(d->reason, sizeof(d->reason), "Budget exhausted (%.2f GSU remaining)",
                 e->current.budget_remaining);
        goto done;
    }

    /* Decision based on confidence thresholds */
    if (d->confidence >= exec_thresh) {
        d->action = OODA_ACTION_EXECUTE;
        d->capability = CAPABILITY_EXPERT;
        snprintf(d->reason, sizeof(d->reason),
                 "High confidence (%.2f >= %.2f), proceeding with execution", d->confidence,
                 exec_thresh);
    } else if (d->confidence >= deleg_thresh) {
        d->action = OODA_ACTION_EXECUTE;
        d->capability = CAPABILITY_PROFICIENT;
        d->requires_confirmation = true;
        snprintf(d->reason, sizeof(d->reason),
                 "Moderate confidence (%.2f >= %.2f), executing with monitoring", d->confidence,
                 deleg_thresh);
    } else if (d->confidence >= esc_thresh) {
        d->action = OODA_ACTION_DELEGATE;
        d->capability = CAPABILITY_COMPETENT;
        snprintf(d->reason, sizeof(d->reason),
                 "Low confidence (%.2f), delegating to more capable agent", d->confidence);
    } else {
        d->action = OODA_ACTION_ESCALATE;
        d->capability = CAPABILITY_NOVICE;
        snprintf(d->reason, sizeof(d->reason),
                 "Very low confidence (%.2f < %.2f), escalating to human", d->confidence,
                 esc_thresh);
    }

done:
    e->current.phase = OODA_PHASE_ACT;
    e->current.phase_started_at = now_sec();
    return d->action;
}

bool ooda_act_result(ooda_engine_t *e, bool success, const char *result) {
    if (!e || !e->initialized)
        return false;
    if (e->current.phase != OODA_PHASE_ACT)
        return false;

    e->current.action_taken = true;
    e->current.action_success = success;
    if (result)
        snprintf(e->current.action_result, sizeof(e->current.action_result), "%s", result);

    return true;
}

bool ooda_complete(ooda_engine_t *e) {
    if (!e || !e->initialized)
        return false;
    if (e->current.phase == OODA_PHASE_IDLE || e->current.phase == OODA_PHASE_COMPLETE)
        return false;

    double end_time = now_sec();
    e->current.phase = OODA_PHASE_COMPLETE;

    /* Archive to history */
    if (e->history_count < OODA_MAX_HISTORY) {
        ooda_history_entry_t *h = &e->history[e->history_count++];
        h->cycle_id = e->current.id;
        h->action = e->current.decision.action;
        h->confidence = e->current.decision.confidence;
        h->success = e->current.action_success;
        h->duration_ms = (end_time - e->current.started_at) * 1000;
        h->resource_cost = e->current.decision.resource_cost;
        h->timestamp = end_time;
    }

    /* Update statistics */
    e->total_cycles++;
    switch (e->current.decision.action) {
        case OODA_ACTION_EXECUTE:
            e->execute_count++;
            break;
        case OODA_ACTION_DELEGATE:
            e->delegate_count++;
            break;
        case OODA_ACTION_WAIT:
            e->wait_count++;
            break;
        case OODA_ACTION_REST:
            e->rest_count++;
            break;
        case OODA_ACTION_ESCALATE:
            e->escalate_count++;
            break;
    }

    /* Update running average confidence */
    double n = (double)e->total_cycles;
    e->avg_confidence = ((n - 1) * e->avg_confidence + e->current.decision.confidence) / n;

    return true;
}

void ooda_abort(ooda_engine_t *e) {
    if (!e || !e->initialized)
        return;
    e->current.phase = OODA_PHASE_IDLE;
}

/* ── Query ────────────────────────────────────────────────────────────── */

ooda_phase_t ooda_current_phase(const ooda_engine_t *e) {
    return e ? e->current.phase : OODA_PHASE_IDLE;
}

const ooda_decision_t *ooda_current_decision(const ooda_engine_t *e) {
    if (!e || e->current.phase < OODA_PHASE_ACT)
        return NULL;
    return &e->current.decision;
}

int ooda_recent_history(const ooda_engine_t *e, ooda_history_entry_t *out, int max) {
    if (!e || !out || max <= 0)
        return 0;
    int start = e->history_count > max ? e->history_count - max : 0;
    int count = e->history_count - start;
    for (int i = 0; i < count; i++)
        out[i] = e->history[start + i];
    return count;
}

double ooda_success_rate(const ooda_engine_t *e, int last_n) {
    if (!e || e->history_count == 0 || last_n <= 0)
        return 0.0;
    int start = e->history_count > last_n ? e->history_count - last_n : 0;
    int count = e->history_count - start;
    int success = 0;
    for (int i = start; i < e->history_count; i++) {
        if (e->history[i].success)
            success++;
    }
    return (double)success / count;
}

/* ── Serialization ────────────────────────────────────────────────────── */

int ooda_cycle_to_json(const ooda_cycle_t *c, char *buf, size_t len) {
    if (!c || !buf)
        return 0;
    return snprintf(buf, len,
                    "{\"id\":%d,\"phase\":\"%s\",\"observations\":%d,"
                    "\"factors\":%d,\"tier\":%d,\"safety_critical\":%s,"
                    "\"decision\":{\"action\":\"%s\",\"confidence\":%.3f,"
                    "\"capability\":\"%s\",\"reason\":\"%s\"},"
                    "\"action_taken\":%s,\"action_success\":%s}",
                    c->id, ooda_phase_name(c->phase), c->observation_count, c->factor_count,
                    c->principal_tier, c->safety_critical ? "true" : "false",
                    ooda_action_name(c->decision.action), c->decision.confidence,
                    ooda_capability_name(c->decision.capability), c->decision.reason,
                    c->action_taken ? "true" : "false", c->action_success ? "true" : "false");
}

int ooda_to_json(const ooda_engine_t *e, char *buf, size_t len) {
    if (!e || !buf)
        return 0;
    int n = snprintf(buf, len,
                     "{\"total_cycles\":%d,\"avg_confidence\":%.3f,"
                     "\"actions\":{\"execute\":%d,\"delegate\":%d,"
                     "\"wait\":%d,\"rest\":%d,\"escalate\":%d},"
                     "\"success_rate_10\":%.3f,"
                     "\"thresholds\":{\"execute\":%.2f,\"delegate\":%.2f,\"escalate\":%.2f},"
                     "\"current\":",
                     e->total_cycles, e->avg_confidence, e->execute_count, e->delegate_count,
                     e->wait_count, e->rest_count, e->escalate_count, ooda_success_rate(e, 10),
                     e->execute_threshold, e->delegate_threshold, e->escalate_threshold);

    n += ooda_cycle_to_json(&e->current, buf + n, len - n);
    n += snprintf(buf + n, len - n, "}");
    return n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Scheduler Task Adapters (§1/§7)
 *
 * Each OODA phase is modeled as a cooperative scheduler task.
 * ooda_run_scheduled() chains them sequentially using sched_wait_task,
 * allowing the scheduler to interleave other work between phases.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Context passed to each phase task */
typedef struct {
    ooda_engine_t *engine;
    ooda_action_t decided_action; /* set by decide task */
    int cycle_id;
} ooda_sched_ctx_t;

static int ooda_observe_task(void *ctx) {
    ooda_sched_ctx_t *sc = (ooda_sched_ctx_t *)ctx;
    if (!sc || !sc->engine)
        return -1;

    ooda_engine_t *e = sc->engine;
    /* The cycle should already be in OBSERVE phase from ooda_begin().
       If observations were pre-loaded before scheduling, this is a no-op
       transition; otherwise callers added observations before spawning. */
    if (e->current.phase != OODA_PHASE_OBSERVE)
        return -1;

    /* Advance to orient phase when observe completes */
    e->current.phase = OODA_PHASE_ORIENT;
    e->current.phase_started_at = now_sec();
    return 0;
}

static int ooda_orient_task(void *ctx) {
    ooda_sched_ctx_t *sc = (ooda_sched_ctx_t *)ctx;
    if (!sc || !sc->engine)
        return -1;

    ooda_engine_t *e = sc->engine;
    if (e->current.phase != OODA_PHASE_ORIENT)
        return -1;

    /* Orientation factors should already be loaded; advance to decide */
    e->current.phase = OODA_PHASE_DECIDE;
    e->current.phase_started_at = now_sec();
    return 0;
}

static int ooda_decide_task(void *ctx) {
    ooda_sched_ctx_t *sc = (ooda_sched_ctx_t *)ctx;
    if (!sc || !sc->engine)
        return -1;

    /* ooda_decide auto-advances to ACT and returns the chosen action */
    sc->decided_action = ooda_decide(sc->engine);
    return 0;
}

static int ooda_act_task(void *ctx) {
    ooda_sched_ctx_t *sc = (ooda_sched_ctx_t *)ctx;
    if (!sc || !sc->engine)
        return -1;

    ooda_engine_t *e = sc->engine;
    if (e->current.phase != OODA_PHASE_ACT)
        return -1;

    /* Record that the action phase ran; actual execution is up to the caller.
       Mark success=true as default; callers can override via ooda_act_result. */
    ooda_act_result(e, true, "scheduled execution");
    ooda_complete(e);
    return 0;
}

int ooda_run_scheduled(ooda_cycle_t *cycle, scheduler_t *sched) {
    if (!cycle || !sched)
        return -1;

    /* Use the cycle's engine — the cycle is always &engine->current,
       so we recover the engine by container-of arithmetic. */
    ooda_engine_t *engine = (ooda_engine_t *)((char *)cycle - offsetof(ooda_engine_t, current));

    static ooda_sched_ctx_t sctx; /* static — lives beyond sched_run */
    sctx.engine = engine;
    sctx.decided_action = OODA_ACTION_ESCALATE;
    sctx.cycle_id = cycle->id;

    /* Spawn each phase as a high-priority task, chained sequentially */
    task_id_t t_observe =
        sched_spawn(sched, ooda_observe_task, &sctx, "ooda:observe", SCHED_PRIO_HIGH);
    task_id_t t_orient =
        sched_spawn(sched, ooda_orient_task, &sctx, "ooda:orient", SCHED_PRIO_HIGH);
    task_id_t t_decide =
        sched_spawn(sched, ooda_decide_task, &sctx, "ooda:decide", SCHED_PRIO_HIGH);
    task_id_t t_act = sched_spawn(sched, ooda_act_task, &sctx, "ooda:act", SCHED_PRIO_HIGH);

    if (t_observe == TASK_INVALID || t_orient == TASK_INVALID || t_decide == TASK_INVALID ||
        t_act == TASK_INVALID) {
        return -1;
    }

    /* Chain: orient waits for observe, decide waits for orient, act waits for decide */
    sched_task_t *orient_task = sched_task_get(sched, t_orient);
    if (orient_task) {
        orient_task->wait_task = t_observe;
        orient_task->state = TASK_WAITING_TASK;
    }

    sched_task_t *decide_task = sched_task_get(sched, t_decide);
    if (decide_task) {
        decide_task->wait_task = t_orient;
        decide_task->state = TASK_WAITING_TASK;
    }

    sched_task_t *act_task_p = sched_task_get(sched, t_act);
    if (act_task_p) {
        act_task_p->wait_task = t_decide;
        act_task_p->state = TASK_WAITING_TASK;
    }

    /* Run scheduler until all four tasks complete */
    sched_run(sched, 10);

    return (int)sctx.decided_action;
}
