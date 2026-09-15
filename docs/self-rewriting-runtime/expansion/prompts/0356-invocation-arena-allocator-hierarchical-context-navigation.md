# 356 — Invocation arena allocator synthesizer: Hierarchical context navigation

Implement the hierarchical context navigation feature for the Invocation arena allocator synthesizer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn arena allocation procedures that exploit invocation lifetimes while preserving alignment, nested scopes, and concurrent ownership.

REPL `allocation_graphs`, `scope_traces`, and `arena_layouts` retain complete object lifetimes, chunk boundaries, and alignment requests. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs analyze scope subgraphs, delegate escape questions, and construct allocation/reset policies with explicit ownership. Role output: Final `ArenaPolicyCandidate` handle includes native allocator code, scope rules, live-reference obligations, and allocation witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Build lossless external indexes over task contexts and train the root to navigate coarse descriptors into exact evidence regions. Child RLMs inspect selected subtrees; summaries guide navigation but never replace authoritative content. Record access paths and evidence references for the final artifact.

Train from instrumented allocation episodes, known escaping objects, alignment failures, and independently validated lifetime-preserving rewrites. A fixed ownership checker plus guarded allocator validates live-object bytes, alignment, release timing, and allocation bounds. Hold out chunk sizes, scope nesting, concurrency patterns, and alias lifetimes with related traces grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Rewinding only the newest chunk leaves older nested allocations incorrectly reusable while an outer reference remains live. A summary that omits a decisive exception must not erase the exception from the answer. Force a query requiring descent into a rarely accessed subtree and verify the exact underlying evidence is consumed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/arena_alloc.c`, `src/json_util.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`CodeSandbox.execute`). Donor baseline: Retains Python locals between executions; native arena ownership and allocation policies are absent. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
