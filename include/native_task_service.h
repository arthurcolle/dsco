#ifndef DSCO_NATIVE_TASK_SERVICE_H
#define DSCO_NATIVE_TASK_SERVICE_H
#include <stdio.h>
#include <stdbool.h>
#define NATIVE_TASK_SERVICE_WORKERS 4
/* Starts a fixed, bounded pool for the existing local factory queue. Calls are
 * serialized and idempotent; an active pool keeps its original output stream.
 * Failure joins any partially created pool without claiming queued work.
 * Results may complete out of order; the window model rejects stale results. */
bool native_task_service_start(FILE *out);
/* Finish captured requests and join all workers; do not drain queued work.
 * A stop/dequeue race can capture one last request per worker. Pending requests
 * remain available on restart. Wakeups have a 100 ms timed-wait fallback (not
 * a hard shutdown deadline: execution and notifications must also finish).
 * Keep out alive until stop returns. Do not call start/stop from a worker or
 * while holding locks needed by window completion/compositor notification. */
void native_task_service_stop(void);
int native_workbench_run(void);
#endif
