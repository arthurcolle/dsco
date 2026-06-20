#ifndef DSCO_SCHEDULER_H
#define DSCO_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ── Task priorities ───────────────────────────────────────────────── */

typedef enum {
    SCHED_PRIO_CRITICAL = 0,
    SCHED_PRIO_HIGH     = 1,
    SCHED_PRIO_NORMAL   = 2,
    SCHED_PRIO_LOW      = 3,
    SCHED_PRIO_IDLE     = 4,
    SCHED_PRIO_COUNT    = 5,
} sched_priority_t;

/* ── Task states ───────────────────────────────────────────────────── */

typedef enum {
    TASK_CREATED,
    TASK_READY,
    TASK_RUNNING,
    TASK_WAITING_IO,
    TASK_WAITING_TIMER,
    TASK_WAITING_TASK,
    TASK_COMPLETED,
    TASK_FAILED,
    TASK_CANCELLED,
} task_state_t;

/* ── Task function signature ───────────────────────────────────────── */

typedef int (*task_func_t)(void *ctx);

/* ── Task handle ───────────────────────────────────────────────────── */

typedef int task_id_t;
#define TASK_INVALID (-1)

/* ── Scheduler ─────────────────────────────────────────────────────── */

#define SCHED_MAX_TASKS 256

typedef struct sched_task {
    task_id_t        id;
    task_func_t      func;
    void            *ctx;
    const char      *label;
    sched_priority_t priority;
    task_state_t     state;

    uint64_t         created_ms;
    uint64_t         started_ms;
    uint64_t         completed_ms;
    uint64_t         cpu_ms;
    int              ticks;

    int              wait_fd;
    uint64_t         wake_time_ms;
    task_id_t        wait_task;

    int              exit_code;
    const char      *error_msg;

    int              budget;
} sched_task_t;

typedef struct scheduler_t {
    sched_task_t tasks[SCHED_MAX_TASKS];
    int          task_count;
    task_id_t    next_id;

    task_id_t    run_queue[SCHED_PRIO_COUNT][SCHED_MAX_TASKS];
    int          run_queue_len[SCHED_PRIO_COUNT];

    task_id_t    current;

    bool         running;
    uint64_t     tick_count;
    uint64_t     total_dispatches;

    int          default_budget;
} scheduler_t;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void        sched_init(scheduler_t *s);
void        sched_destroy(scheduler_t *s);

/* ── Task management ───────────────────────────────────────────────── */

task_id_t   sched_spawn(scheduler_t *s, task_func_t func, void *ctx,
                        const char *label, sched_priority_t prio);
bool        sched_cancel(scheduler_t *s, task_id_t id);
sched_task_t *sched_task_get(scheduler_t *s, task_id_t id);

/* ── Wait operations ───────────────────────────────────────────────── */

void        sched_wait_fd(scheduler_t *s, int fd);
void        sched_sleep(scheduler_t *s, int ms);
void        sched_wait_task(scheduler_t *s, task_id_t other);

/* ── Execution ─────────────────────────────────────────────────────── */

int         sched_tick(scheduler_t *s);
int         sched_run(scheduler_t *s, int poll_ms);
void        sched_stop(scheduler_t *s);

/* ── Convenience ───────────────────────────────────────────────────── */

int         sched_run_sync(scheduler_t *s, task_func_t func, void *ctx,
                           const char *label);
int         sched_count_by_state(scheduler_t *s, task_state_t state);

/* ── Stats ─────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t tick_count;
    uint64_t total_dispatches;
    int      tasks_total;
    int      tasks_ready;
    int      tasks_waiting;
    int      tasks_completed;
    int      tasks_failed;
} sched_stats_t;

sched_stats_t sched_get_stats(scheduler_t *s);
void          sched_dump(scheduler_t *s);

#endif /* DSCO_SCHEDULER_H */
