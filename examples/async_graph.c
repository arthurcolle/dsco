#include "async_pool.h"
#include "async_join.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Pure computation only. Execution of tools requires the capability gate. */
static async_result_t hash_text(async_runtime_t *r, async_handle_t self, void *ctx) {
    const unsigned char *s = ctx; uint64_t value = UINT64_C(14695981039346656037);
    while (*s) {
        if (async_cancel_requested(r,self) == 1)
            return (async_result_t){ASYNC_CANCELLED,ECANCELED,0};
        value = (value ^ *s++) * UINT64_C(1099511628211);
    }
    return (async_result_t){ASYNC_SUCCESS,0,value};
}
static void delivered(async_runtime_t *r, async_handle_t h, const async_result_t *value, void *ctx) {
    (void)r; (void)h;
    printf("owner received %s: outcome=%d value=%llu\n",(char *)ctx,value->outcome,
        (unsigned long long)value->value);
}
int main(void) {
    async_runtime_t *r = async_runtime_new(8,16); if (!r) { perror("runtime"); return 1; }
    async_pool_ticket_t tickets[3] = {0}; size_t submitted = 0; int rc = 1;
    char *texts[] = {"operation identity", "retained result", "dependency completion"};
    async_handle_t inputs[3]; async_join_t *all = NULL;
    for (; submitted < 3; ++submitted) {
        if (async_pool_submit(r,hash_text,texts[submitted],&tickets[submitted]) < 0) goto cleanup;
        inputs[submitted] = tickets[submitted].output;
    }
    all = async_join_new(r,inputs,3,3); if (!all) goto cleanup;
    for (size_t i=0;i<3;++i) if (!async_subscribe(r,inputs[i],delivered,texts[i])) goto cleanup;
    async_result_t result;
    if (async_wait(r,async_join_output(all),&result)<0) goto cleanup;
    printf("%s: all dependency results retained; successes=%llu\n",
        ev_loop_backend(async_runtime_loop(r)),(unsigned long long)result.value);
    rc = result.outcome == ASYNC_SUCCESS ? 0 : 1;
cleanup:
    if (rc) fprintf(stderr,"async graph failed: %s\n",strerror(errno));
    if (all) async_join_free(all);
    for(size_t i=0;i<submitted;++i) {
        if (async_pool_join(&tickets[i])<0) { perror("worker publication"); rc=1; }
        async_release(r,tickets[i].output);
    }
    for (;;) {
        async_stats_t stats=async_runtime_stats(r);
        if (!stats.mailbox_depth && !stats.ready_depth) break;
        if (async_step(r,0)<0) { perror("reactor drain"); return 1; }
    }
    if (async_runtime_free(r)<0) { perror("runtime destruction"); rc=1; }
    dsco_pool_global_shutdown(); return rc;
}
