/* Isolated fan-out regression: no network, model, or real swarm process. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/work_dispatch.c"

static swarm_t fake_swarm;
static int spawns, deliveries, executions, polls;
swarm_t *tools_swarm_handle(void) { return &fake_swarm; }
swarm_child_t *swarm_get(swarm_t *s, int id) {
    return id >= 0 && id < s->child_count ? &s->children[id] : NULL;
}
int swarm_poll(swarm_t *s, int timeout_ms) { (void)s; (void)timeout_ms; polls++; return 0; }
int swarm_spawn_provider(swarm_t *s, int group_id, const char *task,
                         const char *model, const char *provider) {
    (void)group_id; (void)task; (void)model; (void)provider;
    int id = s->child_count++;
    s->children[id].status = SWARM_RUNNING;
    spawns++;
    return id;
}
void swarm_set_next_instance(const char *effort, double temp, double top_p, int top_k,
                             int thinking, const char *choice, const char *prompt, int turns) {
    (void)effort;(void)temp;(void)top_p;(void)top_k;(void)thinking;(void)choice;(void)prompt;(void)turns;
}
void swarm_set_next_cwd(const char *cwd) { (void)cwd; }
void swarm_set_next_budget_usd(double usd) { (void)usd; }
bool work_unit_from_json(work_unit_t *w, const char *json) {
    memset(w, 0, sizeof(*w));
    w->kind = strstr(json, "prompt") ? WORK_UNIT_PROMPT : WORK_UNIT_TOOL;
    w->provider = strdup("fake");
    w->payload = strdup("fake");
    if (w->kind == WORK_UNIT_TOOL) w->tool = strdup("fake");
    if (strstr(json, "missing")) {
        w->depends_on = calloc(1, sizeof(*w->depends_on));
        strcpy(w->depends_on[0], "missing");
        w->depends_count = 1;
    }
    return true;
}
void work_unit_free(work_unit_t *w) {
    free(w->provider); free(w->payload); free(w->tool); free(w->depends_on);
    memset(w, 0, sizeof(*w));
}
bool tools_execute_for_tier(const char *name, const char *args, const char *tier,
                            char *result, size_t cap) {
    (void)name;(void)args;(void)tier;(void)cap;
    executions++; strcpy(result, "ok"); return true;
}
void work_result_init(work_result_t *r, work_result_kind_t kind) {
    memset(r, 0, sizeof(*r)); r->kind = kind;
}
void work_result_free(work_result_t *r) { free(r->error_code); free(r->error_detail); free(r->text); }
bool work_result_set_text(work_result_t *r, const char *text) { r->text = strdup(text); return r->text != NULL; }
char *work_result_to_json(const work_result_t *r) { return strdup(r->ok ? "{\"ok\":true}" : "{\"ok\":false}"); }
bool work_result_take(work_result_t *r) { (void)r; return false; }
void work_result_clear_pending(void) {}
goal_queue_task_status_t goal_queue_deliver_result(goal_queue_t *q, int id, const char *json, bool ok) {
    (void)json; deliveries++;
    for (int i = 0; i < q->task_count; i++)
        if (q->tasks[i].id == id) {
            q->tasks[i].status = ok ? GOAL_QUEUE_TASK_COMPLETE : GOAL_QUEUE_TASK_FAILED;
            return q->tasks[i].status;
        }
    return GOAL_QUEUE_TASK_UNUSED;
}

int main(void) {
    goal_queue_t *q = calloc(1, sizeof(*q));
    assert(q);
    q->initialized = true; q->task_count = 2;
    q->tasks[0].id = 1; q->tasks[0].status = GOAL_QUEUE_TASK_QUEUED;
    q->tasks[0].lane = GOAL_QUEUE_LANE_WORK;
    strcpy(q->tasks[0].work_unit_json, "prompt");
    strcpy(q->tasks[0].work_unit_id, "one");
    q->tasks[1].id = 2; q->tasks[1].status = GOAL_QUEUE_TASK_QUEUED;
    q->tasks[1].lane = GOAL_QUEUE_LANE_WORK;
    strcpy(q->tasks[1].work_unit_json, "tool missing");
    strcpy(q->tasks[1].work_unit_id, "two");
    work_fanout_stats_t st = work_dispatch_fanout(q, "trusted", 1, 0);
    assert(spawns == 1 && st.workers_spawned == 1 && deliveries == 0);
    assert(q->tasks[0].status == GOAL_QUEUE_TASK_ACTIVE);
    assert(q->tasks[0].worker_child_id_plus_one == 1);
    assert(q->tasks[1].status == GOAL_QUEUE_TASK_QUEUED && executions == 0);
    assert(st.deferred_deps >= 1 && st.model_needed);
    fake_swarm.children[0].status = SWARM_DONE;
    st = work_dispatch_fanout(q, "trusted", 1, 0);
    assert(st.workers_done == 1 && deliveries == 1 && spawns == 1);
    assert(q->tasks[0].status == GOAL_QUEUE_TASK_COMPLETE);
    assert(q->tasks[0].worker_child_id_plus_one == 0);
    assert(q->tasks[1].status == GOAL_QUEUE_TASK_QUEUED && executions == 0);
    free(q);
    puts("fanout: missing dependency blocked; straggler delivered on next call");
    return 0;
}
