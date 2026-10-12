#include "async_runtime.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>

#define NONE UINT32_MAX
enum { CELL_FREE, CELL_PENDING, CELL_QUEUED, CELL_DONE };
enum { EDGE_FREE, EDGE_WAITING, EDGE_READY };
typedef struct {
    uint32_t generation, refs, next, edges;
    unsigned phase;
    bool cancel;
    async_result_t result;
} cell_t;
typedef struct {
    uint32_t generation, source, prev, next;
    uint64_t ready_sequence;
    unsigned phase;
    async_completion_fn fn;
    void *ctx;
} edge_t;
struct async_runtime {
    pthread_mutex_t mutex;
    pthread_t owner;
    ev_loop_t *loop;
    cell_t *cells;
    edge_t *edges;
    uint32_t cell_cap, edge_cap, cell_free, edge_free;
    uint32_t mail_head, mail_tail, ready_head, ready_tail;
    bool dispatching;
    uint64_t ready_sequence;
    async_stats_t stats;
};
static async_handle_t handle(uint32_t slot, uint32_t generation) {
    return ((uint64_t)generation << 32) | ((uint64_t)slot + 1);
}
static bool owner(async_runtime_t *r) {
    if (!r) { errno = EINVAL; return false; }
    if (!pthread_equal(r->owner, pthread_self())) { errno = EPERM; return false; }
    return true;
}
static cell_t *cell(async_runtime_t *r, async_handle_t h, uint32_t *slot) {
    uint32_t low = (uint32_t)h;
    if (!low || low > r->cell_cap) { errno = ESTALE; return NULL; }
    *slot = low - 1;
    cell_t *c = &r->cells[*slot];
    if (c->phase == CELL_FREE || !c->refs || c->generation != (uint32_t)(h >> 32)) {
        errno = ESTALE; return NULL;
    }
    return c;
}
/* Called under mutex. Exhausted generations retire rather than alias. */
static void drop(async_runtime_t *r, uint32_t i) {
    cell_t *c = &r->cells[i];
    if (--c->refs || c->phase == CELL_QUEUED) return;
    c->phase = CELL_FREE;
    --r->stats.cells_live; ++r->stats.recycled;
    if (c->generation != UINT32_MAX) { c->next = r->cell_free; r->cell_free = i; }
}
static void edge_recycle(async_runtime_t *r, uint32_t i) {
    edge_t *e = &r->edges[i]; e->phase = EDGE_FREE;
    --r->stats.edges_live;
    if (e->generation != UINT32_MAX) { e->next = r->edge_free; r->edge_free = i; }
}
static void ready_append(async_runtime_t *r, uint32_t i) {
    edge_t *e = &r->edges[i]; e->phase = EDGE_READY;
    e->ready_sequence = ++r->ready_sequence;
    e->prev = r->ready_tail; e->next = NONE;
    if (r->ready_tail != NONE) r->edges[r->ready_tail].next = i;
    else r->ready_head = i;
    r->ready_tail = i; ++r->stats.ready_depth;
}
static void edge_remove(async_runtime_t *r, uint32_t i) {
    edge_t *e = &r->edges[i];
    uint32_t *head = e->phase == EDGE_READY ? &r->ready_head : &r->cells[e->source].edges;
    if (e->prev != NONE) r->edges[e->prev].next = e->next;
    else *head = e->next;
    if (e->next != NONE) r->edges[e->next].prev = e->prev;
    else if (e->phase == EDGE_READY) r->ready_tail = e->prev;
    if (e->phase == EDGE_READY) --r->stats.ready_depth;
}
async_runtime_t *async_runtime_new(size_t cells, size_t edges) {
    if (!cells || !edges || cells >= UINT32_MAX || edges >= UINT32_MAX ||
        cells > SIZE_MAX / sizeof(cell_t) || edges > SIZE_MAX / sizeof(edge_t)) {
        errno = EINVAL; return NULL;
    }
    async_runtime_t *r = calloc(1, sizeof(*r)); if (!r) return NULL;
    r->cells = calloc(cells, sizeof(cell_t)); r->edges = calloc(edges, sizeof(edge_t));
    if (!r->cells || !r->edges) goto fail;
    int rc = pthread_mutex_init(&r->mutex, NULL);
    if (rc) { errno = rc; goto fail; }
    r->loop = ev_loop_new();
    if (!r->loop) { pthread_mutex_destroy(&r->mutex); goto fail; }
    r->owner = pthread_self(); r->cell_cap = (uint32_t)cells; r->edge_cap = (uint32_t)edges;
    r->mail_head = r->mail_tail = r->ready_head = r->ready_tail = NONE;
    for (uint32_t i = 0; i < cells; ++i) r->cells[i].next = i + 1 < cells ? i + 1 : NONE;
    for (uint32_t i = 0; i < edges; ++i) r->edges[i].next = i + 1 < edges ? i + 1 : NONE;
    return r;
fail: {
    int saved = errno; free(r->cells); free(r->edges); free(r); errno = saved; return NULL;
}}
int async_runtime_free(async_runtime_t *r) {
    if (!owner(r)) return -1;
    pthread_mutex_lock(&r->mutex);
    bool busy = r->dispatching || r->stats.cells_live || r->stats.edges_live;
    pthread_mutex_unlock(&r->mutex);
    if (busy) { errno = EBUSY; return -1; }
    ev_loop_free(r->loop); pthread_mutex_destroy(&r->mutex);
    free(r->cells); free(r->edges); free(r); return 0;
}
ev_loop_t *async_runtime_loop(async_runtime_t *r) { return owner(r) ? r->loop : NULL; }
async_stats_t async_runtime_stats(async_runtime_t *r) {
    if (!r) return (async_stats_t){0};
    pthread_mutex_lock(&r->mutex); async_stats_t s = r->stats;
    pthread_mutex_unlock(&r->mutex); return s;
}
async_handle_t async_cell_new(async_runtime_t *r) {
    if (!r) { errno = EINVAL; return 0; }
    pthread_mutex_lock(&r->mutex);
    uint32_t i = r->cell_free; async_handle_t h = 0;
    if (i == NONE) errno = EAGAIN;
    else {
        cell_t *c = &r->cells[i]; r->cell_free = c->next;
        uint32_t generation = c->generation + 1;
        *c = (cell_t){.generation = generation, .refs = 1, .next = NONE,
                     .edges = NONE, .phase = CELL_PENDING};
        ++r->stats.cells_live; h = handle(i, generation);
    }
    pthread_mutex_unlock(&r->mutex); return h;
}
int async_retain(async_runtime_t *r, async_handle_t h) {
    if (!r) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&r->mutex); uint32_t i; cell_t *c = cell(r, h, &i); int rc = -1;
    if (c && c->refs == UINT32_MAX) errno = EOVERFLOW;
    else if (c && c->refs) { ++c->refs; rc = 0; }
    else if (c) errno = ESTALE;
    pthread_mutex_unlock(&r->mutex); return rc;
}
int async_release(async_runtime_t *r, async_handle_t h) {
    if (!r) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&r->mutex); uint32_t i; cell_t *c = cell(r, h, &i); int rc = -1;
    if (c && c->refs) { drop(r, i); rc = 0; } else if (c) errno = ESTALE;
    pthread_mutex_unlock(&r->mutex); return rc;
}
int async_read(async_runtime_t *r, async_handle_t h, async_result_t *out) {
    if (!r || !out) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&r->mutex); uint32_t i; cell_t *c = cell(r, h, &i); int rc = -1;
    if (c) { rc = c->phase != CELL_PENDING; if (rc) *out = c->result; }
    pthread_mutex_unlock(&r->mutex); return rc;
}
int async_publish(async_runtime_t *r, async_handle_t h, async_result_t result) {
    if (!r || result.outcome < ASYNC_SUCCESS || result.outcome > ASYNC_CANCELLED) {
        errno = EINVAL; return -1;
    }
    pthread_mutex_lock(&r->mutex); uint32_t i; cell_t *c = cell(r, h, &i); int rc = -1;
    if (c && c->phase != CELL_PENDING) errno = EALREADY;
    else if (c) {
        /* Wake before commit while holding publication lock. An awakened owner
         * cannot drain until commit. Failed wake leaves the operation pending. */
        bool notify = r->mail_head == NONE && !pthread_equal(r->owner,pthread_self());
        if (!notify || ev_loop_wake(r->loop) == 0) {
            if (notify) ++r->stats.wakes;
            c->result = result; c->phase = CELL_QUEUED; c->next = NONE;
            if (r->mail_tail != NONE) r->cells[r->mail_tail].next = i;
            else r->mail_head = i;
            r->mail_tail = i; ++r->stats.mailbox_depth; ++r->stats.publications; rc = 0;
        }
    }
    pthread_mutex_unlock(&r->mutex); return rc;
}
static int cancellation(async_runtime_t *r, async_handle_t h, bool set) {
    if (!r) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&r->mutex); uint32_t i; cell_t *c = cell(r, h, &i); int rc = -1;
    if (c) { if (set) c->cancel = true; rc = set ? 0 : c->cancel; }
    pthread_mutex_unlock(&r->mutex); return rc;
}
int async_request_cancel(async_runtime_t *r, async_handle_t h) { return cancellation(r,h,true); }
int async_cancel_requested(async_runtime_t *r, async_handle_t h) { return cancellation(r,h,false); }
async_subscription_t async_subscribe(async_runtime_t *r, async_handle_t h,
                                     async_completion_fn fn, void *ctx) {
    if (!owner(r)) return 0;
    if (!fn) { errno = EINVAL; return 0; }
    pthread_mutex_lock(&r->mutex); uint32_t source; cell_t *c = cell(r, h, &source);
    async_subscription_t subscription = 0;
    if (c && (!c->refs || c->refs == UINT32_MAX)) errno = c->refs ? EOVERFLOW : ESTALE;
    else if (c && r->edge_free == NONE) errno = EAGAIN;
    else if (c) {
        uint32_t i = r->edge_free; edge_t *e = &r->edges[i]; r->edge_free = e->next;
        uint32_t generation = e->generation + 1;
        *e = (edge_t){.generation = generation, .source = source,
            .fn = fn, .ctx = ctx, .phase = EDGE_WAITING, .prev = NONE, .next = c->edges};
        ++c->refs; ++r->stats.edges_live; subscription = handle(i, generation);
        if (c->phase == CELL_DONE) ready_append(r, i);
        else { if (c->edges != NONE) r->edges[c->edges].prev = i; c->edges = i; }
    }
    pthread_mutex_unlock(&r->mutex); return subscription;
}
int async_unsubscribe(async_runtime_t *r, async_subscription_t subscription) {
    if (!owner(r)) return -1;
    pthread_mutex_lock(&r->mutex); uint32_t low = (uint32_t)subscription; int rc = -1;
    if (!low || low > r->edge_cap) errno = ESTALE;
    else {
        uint32_t i = low - 1; edge_t *e = &r->edges[i];
        if (e->phase == EDGE_FREE || e->generation != (uint32_t)(subscription >> 32)) errno = ESTALE;
        else { edge_remove(r,i); drop(r,e->source); edge_recycle(r,i); rc = 0; }
    }
    pthread_mutex_unlock(&r->mutex); return rc;
}
static int drain(async_runtime_t *r) {
    pthread_mutex_lock(&r->mutex);
    size_t n = r->stats.mailbox_depth;
    while (n--) {
        uint32_t i = r->mail_head; cell_t *c = &r->cells[i];
        r->mail_head = c->next; if (r->mail_head == NONE) r->mail_tail = NONE;
        --r->stats.mailbox_depth; c->phase = CELL_DONE;
        uint32_t e = c->edges; c->edges = NONE;
        while (e != NONE) { uint32_t next = r->edges[e].next; ready_append(r,e); e = next; }
        if (!c->refs) { c->refs = 1; drop(r,i); }
    }
    /* Snapshot generation watermark: reentrant subscribe/unsubscribe may reuse
     * slots. Entry handles, not queue length alone, define this delivery batch. */
    size_t budget = r->stats.ready_depth;
    uint64_t watermark = r->ready_sequence;
    pthread_mutex_unlock(&r->mutex);
    int delivered = 0;
    while (budget--) {
        pthread_mutex_lock(&r->mutex);
        uint32_t i = r->ready_head;
        if (i == NONE) { pthread_mutex_unlock(&r->mutex); break; }
        edge_t copy = r->edges[i];
        /* Modular sequence ordering is safe for the bounded live ready set. */
        if ((int64_t)(copy.ready_sequence - watermark) > 0) {
            pthread_mutex_unlock(&r->mutex); break;
        }
        edge_remove(r,i); edge_recycle(r,i);
        cell_t *c = &r->cells[copy.source]; async_handle_t h = handle(copy.source,c->generation);
        async_result_t result = c->result;
        pthread_mutex_unlock(&r->mutex);
        copy.fn(r,h,&result,copy.ctx);
        pthread_mutex_lock(&r->mutex); drop(r,copy.source); ++r->stats.callbacks;
        pthread_mutex_unlock(&r->mutex); ++delivered;
    }
    return delivered;
}
int async_step(async_runtime_t *r, int timeout_ms) {
    if (!owner(r)) return -1;
    if (r->dispatching) { errno = EDEADLK; return -1; }
    r->dispatching = true;
    async_stats_t before = async_runtime_stats(r);
    int delivered = 0, rc;
    /* Always service FDs/timers once; queued work forbids sleeping. */
    rc = ev_loop_poll(r->loop, before.mailbox_depth || before.ready_depth ? 0 : timeout_ms);
    if (rc >= 0) delivered = drain(r);
    r->dispatching = false;
    return rc < 0 ? -1 : delivered;
}
int async_wait(async_runtime_t *r, async_handle_t h, async_result_t *out) {
    if (!owner(r)) return -1;
    if (!out) { errno = EINVAL; return -1; }
    if (r->dispatching) { errno = EDEADLK; return -1; }
    if (async_retain(r,h) < 0) return -1;
    int rc;
    while ((rc = async_read(r,h,out)) == 0) { if (async_step(r,-1) < 0) { rc = -1; break; } }
    int saved = errno; async_release(r,h); errno = saved; return rc < 0 ? -1 : 0;
}
