# 71 — Train an RLM to adapt native algorithms to allocation pressure

Train an allocation-pressure specialist RLM that chooses and produces different native algorithms for the same logical operation as available memory changes. Start with a bounded transform having a fast materializing implementation and a lower-memory streaming implementation. Capture measured allocation peaks, input size, and concurrency; learn guarded applicability rather than merely changing an allocator limit. Each variant must satisfy the same externally checked output contract.

Keep pressure traces, code variants, and outcome records in REPL variables. The root sees bounded statistics, uses code to inspect pressure windows, and delegates implementation or guard analysis to child RLMs. Retain candidate decisions and counterexamples in the environment. Return a final variable identifying qualified native variants, switching predicates, and measured tradeoffs.

Train a root adapter on episodes where workload size and concurrent activity change the best algorithm. Reward verified completion under declared memory ceilings, then latency and compilation cost; account for unsuccessful attempts. Hold out workload mixtures and pressure transitions. Run a bounded actual update and compare its policy with the frozen parent and always-materialize/always-stream baselines at matched resource budgets.

Define a minimal guarded cell interface if absent. Switch only at invocation boundaries, pin in-flight calls, and include hysteresis so noisy readings cannot cause compilation thrash. Falsifying test: introduce allocator pressure during a burst and require bounded-memory completion with exact reference outputs; relieve pressure and verify the declared policy adapts without oscillating on every sample. Prove actual native variant selection, resident heap/session continuity, and no host exec. A candidate that avoids allocation by dropping records or whose estimates contradict measured peaks must be rejected.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/arena_alloc.c`, `src/cost_frontier.c`, `src/vm.c`, `src/self_improve.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/base.py` symbols `AgentStats.record_call`: The donor records operation timings and failures but does not learn native algorithm choices from memory-pressure feedback. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
