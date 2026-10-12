# Cooperative async/await in C

DSCO's existing event loop now selects **kqueue** on macOS, **epoll** on Linux,
and **poll** elsewhere. `EV_FORCE_POLL` selects the portable backend for tests.
`ev_loop_backend(loop)` reports the compiled backend.

`src/scheduler_io.c` connects scheduler I/O waits to that reactor. `sched_run()`
and `sched_run_sync()` create it lazily, keep registrations between idle waits,
and release it before returning. Readiness/error events resume matching tasks,
including multiple tasks waiting on one descriptor. Timers bound kernel waits;
ready tasks are dispatched without entering the reactor. The existing run
interval remains a maximum idle wait, not a delay imposed on ready I/O.
`sched_tick()` alone still does not drive descriptor readiness.

## Stackless continuation example

```c
#include "scheduler_async.h"
#include <unistd.h>
#include <errno.h>

typedef struct {
    sched_async_t async;  /* initialize to zero */
    scheduler_t *scheduler;
    int fd;              /* nonblocking, remains open until completion */
    char byte;
    ssize_t count;
} reader_t;

static int reader(void *arg) {
    reader_t *r = arg;
    SCHED_ASYNC_BEGIN(&r->async);
    while ((r->count = read(r->fd, &r->byte, 1)) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -errno;
        SCHED_AWAIT_FD(&r->async, r->scheduler, r->fd, EV_READ);
    }
    /* count == 0 is EOF; count == 1 is a completed read. */
    SCHED_ASYNC_END(&r->async);
}
```

Spawn with `sched_spawn()` and drive with `sched_run()`. Await helpers:

- `SCHED_AWAIT_FD(handle, scheduler, fd, EV_READ/EV_WRITE)`
- `SCHED_AWAIT_SLEEP(handle, scheduler, milliseconds)`
- `SCHED_AWAIT_TASK(handle, scheduler, task_id)`

Legacy `sched_wait_fd()` awaits readability. `sched_wait_fd_events()` specifies
read/write interest; `sched_current_events()` reports readiness and `EV_ERROR`
(hangup/EOF/backend error) inside the resumed callback. Readiness is only a hint:
retry the nonblocking operation, await again on EAGAIN, handle EOF and errors.
Registration failure resumes the waiting task with `EV_ERROR` rather than hanging.

## Limits and ownership

This is cooperative **stackless** async, not compiler-native async syntax or
stackful fibers. Values surviving an await belong in the task context, never
in automatic locals. Place each await on its own line. Do not nest awaits in
another switch; line-number continuations must not be persisted across builds.
A task completes with return 0, yields/suspends with a positive return, and fails
with a negative return. Task await resumes after completion/failure/cancellation;
inspect the awaited task's state/exit code if its result matters.

All scheduler and event-loop operations are single-threaded, including stop and
cancellation. Cross-thread wakeup/post is **not implemented**. Blocking tool,
provider, file or CPU work still blocks its executing thread; kernel readiness
alone cannot make those functions async. No wholesale tool/swarm migration is
claimed. A future cross-thread completion bridge needs a synchronized queue and
wakeup descriptor (pipe/eventfd or kqueue user event), with explicit ownership.

Keep descriptors open until resumed or cancelled; do not close/reuse an awaited
fd from another task while it is suspended. The low-level event loop requires
unwatch before close/reuse. Do not recursively run/poll/free it from callbacks.
The current limits remain 256 descriptors/tasks and 64 event-loop timers.

## Verification

`make test-scheduler-async` exercises native and forced-poll event loops plus
scheduler integration: read/write interest, real EAGAIN retry, shared descriptor,
timers, task await, EOF, invalid descriptor and cancellation. Also run
`make test_ooda_calibration` to verify existing public task-state writers.
Kernel readiness targets idle I/O efficiency; it does **not** establish a
20-million-per-second no-op dispatch rate.
