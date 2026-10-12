#include "async_pool.h"
#include <errno.h>
#include <stdlib.h>
typedef struct {
    async_runtime_t *runtime; async_handle_t output; async_pool_fn fn; void *ctx;
    int error;
} pool_work_t;
static void run(void *ctx) {
    pool_work_t *w = ctx;
    async_result_t result = w->fn(w->runtime,w->output,w->ctx);
    if (async_publish(w->runtime,w->output,result) < 0) w->error = errno;
    async_release(w->runtime,w->output);
}
int async_pool_submit(async_runtime_t *r, async_pool_fn fn, void *ctx, async_pool_ticket_t *ticket) {
    if (!r || !fn || !ticket) { errno = EINVAL; return -1; }
    if (!async_runtime_loop(r)) return -1;
    pool_work_t *w = calloc(1,sizeof(*w)); if (!w) return -1;
    w->runtime = r; w->fn = fn; w->ctx = ctx; w->output = async_cell_new(r);
    if (!w->output) { free(w); return -1; }
    if (async_retain(r,w->output) < 0) {
        int saved = errno; async_release(r,w->output); free(w); errno = saved; return -1;
    }
    dsco_task_t *task = dsco_pool_try_submit(run,w);
    if (!task) {
        int saved = errno; async_release(r,w->output); async_release(r,w->output);
        free(w); errno = saved; return -1;
    }
    *ticket = (async_pool_ticket_t){task,w->output,w}; return 0;
}
int async_pool_join(async_pool_ticket_t *ticket) {
    if (!ticket || !ticket->task || !ticket->private_work) { errno = EINVAL; return -1; }
    dsco_pool_join(ticket->task);
    pool_work_t *w = ticket->private_work; int error = w->error; free(w);
    ticket->task = NULL; ticket->private_work = NULL;
    if (error) { errno = error; return -1; } return 0;
}
