# 80 — Train an RLM for lossless logical code-cell checkpoints

Train a checkpoint specialist RLM that preserves the logical state of a self-rewriting code-cell computation across suspension or recovery. Serialize typed values, stable continuation labels, code/ABI/schema hashes, owned-object identities, and completed-effect references. Never serialize raw instruction pointers, stack addresses, or borrowed host pointers as portable state. Reconstruct executable mappings through verified artifacts and resolve resources through explicit typed handles.

Keep live-value schemas, checkpoint candidates, continuation graphs, and restoration traces in external REPL variables. The root receives bounded state metadata, inspects selected ownership slices through code, and delegates reconstruction obligations to child RLMs. Retain proposed serializers and their rejected cases. The final environment handle identifies a checked checkpoint/reconstruction program with its supported state domain.

Train the role-owned root adapter on checkpoint-construction trajectories from bounded native/IR computations. Reward independent suffix-equivalence and complete reachable-state coverage before checkpoint size or restoration latency. Hold out object-graph shapes, cycles, and continuation locations. Perform a bounded real parameter update and compare the trained checkpoint with its frozen parent at matched checker budgets.

Implement a minimal logical continuation interface if absent. Falsifying test: checkpoint after a governed observation, restore into a deliberately different allocation layout, and require identical remaining outputs with no repeated external effect. Reject stale code hashes, unknown schema versions, missing objects, and pointer-looking values declared as handles. Checkpoint an old generation while a new one is active and restore only to a verified compatible continuation. Recovery may use a new process; label it recovery, not evidence of live same-address-space rewriting. Separately demonstrate lossless pause/resume within the resident process, preserving unrelated live session state.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/vm.c`, `src/recovery.c`, `src/execution_recovery.c`, `src/arena_alloc.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `CodeSandbox.execute`: The donor retains Python locals in memory but does not serialize typed executable continuations or validate their restoration. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
