#include "async_runtime.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
static int injected_wake(ev_loop_t *loop);
#define ev_loop_wake injected_wake
#include "../src/async_runtime.c"
#undef ev_loop_wake
static bool fail_wake = true;
static int injected_wake(ev_loop_t *loop) {
    if (fail_wake) { errno = EIO; return -1; }
    return ev_loop_wake(loop);
}
typedef struct { async_runtime_t *r; async_handle_t h; int rc,error; } args_t;
static void *publish(void *ctx) {
    args_t *a=ctx;
    a->rc=async_publish(a->r,a->h,(async_result_t){ASYNC_SUCCESS,0,123}); a->error=errno;
    return NULL;
}
static void unused(async_runtime_t *r, async_handle_t h, const async_result_t *v, void *ctx) {
    (void)r; (void)h; (void)v; (void)ctx;
}
int main(void) {
    async_runtime_t *r=async_runtime_new(1,1); assert(r); async_handle_t h=async_cell_new(r);
    args_t a={r,h,0,0}; pthread_t thread;
    assert(pthread_create(&thread,NULL,publish,&a)==0); assert(pthread_join(thread,NULL)==0);
    assert(a.rc==-1 && a.error==EIO); async_result_t out; assert(async_read(r,h,&out)==0);
    async_stats_t s=async_runtime_stats(r); assert(!s.publications && !s.mailbox_depth);
    fail_wake=false;
    assert(pthread_create(&thread,NULL,publish,&a)==0); assert(pthread_join(thread,NULL)==0);
    assert(a.rc==0 && async_read(r,h,&out)==1 && out.value==123);
    assert(async_release(r,h)==0); assert(async_step(r,0)==0); assert(async_runtime_free(r)==0);
    puts("PASS injected wake failure: no phantom completion; explicit producer retry accepted");
    r=async_runtime_new(1,1); assert(r);
    /* Exercise wrap retirement without billions of allocations. */
    r->cells[0].generation=UINT32_MAX-1;
    h=async_cell_new(r); assert((uint32_t)(h>>32)==UINT32_MAX);
    r->edges[0].generation=UINT32_MAX-1;
    async_subscription_t edge=async_subscribe(r,h,unused,NULL); assert(edge);
    assert((uint32_t)(edge>>32)==UINT32_MAX);
    assert(async_unsubscribe(r,edge)==0);
    assert(!async_subscribe(r,h,unused,NULL) && errno==EAGAIN);
    assert(async_release(r,h)==0); assert(!async_cell_new(r) && errno==EAGAIN);
    assert(async_runtime_free(r)==0);
    puts("PASS generation exhaustion: cell and edge slots retire, never wrap to stale identities");
}
