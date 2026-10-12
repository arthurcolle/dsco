/* Mechanical work dispatcher. See include/work_dispatch.h. */

#include "work_dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tools.h"
#include "work_result.h"
#include "work_unit.h"

/* A dispatched tool result can be large (diffs); this is the tool-side buffer,
 * not what lands on the task (result_json is capped by the task field). */
#define WD_RESULT_MAX (256 * 1024)

static bool wd_deps_clear(const goal_queue_t *q, const work_unit_t *wu);

static const goal_queue_task_t *wd_current(const goal_queue_t *q) {
    if (!q || !q->initialized || q->current_id <= 0)
        return NULL;
    for (int i = 0; i < q->task_count; i++)
        if (q->tasks[i].id == q->current_id)
            return &q->tasks[i];
    return NULL;
}

bool work_dispatch_can_execute(const goal_queue_t *q) {
    const goal_queue_task_t *t = wd_current(q);
    if (!t || t->lane != GOAL_QUEUE_LANE_WORK || !t->work_unit_json[0])
        return false;
    work_unit_t wu;
    bool ok = work_unit_from_json(&wu, t->work_unit_json);
    bool mech = ok && work_dispatch_unit_is_mechanical(&wu);
    work_unit_free(&wu);
    return mech;
}

/* Execute one tool-kind unit on the leased task. Returns the post-delivery
 * status. */
static goal_queue_task_status_t wd_execute_leased(goal_queue_t *q, const char *tier,
                                                  work_dispatch_stats_t *stats) {
    const goal_queue_task_t *t = wd_current(q);
    if (!t)
        return GOAL_QUEUE_TASK_UNUSED;
    int task_id = t->id;

    work_unit_t wu;
    if (!work_unit_from_json(&wu, t->work_unit_json)) {
        work_unit_free(&wu);
        return goal_queue_deliver_result(
            q, task_id, "{\"kind\":\"error\",\"ok\":false,\"error_code\":\"bad_work_unit\"}",
            false);
    }

    char *result = malloc(WD_RESULT_MAX);
    if (!result) {
        work_unit_free(&wu);
        return goal_queue_deliver_result(
            q, task_id, "{\"kind\":\"error\",\"ok\":false,\"error_code\":\"oom\"}", false);
    }
    result[0] = '\0';

    /* Leased mechanical work must respect the same dependency contract as
     * fan-out; otherwise head-of-queue execution bypasses its prerequisite. */
    if (!wd_deps_clear(q, &wu)) {
        free(result);
        work_unit_free(&wu);
        return GOAL_QUEUE_TASK_ACTIVE;
    }

    /* Through the gate, never around it. Governance applies to mechanical
     * dispatch exactly as it does to model-issued calls. */
    work_result_clear_pending();
    bool ok = tools_execute_for_tier(wu.tool, wu.payload ? wu.payload : "{}", tier, result,
                                     WD_RESULT_MAX);
    stats->executed++;

    /* Prefer the typed result the tool published; synthesize one otherwise so
     * the task always records structure. */
    char *result_json = NULL;
    work_result_t wr;
    if (work_result_take(&wr)) {
        result_json = work_result_to_json(&wr);
        work_result_free(&wr);
    } else {
        work_result_t synth;
        work_result_init(&synth, ok ? WORK_RESULT_TEXT : WORK_RESULT_ERROR);
        synth.ok = ok;
        if (!ok) {
            synth.error_code = strdup("tool_failed");
            /* First line of the tool error is the detail; the full text can be
             * huge and belongs to the model path, not the ledger. */
            char *nl = strchr(result, '\n');
            if (nl)
                *nl = '\0';
            synth.error_detail = strdup(result);
        } else {
            /* Truncate the rendered text to what the task field can hold. */
            if (strlen(result) >= 700)
                result[700] = '\0';
            work_result_set_text(&synth, result);
        }
        result_json = work_result_to_json(&synth);
        work_result_free(&synth);
    }
    free(result);
    work_unit_free(&wu);

    goal_queue_task_status_t st = goal_queue_deliver_result(q, task_id, result_json, ok);
    free(result_json);
    return st;
}

work_dispatch_stats_t work_dispatch_drain(goal_queue_t *q, const char *tier) {
    work_dispatch_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    if (!q || !q->initialized)
        return stats;

    /* Each iteration: if the leased task is mechanical, run it; the delivery
     * releases the lease, so prepare() surfaces the next task. Stop at the
     * first task that needs inference, leaving it leased for the prompt path.
     * The queue's task cap bounds this loop structurally, but guard anyway. */
    for (int guard = 0; guard < GOAL_QUEUE_MAX_TASKS * 4; guard++) {
        if (q->current_id <= 0 && !goal_queue_prepare(q))
            break; /* queue empty */
        if (!work_dispatch_can_execute(q)) {
            stats.model_needed = wd_current(q) != NULL;
            break;
        }
        goal_queue_task_status_t st = wd_execute_leased(q, tier, &stats);
        if (st == GOAL_QUEUE_TASK_COMPLETE)
            stats.completed++;
        else if (st == GOAL_QUEUE_TASK_QUEUED)
            stats.requeued++;
        else if (st == GOAL_QUEUE_TASK_FAILED)
            stats.failed++;
        else if (st == GOAL_QUEUE_TASK_ACTIVE) {
            stats.model_needed = true; /* unresolved dependency; no busy loop */
            break;
        }
        /* current_id was released by delivery; loop to lease the next. */
    }
    return stats;
}

/* ── Fan-out ────────────────────────────────────────────────────────────── */

#include "swarm.h"

/* The swarm instance lives in tools.c; expose it for worker fan-out. */
extern swarm_t *tools_swarm_handle(void);

/* A task is "satisfied" for dependency purposes only when complete. */
static bool wd_dep_exists(const goal_queue_t *q, const char *dep_id) {
    for (int i = 0; i < q->task_count; i++)
        if (q->tasks[i].work_unit_id[0] &&
            strcmp(q->tasks[i].work_unit_id, dep_id) == 0)
            return true;
    return false;
}

static bool wd_dep_satisfied(const goal_queue_t *q, const char *dep_id) {
    for (int i = 0; i < q->task_count; i++) {
        if (q->tasks[i].work_unit_id[0] &&
            strcmp(q->tasks[i].work_unit_id, dep_id) == 0)
            return q->tasks[i].status == GOAL_QUEUE_TASK_COMPLETE;
    }
    /* A missing dependency is not evidence that its prerequisite completed. */
    return false;
}

static bool wd_deps_clear(const goal_queue_t *q, const work_unit_t *wu) {
    for (size_t i = 0; i < wu->depends_count; i++)
        if (!wd_dep_satisfied(q, wu->depends_on[i]))
            return false;
    return true;
}

/* Deliver one finished swarm worker's output as the typed result of its task. */
static void wd_deliver_worker(goal_queue_t *q, int task_id, swarm_child_t *child,
                              work_fanout_stats_t *st) {
    bool ok = child->status == SWARM_DONE;
    work_result_t wr;
    work_result_init(&wr, ok ? WORK_RESULT_TEXT : WORK_RESULT_ERROR);
    wr.ok = ok;
    if (child->output && child->output_len) {
        /* Tail is the interesting part of a worker transcript; the task field
         * caps what we keep. */
        const char *tail = child->output;
        if (child->output_len > 600)
            tail = child->output + child->output_len - 600;
        work_result_set_text(&wr, tail);
    }
    if (!ok) {
        wr.error_code = strdup("worker_failed");
    }
    if (child->reported_cost_usd > 0)
        wr.cost_usd = child->reported_cost_usd;
    char *json = work_result_to_json(&wr);
    work_result_free(&wr);

    goal_queue_task_status_t s = goal_queue_deliver_result(q, task_id, json, ok);
    free(json);
    for (int i = 0; i < q->task_count; i++)
        if (q->tasks[i].id == task_id) {
            q->tasks[i].worker_child_id_plus_one = 0;
            break;
        }
    st->workers_done++;
    if (s == GOAL_QUEUE_TASK_COMPLETE)
        st->completed++;
    else if (s == GOAL_QUEUE_TASK_QUEUED)
        st->requeued++;
    else if (s == GOAL_QUEUE_TASK_FAILED)
        st->failed++;
}

work_fanout_stats_t work_dispatch_fanout(goal_queue_t *q, const char *tier,
                                         int max_workers, int worker_timeout_ms) {
    work_fanout_stats_t st;
    memset(&st, 0, sizeof(st));
    if (!q || !q->initialized)
        return st;
    if (max_workers <= 0)
        max_workers = 32; /* frontier-model batches parallelize hard; the real
                           * ceilings are provider rate limits and the swarm's
                           * child cap, both enforced downstream. */

    swarm_t *sw = tools_swarm_handle();

    /* Rebuild in-flight associations from the goal tree on every call.
     * Worker IDs are process-local and ACTIVE tasks requeue on reload. */
    int inflight_task[SWARM_MAX_CHILDREN];
    int inflight_child[SWARM_MAX_CHILDREN];
    int inflight = 0;
    if (max_workers > SWARM_MAX_CHILDREN) max_workers = SWARM_MAX_CHILDREN;
    for (int i = 0; i < q->task_count; i++) {
        goal_queue_task_t *t = &q->tasks[i];
        if (t->status != GOAL_QUEUE_TASK_ACTIVE || !t->worker_child_id_plus_one)
            continue;
        int cid = t->worker_child_id_plus_one - 1;
        if (!sw || !swarm_get(sw, cid)) {
            t->worker_child_id_plus_one = 0;
            work_result_t lost;
            work_result_init(&lost, WORK_RESULT_ERROR);
            lost.ok = false;
            lost.error_code = strdup("worker_lost");
            char *json = work_result_to_json(&lost);
            work_result_free(&lost);
            goal_queue_task_status_t s = goal_queue_deliver_result(q, t->id, json, false);
            free(json);
            if (s == GOAL_QUEUE_TASK_QUEUED) st.requeued++;
            else if (s == GOAL_QUEUE_TASK_FAILED) st.failed++;
            continue;
        }
        if (inflight < SWARM_MAX_CHILDREN) {
            inflight_task[inflight] = t->id;
            inflight_child[inflight++] = cid;
        }
    }

    bool progressed = true;
    while (progressed) {
        progressed = false;

        /* Wave: scan every queued work-lane task (not just the leased head —
         * that head-of-line blocking is exactly what kept fan-out serial). */
        for (int i = 0; i < q->task_count; i++) {
            goal_queue_task_t *t = &q->tasks[i];
            if (t->lane != GOAL_QUEUE_LANE_WORK ||
                t->status != GOAL_QUEUE_TASK_QUEUED || !t->work_unit_json[0])
                continue;

            work_unit_t wu;
            if (!work_unit_from_json(&wu, t->work_unit_json)) {
                work_unit_free(&wu);
                continue;
            }
            if (!wd_deps_clear(q, &wu)) {
                st.deferred_deps++;
                work_unit_free(&wu);
                continue;
            }

            if (wu.kind == WORK_UNIT_TOOL && wu.tool && wu.tool[0]) {
                /* Mechanical: run in-process now. Mark active so the delivery
                 * transition is legal, mirroring a lease. */
                t->status = GOAL_QUEUE_TASK_ACTIVE;
                t->attempts++;
                int task_id = t->id;

                char *result = malloc(WD_RESULT_MAX);
                if (result) {
                    result[0] = '\0';
                    work_result_clear_pending();
                    bool ok = tools_execute_for_tier(wu.tool, wu.payload ? wu.payload : "{}",
                                                     tier, result, WD_RESULT_MAX);
                    st.tool_executed++;
                    char *json = NULL;
                    work_result_t wr;
                    if (work_result_take(&wr)) {
                        json = work_result_to_json(&wr);
                        work_result_free(&wr);
                    } else {
                        work_result_t synth;
                        work_result_init(&synth, ok ? WORK_RESULT_TEXT : WORK_RESULT_ERROR);
                        synth.ok = ok;
                        if (strlen(result) >= 700)
                            result[700] = '\0';
                        if (ok)
                            work_result_set_text(&synth, result);
                        else
                            synth.error_detail = strdup(result);
                        json = work_result_to_json(&synth);
                        work_result_free(&synth);
                    }
                    free(result);
                    goal_queue_task_status_t s2 =
                        goal_queue_deliver_result(q, task_id, json, ok);
                    free(json);
                    if (s2 == GOAL_QUEUE_TASK_COMPLETE)
                        st.completed++;
                    else if (s2 == GOAL_QUEUE_TASK_QUEUED)
                        st.requeued++;
                    else if (s2 == GOAL_QUEUE_TASK_FAILED)
                        st.failed++;
                    progressed = true; /* completions may clear deps */
                }
            } else if (wu.kind == WORK_UNIT_PROMPT && sw &&
                       (wu.provider || wu.model) && inflight < max_workers) {
                /* Inference work with its own routing: fan out as a worker.
                 * Per-unit persona/temperature ride the spawn instance. */
                swarm_set_next_instance(wu.effort, wu.temperature, wu.top_p, wu.top_k,
                                        wu.thinking_budget, wu.tool_choice,
                                        wu.system_prompt, wu.max_turns > 0 ? wu.max_turns : 8);
                if (wu.cwd)
                    swarm_set_next_cwd(wu.cwd);
                if (wu.max_cost_usd > 0)
                    swarm_set_next_budget_usd(wu.max_cost_usd);
                int cid = swarm_spawn_provider(sw, -1, wu.payload ? wu.payload : "",
                                               wu.model, wu.provider);
                if (cid >= 0) {
                    t->status = GOAL_QUEUE_TASK_ACTIVE;
                    t->attempts++;
                    t->worker_child_id_plus_one = cid + 1;
                    inflight_task[inflight] = t->id;
                    inflight_child[inflight] = cid;
                    inflight++;
                    st.workers_spawned++;
                    progressed = true;
                }
            }
            work_unit_free(&wu);
        }

        /* Collect any workers that already finished; their completions can
         * unblock the next wave's dependencies. */
        if (inflight > 0 && sw) {
            swarm_poll(sw, 0);
            for (int k = 0; k < inflight;) {
                swarm_child_t *c = swarm_get(sw, inflight_child[k]);
                if (c && (c->status == SWARM_DONE || c->status == SWARM_ERROR ||
                          c->status == SWARM_KILLED)) {
                    wd_deliver_worker(q, inflight_task[k], c, &st);
                    inflight_task[k] = inflight_task[inflight - 1];
                    inflight_child[k] = inflight_child[inflight - 1];
                    inflight--;
                    progressed = true;
                } else {
                    k++;
                }
            }
        }
    }

    /* Final wave done; optionally wait for stragglers. */
    long long waited = 0;
    while (inflight > 0 && sw && waited < worker_timeout_ms) {
        int slice = worker_timeout_ms - (int)waited < 250 ? worker_timeout_ms - (int)waited : 250;
        swarm_poll(sw, slice);
        waited += slice;
        for (int k = 0; k < inflight;) {
            swarm_child_t *c = swarm_get(sw, inflight_child[k]);
            if (c && (c->status == SWARM_DONE || c->status == SWARM_ERROR ||
                      c->status == SWARM_KILLED)) {
                wd_deliver_worker(q, inflight_task[k], c, &st);
                inflight_task[k] = inflight_task[inflight - 1];
                inflight_child[k] = inflight_child[inflight - 1];
                inflight--;
            } else {
                k++;
            }
        }
    }
    /* Workers still running stay ACTIVE on the tree; the next fanout call (or
     * the next agent turn) collects them. Nothing is lost — the swarm owns
     * the processes, the tree owns the tasks. */

    /* Does anything left need the coordinator model? */
    for (int i = 0; i < q->task_count; i++) {
        const goal_queue_task_t *t = &q->tasks[i];
        if (t->status != GOAL_QUEUE_TASK_QUEUED) continue;
        if (t->lane == GOAL_QUEUE_LANE_PLAN || !t->work_unit_json[0]) {
            st.model_needed = true;
            break;
        }
        work_unit_t wu;
        if (!work_unit_from_json(&wu, t->work_unit_json)) {
            work_unit_free(&wu);
            st.model_needed = true;
            break;
        }
        for (size_t d = 0; d < wu.depends_count; d++)
            if (!wd_dep_exists(q, wu.depends_on[d])) {
                st.model_needed = true; /* unresolved ID needs correction */
                break;
            }
        work_unit_free(&wu);
        if (st.model_needed) break;
    }
    return st;
}
