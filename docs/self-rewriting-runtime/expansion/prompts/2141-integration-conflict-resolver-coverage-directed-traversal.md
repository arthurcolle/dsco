# 2141 — Integration conflict resolver: Coverage-directed recursive traversal

Implement the coverage-directed recursive traversal feature for the Integration conflict resolver RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Propose conflict resolutions that preserve independently established behavior from concurrent agents rather than accepting whichever patch applies.

Keep base, branch diffs, interface contracts, and failing integration traces in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Programmatically partition intersecting changes, recursively compare semantic obligations, and assemble a minimal resolution with explicit unresolved conflicts. Role output: Produce a conflict-resolution root adapter plus a candidate merge, obligation matrix, and executable verification receipt. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Represent task partitions and their dependencies as an explicit coverage graph. Train the root to traverse unresolved frontiers, dispatch independent work, and mark a node complete only when its typed result satisfies the role contract. Preserve coverage certificates with the output object.

Use root episodes from competing patches, verified merged implementations, rejected textual merges, and independently replayed counterexamples. An isolated build and behavioral test oracle scores retained branch obligations, not merge cleanliness or author agreement. Hold out repository families, conflict structures, and interacting function pairs from training. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A clean textual merge that reverses one branch's bounds check must fail despite compiling. Drop one required region while returning a plausible aggregate. The completeness check must reject it; duplicate completion messages cannot compensate for a missing node or inflate coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/blackboard.c`, `src/self_improve.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/decomposition.py` (`validate_decomposition`). Donor baseline: Validates bounded goal hierarchies and combined completion dependencies before importing work. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
