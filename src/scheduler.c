#include "scheduler.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Monotonic clock ───────────────────────────────────────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ── Task lookup ───────────────────────────────────────────────────── */

static sched_task_t *find_task(scheduler_t *s, task_id_t id) {
    for (int i = 0; i < s->task_count; i++) {
        if (s->tasks[i].id == id)
            return &s->tasks[i];
    }
    return NULL;
}

/* ── Run queue management ──────────────────────────────────────────── */

static void enqueue(scheduler_t *s, sched_priority_t prio, task_id_t id) {
    int p = (int)prio;
    if (p < 0 || p >= SCHED_PRIO_COUNT) return;
    if (s->run_queue_len[p] >= SCHED_MAX_TASKS) return;

    /* Avoid duplicates */
    for (int i = 0; i < s->run_queue_len[p]; i++) {
        if (s->run_queue[p][i] == id) return;
    }

    s->run_queue[p][s->run_queue_len[p]++] = id;
}

static task_id_t dequeue(scheduler_t *s, sched_priority_t prio) {
    int p = (int)prio;
    if (p < 0 || p >= SCHED_PRIO_COUNT || s->run_queue_len[p] == 0)
        return TASK_INVALID;

    task_id_t id = s->run_queue[p][0];
    /* Shift remaining entries */
    for (int i = 1; i < s->run_queue_len[p]; i++)
        s->run_queue[p][i - 1] = s->run_queue[p][i];
    s->run_queue_len[p]--;
    return id;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void sched_init(scheduler_t *s) {
    memset(s, 0, sizeof(*s));
    s->current = TASK_INVALID;
    s->default_budget = 100; /* ticks per scheduling round */
}

void sched_destroy(scheduler_t *s) {
    memset(s, 0, sizeof(*s));
}

/* ── Task management ───────────────────────────────────────────────── */

task_id_t sched_spawn(scheduler_t *s, task_func_t func, void *ctx,
                      const char *label, sched_priority_t prio) {
    if (!s || !func || s->task_count >= SCHED_MAX_TASKS)
        return TASK_INVALID;

    int idx = s->task_count++;
    sched_task_t *t = &s->tasks[idx];
    memset(t, 0, sizeof(*t));

    t->id           = s->next_id++;
    t->func         = func;
    t->ctx          = ctx;
    t->label        = label;
    t->priority     = prio;
    t->state        = TASK_READY;
    t->created_ms   = now_ms();
    t->wait_fd      = -1;
    t->wait_task    = TASK_INVALID;
    t->budget       = s->default_budget;

    enqueue(s, prio, t->id);
    return t->id;
}

bool sched_cancel(scheduler_t *s, task_id_t id) {
    sched_task_t *t = find_task(s, id);
    if (!t) return false;
    if (t->state == TASK_COMPLETED || t->state == TASK_FAILED) return false;

    t->state = TASK_CANCELLED;
    t->completed_ms = now_ms();
    return true;
}

sched_task_t *sched_task_get(scheduler_t *s, task_id_t id) {
    return find_task(s, id);
}

/* ── Wait operations ───────────────────────────────────────────────── */

void sched_wait_fd(scheduler_t *s, int fd) {
    if (s->current == TASK_INVALID) return;
    sched_task_t *t = find_task(s, s->current);
    if (t) {
        t->state = TASK_WAITING_IO;
        t->wait_fd = fd;
    }
}

void sched_sleep(scheduler_t *s, int ms) {
    if (s->current == TASK_INVALID) return;
    sched_task_t *t = find_task(s, s->current);
    if (t) {
        t->state = TASK_WAITING_TIMER;
        t->wake_time_ms = now_ms() + (uint64_t)ms;
    }
}

void sched_wait_task(scheduler_t *s, task_id_t other) {
    if (s->current == TASK_INVALID) return;
    sched_task_t *t = find_task(s, s->current);
    if (t) {
        t->state = TASK_WAITING_TASK;
        t->wait_task = other;
    }
}

/* ── Wake condition checking ───────────────────────────────────────── */

static void check_wakeups(scheduler_t *s) {
    uint64_t t = now_ms();
    for (int i = 0; i < s->task_count; i++) {
        sched_task_t *task = &s->tasks[i];

        if (task->state == TASK_WAITING_TIMER && t >= task->wake_time_ms) {
            task->state = TASK_READY;
            enqueue(s, task->priority, task->id);
        }

        if (task->state == TASK_WAITING_TASK) {
            sched_task_t *other = find_task(s, task->wait_task);
            if (!other || other->state == TASK_COMPLETED ||
                other->state == TASK_FAILED || other->state == TASK_CANCELLED) {
                task->state = TASK_READY;
                enqueue(s, task->priority, task->id);
            }
        }
    }
}

/* ── Execution ─────────────────────────────────────────────────────── */

int sched_tick(scheduler_t *s) {
    if (!s) return 0;

    s->tick_count++;
    check_wakeups(s);

    /* Pick highest-priority ready task */
    task_id_t picked = TASK_INVALID;
    for (int p = 0; p < SCHED_PRIO_COUNT; p++) {
        picked = dequeue(s, (sched_priority_t)p);
        if (picked != TASK_INVALID) break;
    }

    if (picked == TASK_INVALID) {
        /* Count active tasks */
        int active = 0;
        for (int i = 0; i < s->task_count; i++) {
            task_state_t st = s->tasks[i].state;
            if (st != TASK_COMPLETED && st != TASK_FAILED && st != TASK_CANCELLED)
                active++;
        }
        return active;
    }

    sched_task_t *task = find_task(s, picked);
    if (!task) return 0;

    /* Execute */
    task->state = TASK_RUNNING;
    if (task->started_ms == 0) task->started_ms = now_ms();
    s->current = task->id;
    s->total_dispatches++;

    uint64_t before = now_ms();
    int ret = task->func(task->ctx);
    uint64_t elapsed = now_ms() - before;
    task->cpu_ms += elapsed;
    task->ticks++;

    s->current = TASK_INVALID;

    if (ret < 0) {
        task->state = TASK_FAILED;
        task->exit_code = ret;
        task->completed_ms = now_ms();
    } else if (ret == 0) {
        task->state = TASK_COMPLETED;
        task->exit_code = 0;
        task->completed_ms = now_ms();
    } else {
        /* Task yielded — check if it set a wait condition */
        if (task->state == TASK_RUNNING) {
            /* No wait condition set — it's simply yielding */
            task->state = TASK_READY;

            /* Budget enforcement */
            if (task->budget > 0 && task->ticks >= task->budget) {
                /* Preempted — lower priority */
                sched_priority_t demoted = task->priority;
                if (demoted < SCHED_PRIO_IDLE) demoted++;
                enqueue(s, demoted, task->id);
            } else {
                enqueue(s, task->priority, task->id);
            }
        }
        /* If task set WAITING_IO/WAITING_TIMER/WAITING_TASK, it stays there */
    }

    /* Count still-active */
    int active = 0;
    for (int i = 0; i < s->task_count; i++) {
        task_state_t st = s->tasks[i].state;
        if (st != TASK_COMPLETED && st != TASK_FAILED && st != TASK_CANCELLED)
            active++;
    }
    return active;
}

int sched_run(scheduler_t *s, int poll_ms) {
    (void)poll_ms;
    if (!s) return -1;
    s->running = true;
    while (s->running) {
        int active = sched_tick(s);
        if (active == 0) break;
    }
    s->running = false;
    return 0;
}

void sched_stop(scheduler_t *s) {
    if (s) s->running = false;
}

/* ── Convenience ───────────────────────────────────────────────────── */

int sched_run_sync(scheduler_t *s, task_func_t func, void *ctx,
                   const char *label) {
    task_id_t id = sched_spawn(s, func, ctx, label, SCHED_PRIO_HIGH);
    if (id == TASK_INVALID) return -1;

    while (1) {
        sched_task_t *t = find_task(s, id);
        if (!t) return -1;
        if (t->state == TASK_COMPLETED) return t->exit_code;
        if (t->state == TASK_FAILED)    return t->exit_code;
        if (t->state == TASK_CANCELLED) return -2;
        sched_tick(s);
    }
}

int sched_count_by_state(scheduler_t *s, task_state_t state) {
    int count = 0;
    for (int i = 0; i < s->task_count; i++) {
        if (s->tasks[i].state == state) count++;
    }
    return count;
}

/* ── Stats ─────────────────────────────────────────────────────────── */

sched_stats_t sched_get_stats(scheduler_t *s) {
    sched_stats_t st;
    memset(&st, 0, sizeof(st));
    if (!s) return st;
    st.tick_count = s->tick_count;
    st.total_dispatches = s->total_dispatches;
    st.tasks_total = s->task_count;
    st.tasks_ready = sched_count_by_state(s, TASK_READY);
    st.tasks_waiting = sched_count_by_state(s, TASK_WAITING_IO) +
                       sched_count_by_state(s, TASK_WAITING_TIMER) +
                       sched_count_by_state(s, TASK_WAITING_TASK);
    st.tasks_completed = sched_count_by_state(s, TASK_COMPLETED);
    st.tasks_failed = sched_count_by_state(s, TASK_FAILED);
    return st;
}

/* ── Debug ─────────────────────────────────────────────────────────── */

static const char *state_name(task_state_t st) {
    switch (st) {
    case TASK_CREATED:       return "CREATED";
    case TASK_READY:         return "READY";
    case TASK_RUNNING:       return "RUNNING";
    case TASK_WAITING_IO:    return "WAIT_IO";
    case TASK_WAITING_TIMER: return "WAIT_TMR";
    case TASK_WAITING_TASK:  return "WAIT_TSK";
    case TASK_COMPLETED:     return "DONE";
    case TASK_FAILED:        return "FAILED";
    case TASK_CANCELLED:     return "CANCEL";
    }
    return "???";
}

static const char *prio_name(sched_priority_t p) {
    switch (p) {
    case SCHED_PRIO_CRITICAL: return "CRIT";
    case SCHED_PRIO_HIGH:     return "HIGH";
    case SCHED_PRIO_NORMAL:   return "NORM";
    case SCHED_PRIO_LOW:      return "LOW";
    case SCHED_PRIO_IDLE:     return "IDLE";
    default:                  return "???";
    }
}

void sched_dump(scheduler_t *s) {
    if (!s) return;
    fprintf(stderr, "  scheduler: %d tasks, %llu ticks, %llu dispatches\n",
            s->task_count, (unsigned long long)s->tick_count,
            (unsigned long long)s->total_dispatches);
    fprintf(stderr, "  %-4s %-16s %-5s %-8s %6s %8s\n",
            "ID", "LABEL", "PRIO", "STATE", "TICKS", "CPU(ms)");
    fprintf(stderr, "  %-4s %-16s %-5s %-8s %6s %8s\n",
            "──", "─────", "────", "─────", "─────", "───────");
    for (int i = 0; i < s->task_count; i++) {
        sched_task_t *t = &s->tasks[i];
        fprintf(stderr, "  %-4d %-16s %-5s %-8s %6d %8llu\n",
                t->id,
                t->label ? t->label : "(none)",
                prio_name(t->priority),
                state_name(t->state),
                t->ticks,
                (unsigned long long)t->cpu_ms);
    }
}
