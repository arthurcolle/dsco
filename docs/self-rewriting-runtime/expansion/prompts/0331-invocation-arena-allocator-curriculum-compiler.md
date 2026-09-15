# 331 — Invocation arena allocator synthesizer: Executable curriculum compiler

Implement the executable curriculum compiler feature for the Invocation arena allocator synthesizer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn arena allocation procedures that exploit invocation lifetimes while preserving alignment, nested scopes, and concurrent ownership.

REPL `allocation_graphs`, `scope_traces`, and `arena_layouts` retain complete object lifetimes, chunk boundaries, and alignment requests. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs analyze scope subgraphs, delegate escape questions, and construct allocation/reset policies with explicit ownership. Role output: Final `ArenaPolicyCandidate` handle includes native allocator code, scope rules, live-reference obligations, and allocation witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Compile a task grammar with explicit difficulty axes, valid transformations, and known failure labels. Generate positive and negative episodes, retaining their originating family identifiers. Train the role's root policy on a balanced development curriculum and expose a reproducible curriculum-build command.

Train from instrumented allocation episodes, known escaping objects, alignment failures, and independently validated lifetime-preserving rewrites. A fixed ownership checker plus guarded allocator validates live-object bytes, alignment, release timing, and allocation bounds. Hold out chunk sizes, scope nesting, concurrency patterns, and alias lifetimes with related traces grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Rewinding only the newest chunk leaves older nested allocations incorrectly reusable while an outer reference remains live. Reject a generator that changes the answer without updating its reference or produces duplicate families under different seeds. Measure valid-case coverage and trained-root success separately.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/arena_alloc.c`, `src/json_util.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`CodeSandbox.execute`). Donor baseline: Retains Python locals between executions; native arena ownership and allocation policies are absent. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
