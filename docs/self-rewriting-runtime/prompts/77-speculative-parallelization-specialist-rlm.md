# 77 — Train an RLM for checked speculative native parallelization

Train a speculative-parallelization specialist RLM that extracts parallelism from bounded native computations whose independence depends on runtime input. Partition an owned-state computation into speculative regions with explicit read/write sets or validated versioned handles. Execute against private state, validate conflicts, and commit in declared serial order only when equivalent. On conflict, discard speculation and run the serial champion. External effects are forbidden inside speculative regions.

Store IR, dependence traces, candidate partitions, and conflict records in external REPL variables. The root receives bounded statistics, selects regions through code, and recursively delegates uncertain dependency slices to child RLMs. Retain proposed guards and observed conflicts. Return the partition and executable candidate through an environment handle with exact applicability conditions.

Train a root adapter on partition/guard trajectories using independent serial-equivalence rewards and whole-workload speed after rollback overhead. Penalize missed conflicts, excessive copying, and wasted worker time. Hold out aliasing patterns and contention distributions. Perform a bounded real parameter update and compare the trained policy with its frozen parent and always-serial baseline at matched resource limits.

Provide a minimal versioned owned-state interpreter/native wrapper if absent. Falsifying test: supply apparently disjoint records that alias through one hidden handle. Validation must detect the conflict, discard every speculative write, and return the exact serial result. A governed counter effect requested within speculation must be rejected before execution, not replayed after rollback. For a nonconflicting batch, demonstrate actual concurrent native execution and improved complete-call timing. If publishing an optimized generation, prove heap/session continuity and audited no resident exec; scheduling two unsafe callbacks concurrently is insufficient.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/vm.c`, `src/lingo_workflow.c`, `src/swarm.c`, `src/execution_kernel.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_streaming_tools.py` symbols `ToolExecutor._execute_parallel`, `ToolExecutor._execute_structured`: The donor runs declared parallel or dependency-ready calls but does not speculate over unknown native data dependencies. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
