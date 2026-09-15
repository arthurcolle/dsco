# 09 — Transactional migration of resident heap state

Allow an improved native DSCO function to change its owned heap-state schema without restarting or corrupting ongoing work. Represent mutable-cell state through explicit schema identifiers and bounded handles, not raw pointers serialized across generations. Define a migration contract that consumes an immutable old-state snapshot and creates a separate candidate state arena with checked ownership and reference relationships.

The migration runs in a prepare phase with no external effects. Validate required fields, ranges, reference integrity, allocation budget, and the new function's state invariants before activation. Commit the code-generation and state-root pair atomically. On failure, discard the candidate arena and leave the active pair untouched. Old in-flight calls retain their original code and state snapshot according to a documented concurrency policy; a migration cannot reinterpret their memory beneath them.

Fence and drain writers before snapshot through commit; reject stale writes. Test a delayed writer. Rollback after new writes must migrate current state losslessly, never silently restore an obsolete snapshot.

Demonstrate a stateful scorer that changes from a flat running-average record to a versioned record containing count, sum, and bounded calibration bins. Specify numeric conversions, missing-field defaults, and when a reverse migration is possible. Rollback must retain an appropriate compatible state or explicitly reject reversal after irreversible schema changes; do not promise arbitrary state rollback. Provide a minimal local state descriptor if none exists.

Falsifying test: migrate populated state while a version-A call is paused, activate B, and verify A completes using its original layout while B reads the transformed values correctly. Inject allocation failure, malformed references, numeric overflow, and an invariant violation during migration. Every failed preparation must preserve the old state hash and observable results. Track allocations to prove discarded candidate arenas and retired old state are eventually reclaimed, with no code/state generation mismatch.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/arena_alloc.c`, `include/arena_alloc.h`, `src/plugin.c`, `src/vm.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `CodeSandbox.execute`, `MetaToolRegistry.register_code`. Observed donor behavior: The donor retains shared Python locals across executions but supplies no schema-versioned state migration or rollback transaction.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
