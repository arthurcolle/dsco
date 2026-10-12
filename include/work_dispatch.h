#ifndef DSCO_WORK_DISPATCH_H
#define DSCO_WORK_DISPATCH_H

#include <stdbool.h>

#include "goal_queue.h"
#include "work_unit.h"

/* Shared pure predicate: planning must never call the draining dispatcher. */
static inline bool work_dispatch_unit_is_mechanical(const work_unit_t *w) {
    return w && w->kind == WORK_UNIT_TOOL && w->tool && w->tool[0];
}

/* Mechanical work dispatcher.
 *
 * The typed-work seam (goal_queue <-> work_unit_t <-> work_result_t) carries
 * everything a tool-kind unit needs to run: tool name, input JSON, acceptance
 * command. Yet execution still cost a full model turn per unit — the model
 * read the leased task out of its prompt, re-issued the same tool call the
 * decompose step had already specified, and narrated the outcome back.
 *
 * This dispatcher closes that gap: after each lease, tool-kind units execute
 * directly through tools_execute_for_tier() (the Immune System gate — the
 * dispatcher does NOT bypass governance; in governed postures a denied tool
 * fails the unit like any other error). The typed result is captured from the
 * work_result side-channel, delivered through goal_queue_deliver_result, and
 * the acceptance command decides completion. The loop drains until the queue
 * offers work that genuinely needs a model: prompt-kind units, plan-lane
 * tasks, or anything without a mechanical execution path.
 *
 * Model turns become what they should be: decomposition and judgment. */

typedef struct {
    int executed;      /* tool units run mechanically                    */
    int completed;     /* accepted (acceptance_cmd exit 0 or ok result)  */
    int requeued;      /* rejected, attempts remain                      */
    int failed;        /* rejected, attempts exhausted                   */
    bool model_needed; /* queue now heads a task requiring inference     */
} work_dispatch_stats_t;

/* Drain every mechanically executable unit at the head of the queue.
 * `tier` is the ambient trust tier for tool execution. Returns stats;
 * dispatch stops at the first task that needs a model turn (that task stays
 * leased for the normal prompt path) or when the queue is empty. */
work_dispatch_stats_t work_dispatch_drain(goal_queue_t *q, const char *tier);

/* Whether the currently leased task can be executed without a model turn. */
bool work_dispatch_can_execute(const goal_queue_t *q);

/* ── Fan-out ─────────────────────────────────────────────────────────────
 * The serial drain handles one leased unit at a time and cannot overlap
 * independent work. work_dispatch_fanout() schedules ALL queued work-lane
 * units whose dependencies are satisfied:
 *
 *   - tool-kind units run in-process, immediately (they are cheap C calls;
 *     parallelizing them buys nothing and risks write races on shared files);
 *   - prompt-kind units with a work_unit fan out as swarm workers, each with
 *     its own provider/model/persona from the unit — truly concurrent
 *     inference across lanes;
 *   - a unit whose depends_on names an incomplete task stays queued until its
 *     edges clear, then joins the next wave.
 *
 * Waves repeat until nothing is schedulable. Worker results are delivered
 * through goal_queue_deliver_result exactly like mechanical results:
 * acceptance_cmd still decides, retries still apply. Prompt-kind units
 * WITHOUT a work_unit still require the coordinator model turn (they have no
 * routing of their own) and are left for the prompt path. */

typedef struct {
    int tool_executed;   /* in-process mechanical units                  */
    int workers_spawned; /* swarm children launched for prompt units     */
    int workers_done;    /* workers whose results were delivered         */
    int completed;
    int requeued;
    int failed;
    int deferred_deps;   /* units skipped because dependencies pending   */
    bool model_needed;
} work_fanout_stats_t;

/* Fan out every schedulable unit; block up to `worker_timeout_ms` for swarm
 * workers of the final wave (0 = don't wait; results collected next call). */
work_fanout_stats_t work_dispatch_fanout(goal_queue_t *q, const char *tier,
                                         int max_workers, int worker_timeout_ms);

#endif /* DSCO_WORK_DISPATCH_H */
