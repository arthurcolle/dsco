#define _DARWIN_C_SOURCE 1
#include "async_runtime.h"
#include "async_join.h"
#include "async_pool.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static double milliseconds(clockid_t clock);

static async_result_t success(uint64_t n) { return (async_result_t){ASYNC_SUCCESS,0,n}; }
static void flush(async_runtime_t *r) {
    for (;;) {
        async_stats_t s = async_runtime_stats(r);
        if (!s.mailbox_depth && !s.ready_depth) break;
        assert(async_step(r,0) >= 0);
    }
}
typedef struct { int calls; uint64_t sum; pthread_t owner; } observed_t;
static void observe(async_runtime_t *r, async_handle_t h, const async_result_t *value, void *ctx) {
    observed_t *o = ctx; assert(pthread_equal(pthread_self(),o->owner));
    async_result_t stored; assert(async_read(r,h,&stored) == 1 && stored.value == value->value);
    ++o->calls; o->sum += value->value;
    assert(async_step(r,0) == -1 && errno == EDEADLK);
    assert(async_runtime_free(r) == -1 && errno == EBUSY);
}
static void reuse(void) {
    async_runtime_t *r = async_runtime_new(1,2); assert(r);
    async_handle_t previous = 0; async_result_t out;
    for (int i = 0; i < 10000; ++i) {
        async_handle_t h = async_cell_new(r); assert(h && h != previous);
        if (previous) assert(async_publish(r,previous,success(99)) == -1 && errno == ESTALE);
        assert(!async_cell_new(r) && errno == EAGAIN);
        assert(async_read(r,h,&out) == 0);
        assert(async_publish(r,h,success(i)) == 0);
        assert(async_publish(r,h,success(99)) == -1 && errno == EALREADY);
        assert(async_read(r,h,&out) == 1 && out.value == (uint64_t)i);
        assert(async_release(r,h) == 0);
        assert(async_read(r,h,&out) == -1 && errno == ESTALE);
        assert(!async_cell_new(r) && errno == EAGAIN); /* mailbox still owns slot */
        flush(r); previous = h;
    }
    async_stats_t stats = async_runtime_stats(r);
    assert(stats.recycled == 10000 && !stats.cells_live);
    assert(async_runtime_free(r) == 0);
    puts("PASS reusable generation arena: 10000 completions, capacity=1, stale IDs rejected");
}
static void retained(void) {
    async_runtime_t *r = async_runtime_new(2,2); assert(r);
    observed_t o = {.owner = pthread_self()}; async_handle_t h = async_cell_new(r);
    assert(async_publish(r,h,success(7)) == 0); flush(r);
    async_subscription_t s = async_subscribe(r,h,observe,&o); assert(s);
    assert(async_release(r,h) == 0); /* subscription is now sole owner */
    assert(async_step(r,-1) == 1 && o.calls == 1 && o.sum == 7);
    assert(async_unsubscribe(r,s) == -1 && errno == ESTALE);
    async_result_t out; assert(async_read(r,h,&out) == -1 && errno == ESTALE);
    assert(async_runtime_free(r) == 0);
    puts("PASS retained late result, edge pin, owner callbacks, recursive dispatch rejection");
}
typedef struct {
    async_runtime_t *r; async_handle_t h; uint64_t value; int rc, error;
    pthread_mutex_t *gate; pthread_cond_t *cond; bool *go;
} publication_t;
static void *publish_thread(void *ctx) {
    publication_t *p = ctx;
    if (p->gate) {
        pthread_mutex_lock(p->gate);
        while (!*p->go) pthread_cond_wait(p->cond,p->gate);
        pthread_mutex_unlock(p->gate);
    }
    p->rc = async_publish(p->r,p->h,success(p->value)); p->error = errno;
    assert(async_release(p->r,p->h) == 0); return NULL;
}
static void mailbox(void) {
    enum { N = 64 }; async_runtime_t *r = async_runtime_new(N,N); assert(r);
    publication_t p[N]; pthread_t threads[N]; observed_t o = {.owner=pthread_self()};
    for (int i = 0; i < N; ++i) {
        async_handle_t h = async_cell_new(r); assert(h);
        assert(async_subscribe(r,h,observe,&o)); assert(async_retain(r,h) == 0);
        p[i] = (publication_t){.r=r,.h=h,.value=(uint64_t)i};
        assert(pthread_create(&threads[i],NULL,publish_thread,&p[i]) == 0);
        assert(async_release(r,h) == 0);
    }
    for (int i = 0; i < N; ++i) { assert(pthread_join(threads[i],NULL) == 0); assert(p[i].rc == 0); }
    async_stats_t stats = async_runtime_stats(r);
    assert(stats.mailbox_depth == N && stats.wakes == 1 && !o.calls);
    assert(async_step(r,-1) == N && o.calls == N && o.sum == N*(N-1)/2);
    assert(async_runtime_free(r) == 0);
    puts("PASS 64 concurrent producers: full reserved mailbox, 1 wake, 64 owner deliveries");
}
static void race(void) {
    enum { N = 16 }; async_runtime_t *r = async_runtime_new(1,1); assert(r);
    pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER; pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    bool go = false; publication_t p[N]; pthread_t threads[N]; async_handle_t h = async_cell_new(r);
    for (int i = 0; i < N; ++i) {
        assert(async_retain(r,h) == 0);
        p[i] = (publication_t){.r=r,.h=h,.value=(uint64_t)i,.gate=&gate,.cond=&cond,.go=&go};
        assert(pthread_create(&threads[i],NULL,publish_thread,&p[i]) == 0);
    }
    pthread_mutex_lock(&gate); go = true; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&gate);
    int accepted = 0; uint64_t winner = 0;
    for (int i = 0; i < N; ++i) {
        assert(pthread_join(threads[i],NULL) == 0);
        if (!p[i].rc) { ++accepted; winner = p[i].value; }
        else assert(p[i].error == EALREADY);
    }
    async_result_t out; assert(accepted == 1 && async_read(r,h,&out) == 1 && out.value == winner);
    assert(async_release(r,h) == 0); flush(r); assert(async_runtime_free(r) == 0);
    pthread_cond_destroy(&cond); pthread_mutex_destroy(&gate);
    puts("PASS 16-way terminal race: exactly 1 publication, 15 explicit duplicate rejections");
}
typedef struct { async_subscription_t remove; async_handle_t h; observed_t *observed; int calls; } reentrant_t;
static void replace_boundary(async_runtime_t *r, async_handle_t h, const async_result_t *out, void *ctx) {
    (void)h; (void)out; reentrant_t *x = ctx; ++x->calls;
    assert(async_unsubscribe(r,x->remove) == 0);
    assert(async_subscribe(r,x->h,observe,x->observed));
}
static void detach_test(void) {
    async_runtime_t *r = async_runtime_new(1,2); assert(r);
    observed_t o = {.owner=pthread_self()}; async_handle_t h = async_cell_new(r);
    async_subscription_t old = async_subscribe(r,h,observe,&o); assert(old);
    assert(async_unsubscribe(r,old) == 0);
    assert(async_publish(r,h,success(2)) == 0); flush(r);
    reentrant_t x = {.h=h,.observed=&o};
    assert(async_subscribe(r,h,replace_boundary,&x));
    x.remove = async_subscribe(r,h,observe,&o); assert(x.remove);
    assert(async_unsubscribe(r,old) == -1 && errno == ESTALE);
    assert(async_step(r,0) == 1 && x.calls == 1 && o.calls == 0);
    assert(async_step(r,0) == 1 && o.calls == 1);
    assert(async_release(r,h) == 0); assert(async_runtime_free(r) == 0);
    puts("PASS O(1) detach and reentrant edge reuse: new subscription waits for next batch");
}
static void joins(void) {
    async_runtime_t *r = async_runtime_new(8,8); assert(r);
    async_handle_t h[3]; for (int i = 0; i < 3; ++i) h[i] = async_cell_new(r);
    async_join_t *j = async_join_new(r,h,3,2); assert(j);
    assert(async_publish(r,h[0],(async_result_t){ASYNC_FAILURE,EIO,0}) == 0);
    assert(async_publish(r,h[1],success(9)) == 0); flush(r);
    async_result_t out; assert(async_read(r,async_join_output(j),&out) == 0);
    assert(async_publish(r,h[2],success(8)) == 0);
    assert(async_wait(r,async_join_output(j),&out) == 0 && out.outcome == ASYNC_SUCCESS && out.value == 2);
    async_join_free(j); for (int i=0;i<3;++i) assert(async_release(r,h[i]) == 0); flush(r);
    for (int i=0;i<3;++i) h[i]=async_cell_new(r);
    j = async_join_new(r,h,3,3); assert(j);
    assert(async_publish(r,h[0],(async_result_t){ASYNC_FAILURE,EACCES,0}) == 0);
    assert(async_wait(r,async_join_output(j),&out) == 0 && out.outcome == ASYNC_FAILURE && out.code == EACCES);
    assert(async_cancel_requested(r,h[1]) == 0); /* observation detached, work alive */
    assert(async_read(r,h[1],&out) == 0);
    async_join_free(j); for(int i=0;i<3;++i) assert(async_release(r,h[i]) == 0); flush(r);
    for(int i=0;i<3;++i) h[i]=async_cell_new(r);
    async_join_t *first = async_join_new(r,h,2,1); assert(first);
    async_handle_t parents[2] = {async_join_output(first),h[2]};
    j = async_join_new(r,parents,2,2); assert(j);
    assert(async_publish(r,h[0],success(1)) == 0); assert(async_publish(r,h[2],success(1)) == 0);
    assert(async_wait(r,async_join_output(j),&out) == 0 && out.value == 2);
    assert(async_read(r,h[1],&out) == 0); /* first-success loser isn't killed */
    async_join_free(j); async_join_free(first);
    for(int i=0;i<3;++i) assert(async_release(r,h[i]) == 0); flush(r);
    assert(async_runtime_free(r) == 0);
    puts("PASS quorum, impossible-all failure, nested DAG, first-success without cancelling losers");
}
static void admission(void) {
    async_runtime_t *r = async_runtime_new(4,1); assert(r);
    async_handle_t h[2]={async_cell_new(r),async_cell_new(r)};
    assert(!async_join_new(r,h,2,2) && errno == EAGAIN);
    async_stats_t s = async_runtime_stats(r); assert(s.cells_live==2 && !s.edges_live);
    async_handle_t dup[2]={h[0],h[0]}; assert(!async_join_new(r,dup,2,1) && errno==EINVAL);
    async_join_t *j = async_join_new(r,h,1,1); assert(j);
    async_handle_t output=async_join_output(j); assert(async_retain(r,output)==0);
    async_join_free(j); async_result_t out;
    assert(async_read(r,output,&out)==1 && out.outcome==ASYNC_CANCELLED);
    assert(async_release(r,output)==0);
    for(int i=0;i<2;++i) assert(async_release(r,h[i])==0); flush(r);
    assert(async_runtime_free(r)==0);
    puts("PASS graph admission rollback, distinct-vote validation, retained join cancellation");
}
typedef struct { pthread_mutex_t mutex; pthread_cond_t cond; bool go; int entered; } worker_gate_t;
static async_result_t gated_work(async_runtime_t *r, async_handle_t h, void *ctx) {
    worker_gate_t *g=ctx; pthread_mutex_lock(&g->mutex);
    ++g->entered; pthread_cond_broadcast(&g->cond);
    while(!g->go) pthread_cond_wait(&g->cond,&g->mutex);
    pthread_mutex_unlock(&g->mutex);
    assert(!async_subscribe(r,h,observe,NULL) && errno==EPERM);
    return async_cancel_requested(r,h) ? (async_result_t){ASYNC_CANCELLED,ECANCELED,0} : success(42);
}
static void pool(void) {
    async_runtime_t *r=async_runtime_new(1,2); assert(r);
    worker_gate_t gate={PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,false,0};
    async_pool_ticket_t t; assert(async_pool_submit(r,gated_work,&gate,&t)==0);
    async_pool_ticket_t rejected; assert(async_pool_submit(r,gated_work,&gate,&rejected)==-1 && errno==EAGAIN);
    assert(async_request_cancel(r,t.output)==0); async_result_t out; assert(async_read(r,t.output,&out)==0);
    observed_t o={.owner=pthread_self()}; async_subscription_t edge=async_subscribe(r,t.output,observe,&o); assert(edge);
    assert(async_unsubscribe(r,edge)==0); /* detached observer doesn't affect worker */
    pthread_mutex_lock(&gate.mutex); gate.go=true; pthread_cond_signal(&gate.cond); pthread_mutex_unlock(&gate.mutex);
    assert(async_wait(r,t.output,&out)==0 && out.outcome==ASYNC_CANCELLED);
    assert(async_pool_join(&t)==0); assert(async_release(r,t.output)==0); flush(r);
    gate.go=true;
    for(int i=0;i<2000;++i) {
        assert(async_pool_submit(r,gated_work,&gate,&t)==0);
        assert(async_wait(r,t.output,&out)==0 && out.value==42);
        assert(async_pool_join(&t)==0); assert(async_release(r,t.output)==0); flush(r);
    }
    assert(async_runtime_free(r)==0); dsco_pool_global_shutdown();
    pthread_cond_destroy(&gate.cond); pthread_mutex_destroy(&gate.mutex);
    puts("PASS real pool bridge: cooperative cancel acknowledgement + 2000 reusable submissions");
}
#if defined(DSCO_POOL_FORCE_PTHREAD) || !defined(__APPLE__)
static void queue_admission(void) {
    enum { N=1025 }; assert(dsco_pool_global_init(1)==0);
    async_runtime_t *r=async_runtime_new(N+1,1); assert(r);
    worker_gate_t gate={PTHREAD_MUTEX_INITIALIZER,PTHREAD_COND_INITIALIZER,false,0};
    async_pool_ticket_t tickets[N];
    assert(async_pool_submit(r,gated_work,&gate,&tickets[0])==0);
    pthread_mutex_lock(&gate.mutex);
    while(!gate.entered) pthread_cond_wait(&gate.cond,&gate.mutex);
    pthread_mutex_unlock(&gate.mutex);
    for(int i=1;i<N;++i) assert(async_pool_submit(r,gated_work,&gate,&tickets[i])==0);
    async_pool_ticket_t rejected; double start=milliseconds(CLOCK_MONOTONIC);
    assert(async_pool_submit(r,gated_work,&gate,&rejected)==-1 && errno==EAGAIN);
    assert(milliseconds(CLOCK_MONOTONIC)-start<100);
    assert(async_runtime_stats(r).cells_live==N); /* rejected reserve rolled back */
    pthread_mutex_lock(&gate.mutex); gate.go=true; pthread_cond_broadcast(&gate.cond); pthread_mutex_unlock(&gate.mutex);
    for(int i=0;i<N;++i) { assert(async_pool_join(&tickets[i])==0); assert(async_release(r,tickets[i].output)==0); }
    flush(r); assert(async_runtime_free(r)==0); dsco_pool_global_shutdown();
    pthread_cond_destroy(&gate.cond); pthread_mutex_destroy(&gate.mutex);
    puts("PASS portable pool saturation: 1024 queued + 1 running; immediate EAGAIN, no leaked reservation");
}
#endif
typedef struct { async_runtime_t *r; async_handle_t h; int fd; } socket_ctx_t;
static void readable(int fd, ev_mask_t events, void *ctx) {
    socket_ctx_t *s=ctx; assert(events & EV_READ); char c;
    assert(read(fd,&c,1)==1 && c=='x'); assert(ev_unwatch_fd(async_runtime_loop(s->r),fd)==0);
    assert(async_publish(s->r,s->h,success(1))==0);
}
static void *socket_writer(void *ctx) {
    int fd=*(int *)ctx; struct timespec ts={0,250000000};
    while(nanosleep(&ts,&ts)<0) assert(errno==EINTR);
    assert(write(fd,"x",1)==1); return NULL;
}
static double milliseconds(clockid_t clock) { struct timespec t; assert(clock_gettime(clock,&t)==0); return t.tv_sec*1000.0+t.tv_nsec/1e6; }
static void network(void) {
    int fds[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,fds)==0);
    async_runtime_t *r=async_runtime_new(1,1); assert(r); async_handle_t h=async_cell_new(r);
    socket_ctx_t ctx={r,h,fds[0]}; assert(ev_watch_fd(async_runtime_loop(r),fds[0],EV_READ,readable,&ctx)==0);
    pthread_t writer; assert(pthread_create(&writer,NULL,socket_writer,&fds[1])==0);
    double start=milliseconds(CLOCK_MONOTONIC), cpu=milliseconds(CLOCK_THREAD_CPUTIME_ID);
    async_result_t out; assert(async_wait(r,h,&out)==0 && out.value==1);
    double elapsed=milliseconds(CLOCK_MONOTONIC)-start, used=milliseconds(CLOCK_THREAD_CPUTIME_ID)-cpu;
    ev_stats_t s=ev_loop_stats(async_runtime_loop(r));
    assert(elapsed>=150 && used<100 && s.polls==1 && s.fd_events==1);
    printf("PASS network completion: backend=%s wall_ms=%.3f owner_cpu_ms=%.3f kernel_waits=%llu\n",
        ev_loop_backend(async_runtime_loop(r)),elapsed,used,(unsigned long long)s.polls);
    assert(pthread_join(writer,NULL)==0); assert(async_release(r,h)==0); flush(r);
    assert(async_runtime_free(r)==0); close(fds[0]); close(fds[1]);
}
int main(void) {
    alarm(30); /* Only a test harness watchdog. Production has no default timer. */
    reuse(); retained(); mailbox(); race(); detach_test(); joins(); admission(); pool(); network();
#if defined(DSCO_POOL_FORCE_PTHREAD) || !defined(__APPLE__)
    queue_admission();
#endif
    puts("PASS async structures"); return 0;
}
