#include "scheduler_lifetime.h"
#include <errno.h>
#include <string.h>

static bool terminal(task_state_t state) {
    return state == TASK_COMPLETED || state == TASK_FAILED || state == TASK_CANCELLED;
}

void sched_lifetime_collect(scheduler_t *s) {
    if (!s || s->current != TASK_INVALID) return;
    for (int i = 0; i < s->task_count; ++i) {
        sched_task_t *t = &s->tasks[i];
        if (t->id == TASK_INVALID || !t->release_requested || !terminal(t->state)) continue;
        bool pinned = false;
        for (int j = 0; j < s->task_count; ++j) {
            sched_task_t *observer = &s->tasks[j];
            if (observer->id != TASK_INVALID && !terminal(observer->state) &&
                observer->wait_task == t->id) { pinned = true; break; }
        }
        if (pinned) continue;
        /* Cancellation can leave queue entries. Remove them before reusing
         * capacity, so repeated cancel/release cannot fill a ready queue. */
        for (int p = 0; p < SCHED_PRIO_COUNT; ++p) {
            int used = 0;
            for (int j = 0; j < s->run_queue_len[p]; ++j)
                if (s->run_queue[p][j] != t->id)
                    s->run_queue[p][used++] = s->run_queue[p][j];
            s->run_queue_len[p] = used;
        }
        memset(t, 0, sizeof(*t));
        t->id = TASK_INVALID;
        t->wait_task = TASK_INVALID;
    }
}

bool sched_release(scheduler_t *s, task_id_t id) {
    sched_task_t *t = sched_task_get(s, id);
    if (!t) { errno = ENOENT; return false; }
    if (!terminal(t->state) || s->current == id) { errno = EBUSY; return false; }
    t->release_requested = true;
    sched_lifetime_collect(s);
    return true;
}
