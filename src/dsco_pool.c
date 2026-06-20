#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "dsco_pool.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
  #include <dispatch/dispatch.h>
  #define DSCO_POOL_GCD 1
#endif

/* ──────────────────────────────────────────────────────────────────────────
 *  Task handle (used by both backends)
 * ────────────────────────────────────────────────────────────────────────── */

struct dsco_task {
    atomic_int          done;
    pthread_mutex_t     mu;
    pthread_cond_t      cv;
    dsco_pool_work_fn   fn;
    void               *ctx;
};

/* ──────────────────────────────────────────────────────────────────────────
 *  GCD backend
 * ────────────────────────────────────────────────────────────────────────── */

#if DSCO_POOL_GCD

static dispatch_queue_t g_q_concurrent = NULL;
static int g_workers = 0;

int dsco_pool_global_init(int nthreads) {
    (void)nthreads;
    if (g_q_concurrent) return 0;
    g_q_concurrent = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    g_workers = n > 0 ? (int)n : 4;
    return 0;
}

void dsco_pool_global_shutdown(void) {
    g_q_concurrent = NULL;
    g_workers = 0;
}

int dsco_pool_worker_count(void) { return g_workers; }

typedef struct {
    void               *ctx;
    dsco_pool_iter_fn   fn;
} apply_ctx_t;

void dsco_pool_apply(size_t n, void *ctx, dsco_pool_iter_fn fn) {
    if (!fn || n == 0) return;
    dsco_pool_global_init(0);
    apply_ctx_t a = { ctx, fn };
    dispatch_apply(n, g_q_concurrent, ^(size_t i){
        a.fn(i, a.ctx);
    });
}

static void task_run_block(struct dsco_task *t) {
    t->fn(t->ctx);
    pthread_mutex_lock(&t->mu);
    atomic_store_explicit(&t->done, 1, memory_order_release);
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mu);
}

dsco_task_t *dsco_pool_submit(dsco_pool_work_fn fn, void *ctx) {
    if (!fn) return NULL;
    dsco_pool_global_init(0);
    struct dsco_task *t = (struct dsco_task *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fn = fn; t->ctx = ctx;
    pthread_mutex_init(&t->mu, NULL);
    pthread_cond_init(&t->cv, NULL);
    dispatch_async(g_q_concurrent, ^{ task_run_block(t); });
    return t;
}

void dsco_pool_join(dsco_task_t *t) {
    if (!t) return;
    pthread_mutex_lock(&t->mu);
    while (!atomic_load_explicit(&t->done, memory_order_acquire))
        pthread_cond_wait(&t->cv, &t->mu);
    pthread_mutex_unlock(&t->mu);
    pthread_mutex_destroy(&t->mu);
    pthread_cond_destroy(&t->cv);
    free(t);
}

#else /* ──── pthread fallback ──────────────────────────────────────────── */

#define DSCO_POOL_QUEUE_CAP 1024

typedef struct {
    dsco_pool_work_fn fn;
    void             *ctx;
    struct dsco_task *waker;
} work_item_t;

static struct {
    pthread_t        *threads;
    int               n;
    work_item_t       q[DSCO_POOL_QUEUE_CAP];
    int               head, tail, count;
    pthread_mutex_t   mu;
    pthread_cond_t    not_empty;
    pthread_cond_t    not_full;
    int               shutdown;
} g_p = { NULL, 0, {{0}}, 0, 0, 0,
         PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
         PTHREAD_COND_INITIALIZER, 0 };

static void *worker_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_p.mu);
        while (g_p.count == 0 && !g_p.shutdown)
            pthread_cond_wait(&g_p.not_empty, &g_p.mu);
        if (g_p.shutdown && g_p.count == 0) { pthread_mutex_unlock(&g_p.mu); break; }
        work_item_t it = g_p.q[g_p.head];
        g_p.head = (g_p.head + 1) % DSCO_POOL_QUEUE_CAP;
        g_p.count--;
        pthread_cond_signal(&g_p.not_full);
        pthread_mutex_unlock(&g_p.mu);

        it.fn(it.ctx);
        if (it.waker) {
            pthread_mutex_lock(&it.waker->mu);
            atomic_store_explicit(&it.waker->done, 1, memory_order_release);
            pthread_cond_broadcast(&it.waker->cv);
            pthread_mutex_unlock(&it.waker->mu);
        }
    }
    return NULL;
}

int dsco_pool_global_init(int nthreads) {
    if (g_p.threads) return 0;
    if (nthreads <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        nthreads = n > 0 ? (int)n : 4;
    }
    g_p.n = nthreads;
    g_p.threads = (pthread_t *)calloc(nthreads, sizeof(pthread_t));
    if (!g_p.threads) return -1;
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&g_p.threads[i], NULL, worker_main, NULL) != 0) {
            g_p.n = i; break;
        }
    }
    return 0;
}

void dsco_pool_global_shutdown(void) {
    if (!g_p.threads) return;
    pthread_mutex_lock(&g_p.mu);
    g_p.shutdown = 1;
    pthread_cond_broadcast(&g_p.not_empty);
    pthread_mutex_unlock(&g_p.mu);
    for (int i = 0; i < g_p.n; i++) pthread_join(g_p.threads[i], NULL);
    free(g_p.threads);
    g_p.threads = NULL;
    g_p.n = 0;
    g_p.head = g_p.tail = g_p.count = 0;
    g_p.shutdown = 0;
}

int dsco_pool_worker_count(void) { return g_p.n; }

static void enqueue(work_item_t it) {
    pthread_mutex_lock(&g_p.mu);
    while (g_p.count == DSCO_POOL_QUEUE_CAP && !g_p.shutdown)
        pthread_cond_wait(&g_p.not_full, &g_p.mu);
    if (g_p.shutdown) { pthread_mutex_unlock(&g_p.mu); return; }
    g_p.q[g_p.tail] = it;
    g_p.tail = (g_p.tail + 1) % DSCO_POOL_QUEUE_CAP;
    g_p.count++;
    pthread_cond_signal(&g_p.not_empty);
    pthread_mutex_unlock(&g_p.mu);
}

typedef struct {
    void              *ctx;
    dsco_pool_iter_fn  fn;
    size_t             i;
} apply_iter_t;

static void apply_iter_run(void *p) {
    apply_iter_t *a = (apply_iter_t *)p;
    a->fn(a->i, a->ctx);
    free(a);
}

void dsco_pool_apply(size_t n, void *ctx, dsco_pool_iter_fn fn) {
    if (!fn || n == 0) return;
    dsco_pool_global_init(0);
    /* simple but effective: enqueue n items and wait on a counter */
    atomic_size_t remaining; atomic_init(&remaining, n);
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
    struct ctx_pack { dsco_pool_iter_fn fn; void *ctx; size_t i;
                       atomic_size_t *remaining; pthread_mutex_t *mu;
                       pthread_cond_t *cv; };
    for (size_t i = 0; i < n; i++) {
        struct ctx_pack *c = (struct ctx_pack *)calloc(1, sizeof(*c));
        c->fn = fn; c->ctx = ctx; c->i = i;
        c->remaining = &remaining; c->mu = &mu; c->cv = &cv;
        work_item_t it = { (dsco_pool_work_fn)(void*)({
            void __apply(void *p){
                struct ctx_pack *cc = (struct ctx_pack *)p;
                cc->fn(cc->i, cc->ctx);
                if (atomic_fetch_sub(cc->remaining, 1) == 1) {
                    pthread_mutex_lock(cc->mu);
                    pthread_cond_signal(cc->cv);
                    pthread_mutex_unlock(cc->mu);
                }
                free(cc);
            } __apply;
        }), c, NULL };
        enqueue(it);
    }
    pthread_mutex_lock(&mu);
    while (atomic_load(&remaining) > 0) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

dsco_task_t *dsco_pool_submit(dsco_pool_work_fn fn, void *ctx) {
    if (!fn) return NULL;
    dsco_pool_global_init(0);
    struct dsco_task *t = (struct dsco_task *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fn = fn; t->ctx = ctx;
    pthread_mutex_init(&t->mu, NULL);
    pthread_cond_init(&t->cv, NULL);
    work_item_t it = { fn, ctx, t };
    enqueue(it);
    return t;
}

void dsco_pool_join(dsco_task_t *t) {
    if (!t) return;
    pthread_mutex_lock(&t->mu);
    while (!atomic_load_explicit(&t->done, memory_order_acquire))
        pthread_cond_wait(&t->cv, &t->mu);
    pthread_mutex_unlock(&t->mu);
    pthread_mutex_destroy(&t->mu);
    pthread_cond_destroy(&t->cv);
    free(t);
}

#endif
