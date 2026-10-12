#include "goal_native.h"
#include "goal_queue.h"
#include <stdio.h>
#include <string.h>

static const char *status_name(dsco_goal_status_t status) {
    switch (status) {
        case DSCO_GOAL_ACTIVE: return "active";
        case DSCO_GOAL_PAUSED: return "paused";
        case DSCO_GOAL_BLOCKED: return "blocked";
        case DSCO_GOAL_COMPLETE: return "complete";
        case DSCO_GOAL_BUDGET_LIMITED: return "budget limited";
        default: return "none";
    }
}

/* Goal text comes from a user or a model: do not let it create terminal
 * control sequences or fake status rows in either native presentation path. */
static void clean_line(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t i = 0;
    if (src) for (; *src && i + 1 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        dst[i++] = c < 32 || c == 127 ? ' ' : (char)c;
    }
    dst[i] = 0;
}

bool goal_native_format(const session_state_t *s, char *out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    if (!s || !s->goal_objective[0] || s->goal_status == DSCO_GOAL_NONE)
        return false;
    char objective[161], task[161], reason[161];
    clean_line(objective, sizeof(objective), s->goal_objective);
    clean_line(reason, sizeof(reason), s->goal_reason);
    const goal_queue_t *q = &s->goal_queue;
    const goal_queue_task_t *current = NULL;
    if (q->initialized && q->current_id > 0)
        for (int i = 0; i < q->task_count && i < GOAL_QUEUE_MAX_TASKS; ++i)
            if (q->tasks[i].id == q->current_id &&
                q->tasks[i].status == GOAL_QUEUE_TASK_ACTIVE) {
                current = &q->tasks[i]; break;
            }
    clean_line(task, sizeof(task), current ? current->title : "none leased");
    int n = snprintf(out, cap,
        "GOAL SEEKING\nStatus: %s\nObjective: %s\n"
        "Current task: %s\nPlan queued: %d | Work queued: %d\n"
        "Controller revision: %d\nTurns: %d / %d\nToken budget: %s",
        status_name(s->goal_status), objective, task,
        q->initialized ? q->plan_count : 0, q->initialized ? q->work_count : 0,
        q->initialized ? q->revision : 0, s->goal_turns,
        s->goal_turn_limit > 0 ? s->goal_turn_limit : 32,
        s->goal_token_budget > 0 ? "set" : "not set");
    if (n < 0 || (size_t)n >= cap) { out[cap-1] = 0; return false; }
    size_t used = (size_t)n;
    if (s->goal_token_budget > 0) {
        n = snprintf(out + used, cap - used, " (%d tokens)", s->goal_token_budget);
        if (n < 0 || (size_t)n >= cap - used) { out[cap-1] = 0; return false; }
        used += (size_t)n;
    }
    if (reason[0] && s->goal_status != DSCO_GOAL_ACTIVE) {
        n = snprintf(out + used, cap - used, "\nReason: %s", reason);
        if (n < 0 || (size_t)n >= cap - used) { out[cap-1] = 0; return false; }
    }
    return true;
}
