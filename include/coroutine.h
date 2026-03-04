#ifndef DSCO_COROUTINE_H
#define DSCO_COROUTINE_H

/*
 * Coroutines in C — based on Simon Tatham's technique using Duff's device.
 *
 * This provides stackless coroutines via the C preprocessor + switch/case.
 * Two flavors:
 *   - Static (scr_*): uses static locals, non-reentrant
 *   - Context (ccr_*): uses a context struct, fully reentrant
 *
 * Usage (context-based, reentrant):
 *
 *   typedef struct {
 *       ccr_state_t ccr;    // coroutine state
 *       int i;              // local variables go here
 *       int result;
 *   } my_coro_t;
 *
 *   void my_coro_init(my_coro_t *c) { ccr_init(c); }
 *
 *   int my_coro_next(my_coro_t *c) {
 *       ccr_start(c);
 *       for (c->i = 0; c->i < 10; c->i++) {
 *           c->result = c->i * c->i;
 *           ccr_yield(c, c->result);
 *       }
 *       ccr_end(c);
 *       return -1;  // exhausted
 *   }
 */

/* ── Static coroutines (non-reentrant) ────────────────────────────────── */

#define scr_begin      static int scr_state_ = 0; switch (scr_state_) { case 0:
#define scr_end        } scr_state_ = 0;
#define scr_yield(val) do { scr_state_ = __LINE__; return (val); case __LINE__:; } while (0)
#define scr_reset      do { scr_state_ = 0; } while (0)

/* ── Context-based coroutines (reentrant) ─────────────────────────────── */

typedef int ccr_state_t;

#define ccr_init(ctx)          do { (ctx)->ccr = 0; } while (0)
#define ccr_start(ctx)         switch ((ctx)->ccr) { case 0:
#define ccr_end(ctx)           } (ctx)->ccr = -1;
#define ccr_yield(ctx, val)    do { (ctx)->ccr = __LINE__; return (val); case __LINE__:; } while (0)
#define ccr_finished(ctx)      ((ctx)->ccr == -1)
#define ccr_running(ctx)       ((ctx)->ccr >= 0)

/* ── Generator pattern ────────────────────────────────────────────────── */

/*
 * A generator wraps a coroutine with iteration semantics.
 * The generator function returns true while it has values, false when done.
 *
 *   typedef struct {
 *       ccr_state_t ccr;
 *       int current;        // yielded value
 *       int i;              // internal state
 *   } range_gen_t;
 *
 *   bool range_next(range_gen_t *g, int max) {
 *       gen_start(g);
 *       for (g->i = 0; g->i < max; g->i++) {
 *           g->current = g->i;
 *           gen_yield(g, true);
 *       }
 *       gen_end(g);
 *       return false;
 *   }
 *
 *   // Usage:
 *   range_gen_t g; ccr_init(&g);
 *   while (range_next(&g, 10)) {
 *       printf("%d\n", g.current);
 *   }
 */

#define gen_start(ctx)        ccr_start(ctx)
#define gen_end(ctx)          ccr_end(ctx)
#define gen_yield(ctx, val)   ccr_yield(ctx, val)

/* ── Async/await pattern for cooperative multitasking ─────────────────── */

/*
 * For use with the swarm system. Each sub-task can be a coroutine that
 * yields control back to a scheduler, enabling cooperative multiplexing
 * without threads.
 *
 *   typedef struct {
 *       ccr_state_t ccr;
 *       int phase;
 *       char result[1024];
 *   } async_task_t;
 *
 *   typedef enum { ASYNC_PENDING, ASYNC_READY, ASYNC_DONE } async_status_t;
 *
 *   async_status_t my_task(async_task_t *t) {
 *       ccr_start(t);
 *       // Phase 1: start some work
 *       t->phase = 1;
 *       ccr_yield(t, ASYNC_PENDING);
 *
 *       // Phase 2: check if external data ready
 *       t->phase = 2;
 *       ccr_yield(t, ASYNC_PENDING);
 *
 *       // Phase 3: done
 *       snprintf(t->result, sizeof(t->result), "completed");
 *       ccr_end(t);
 *       return ASYNC_DONE;
 *   }
 */

/* ── Pipeline coroutine ───────────────────────────────────────────────── */

/*
 * Producer/consumer pipelines where data flows between coroutines.
 *
 *   typedef struct {
 *       ccr_state_t ccr;
 *       char *data;      // shared buffer
 *       int len;
 *       bool has_data;
 *   } pipe_coro_t;
 *
 *   // Producer: generates data and yields
 *   void producer(pipe_coro_t *p, const char *input) {
 *       ccr_start(p);
 *       while (input && *input) {
 *           p->data = (char *)input;
 *           p->len = 1;
 *           p->has_data = true;
 *           ccr_yield(p, 0);
 *           input++;
 *       }
 *       p->has_data = false;
 *       ccr_end(p);
 *   }
 *
 *   // Consumer: processes data when available
 *   void consumer(pipe_coro_t *p, char *output, int *out_len) {
 *       ccr_start(p);
 *       *out_len = 0;
 *       while (p->has_data) {
 *           output[(*out_len)++] = *p->data;
 *           ccr_yield(p, 0);
 *       }
 *       ccr_end(p);
 *   }
 */

/* ── Scheduler for multiple coroutines ────────────────────────────────── */

#define CORO_SCHED_MAX 64

typedef enum {
    CORO_STATUS_IDLE,
    CORO_STATUS_RUNNING,
    CORO_STATUS_WAITING,
    CORO_STATUS_DONE,
} coro_status_t;

typedef int (*coro_func_t)(void *ctx);

typedef struct {
    void         *ctx;
    coro_func_t   func;
    coro_status_t status;
    int           priority;
    const char   *label;
} coro_slot_t;

typedef struct {
    coro_slot_t slots[CORO_SCHED_MAX];
    int         count;
    int         active;
    int         round;
} coro_scheduler_t;

static inline void coro_sched_init(coro_scheduler_t *s) {
    s->count = 0;
    s->active = 0;
    s->round = 0;
}

static inline int coro_sched_add(coro_scheduler_t *s, coro_func_t func,
                                  void *ctx, const char *label, int priority) {
    if (s->count >= CORO_SCHED_MAX) return -1;
    int id = s->count++;
    s->slots[id].func = func;
    s->slots[id].ctx = ctx;
    s->slots[id].status = CORO_STATUS_IDLE;
    s->slots[id].priority = priority;
    s->slots[id].label = label;
    s->active++;
    return id;
}

/* Run one round-robin tick. Returns number of still-active coroutines. */
static inline int coro_sched_tick(coro_scheduler_t *s) {
    s->round++;
    int still_active = 0;
    for (int i = 0; i < s->count; i++) {
        coro_slot_t *slot = &s->slots[i];
        if (slot->status == CORO_STATUS_DONE) continue;

        slot->status = CORO_STATUS_RUNNING;
        int ret = slot->func(slot->ctx);

        if (ret < 0) {
            slot->status = CORO_STATUS_DONE;
        } else if (ret == 0) {
            slot->status = CORO_STATUS_WAITING;
            still_active++;
        } else {
            slot->status = CORO_STATUS_IDLE;
            still_active++;
        }
    }
    s->active = still_active;
    return still_active;
}

/* Run until all coroutines are done. */
static inline int coro_sched_run(coro_scheduler_t *s) {
    while (coro_sched_tick(s) > 0) {
        /* Optionally: usleep, poll, etc. between rounds */
    }
    return s->round;
}

#endif /* DSCO_COROUTINE_H */
