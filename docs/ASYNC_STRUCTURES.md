# Async structures

DSCO now has a reusable C completion substrate built on its native reactor.
The composition is specific to the runtime: operation admission reserves result
delivery capacity, reverse edges pin results, and success quorums release
observation without inventing cancellation of side effects.

## Structures and invariants

| Structure | Implementation | Contract |
|---|---|---|
| Retained completion arena | `async_runtime.c`, `cell_t` | Reusable slots, runtime-scoped 64-bit generation handles, copied terminal result, explicit references. Exhausted generations retire. |
| Reserved completion mailbox | Intrusive links in cells | One link per admitted operation; no allocation or queue overflow during publication; exactly one terminal publication. Cross-thread empty-to-nonempty transition wakes kqueue/epoll/pipe. |
| Reverse dependency edges | `edge_t`, intrusive waiting lists and ready deque | Source pins, constant-time detach, retained late delivery, callbacks on reactor owner outside locks. Reentrant new subscriptions wait for the next batch. |
| Success-quorum join | `async_join.c` | First success, all successes, or quorum; resolve when reached or impossible. Loser detachment does not cancel work. Nested joins propagate through reverse edges. |
| Pool completion ticket | `async_pool.c` | Reserve a result cell before dispatch to existing GCD/pthread pool. Worker owns a reference through publication; task handle must be joined. Full portable queue returns EAGAIN immediately. |

Mailbox and ready processing visit incoming completions and affected edges;
they do not scan every pending operation. The implementation uses a mutex for
publication and lifetime synchronization; it does not claim to be lock free.
The runtime preallocates cell/edge storage. A join allocates its finite input
table during construction, never during completion delivery.

There is no default execution timer. `async_wait()` blocks the owner in the
reactor until completion; other registered FDs, explicit timers, and worker
publications continue to dispatch. `async_step(rt, -1)` likewise blocks for an
event when no completion is ready. A nonnegative step argument is an explicit
observation budget and does not expire any operation.

## Worker and dependency example

```c
#include "async_pool.h"
#include "async_join.h"

static async_result_t compute(async_runtime_t *rt, async_handle_t self, void *ctx) {
    (void)ctx;
    if (async_cancel_requested(rt, self) == 1)
        return (async_result_t){ASYNC_CANCELLED, 0, 0};
    return (async_result_t){ASYNC_SUCCESS, 0, 42};
}

/* Error handling omitted here; admission can fail with EAGAIN. */
async_runtime_t *rt = async_runtime_new(128, 512);
async_pool_ticket_t a, b;
async_pool_submit(rt, compute, NULL, &a);
async_pool_submit(rt, compute, NULL, &b);
async_handle_t inputs[] = {a.output, b.output};
async_join_t *both = async_join_new(rt, inputs, 2, 2);
async_result_t result;
async_wait(rt, async_join_output(both), &result);
async_join_free(both);
async_pool_join(&a); async_pool_join(&b);
async_release(rt, a.output); async_release(rt, b.output);
/* Drain publications, including released cells still owned by the mailbox. */
while (async_runtime_stats(rt).mailbox_depth || async_runtime_stats(rt).ready_depth)
    async_step(rt, 0);
async_runtime_free(rt);
```

Readiness integrates directly through the borrowed `async_runtime_loop(rt)`:
register an FD callback, consume its readiness, unwatch before closing/reusing
the FD, then publish the appropriate success/failure result. The socket fixture
does this against a real socketpair; no worker or timer polls its status.

## Ownership and cancellation

- Runtime creation fixes the reactor owner. Subscription mutation, joins,
  stepping, pool submission and destruction are owner-only. Producers may
  retain, read, request cancellation, publish and release from other threads.
- Cell creation returns one owned reference. Give each producer its own retained
  reference before launch. A delivered callback borrows an edge pin; retain the
  handle explicitly if keeping it after callback return.
- `async_request_cancel()` sets a sticky cooperative bit. It neither interrupts
  a blocking syscall nor wakes a worker's private condition variable. The worker
  or FD/process controller must observe the bit through its own event path,
  complete cleanup, and publish `ASYNC_CANCELLED` to acknowledge cancellation.
- Unsubscribe only detaches an observer. Completion remains readable while
  another reference exists. A join owns its output; callers may read/retain it
  but must not publish into it. Destroying an unresolved join publishes its
  cancellation and detaches inputs.
- Results hold an outcome, integer code and 64-bit value. They own no pointers.
  Store large output in existing artifacts/buffer stores and use a separate
  application reference with explicitly managed lifetime.
- Join the pool ticket even after observer detachment or cancellation. Worker
  context lives through join. A nonzero `async_pool_join()` error exposes failed
  publication. Publication wake failure rejects before committing a result;
  successful side effects must not be replayed to retry result notification.
- Stop/join producers before destruction. Runtime free returns EBUSY while
  references, subscriptions, queued completions or callbacks remain. Handles
  belong to one runtime and must not be reused after runtime destruction.

## Verification and boundaries

`make test-async-structures` tests native kqueue/GCD, forced portable poll, and
the real pthread pool compiled on macOS. It exercises retained results,
generation reuse/exhaustion, duplicate publication races, saturated mailboxes,
reentrant subscription mutation, nested/quorum/impossible joins, cooperative
cancellation, portable queue saturation, admission rollback, and socket
readiness. `make test-async-structures-sanitize` runs ASan/UBSan;
`make test-async-structures-tsan` checks both worker backends with TSan.

`make async-library` produces `build/libdsco_async.a` with the reactor and pool
backends included. Consumers include the headers from `include/` and link the
archive with pthread support. `make async-demo` compiles and runs
`examples/async_graph.c` against that archive, dispatching actual pool work and
joining a dependency graph through retained completions.

The modules are registered in DSCO's build and bridge its existing worker pool.
The normal executable can strip currently unreferenced APIs; the static archive
is the usable integration artifact. These are
in-process primitives, not crash-resumable futures or a migration of all command,
MCP and provider execution paths. There is no mutation retry or new tool entry
point. Tool callbacks remain responsible for the existing capability-tier gate.
Linux native epoll execution is not verified by these macOS tests. Global pool
initialization/shutdown must be coordinated with its other users; no concurrent
shutdown guarantee is added. Historical `dsco_pool_submit()` callers retain
their portable blocking admission contract; the async bridge uses the new
`dsco_pool_try_submit()` path.
