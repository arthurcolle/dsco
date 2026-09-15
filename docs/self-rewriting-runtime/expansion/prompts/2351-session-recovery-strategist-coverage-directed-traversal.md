# 2351 — Session recovery strategist: Coverage-directed recursive traversal

Implement the coverage-directed recursive traversal feature for the Session recovery strategist RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Reconstruct the next valid action after interruption without replaying effects whose outcomes are already committed or uncertain.

Keep session records, checkpoints, pending operations, effect receipts, and interrupted call trees in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively reconcile each branch against durable receipts, identify resumable state, and isolate unknown remote outcomes. Role output: Produce a recovery root adapter, resumable action plan, unresolved-outcome set, and independently verified replay receipt. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Represent task partitions and their dependencies as an explicit coverage graph. Train the root to traverse unresolved frontiers, dispatch independent work, and mark a node complete only when its typed result satisfies the role contract. Preserve coverage certificates with the output object.

Use crash-injected episodes spanning pre-dispatch, post-effect, partial-stream, and checkpoint-publication boundaries as role training data. A deterministic effect ledger and replay controller score state continuity and absence of duplicated side effects. Hold out interruption boundaries, operation mixtures, and checkpoint formats from training. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A crash after remote commit but before acknowledgement must not trigger another non-idempotent execution. Drop one required region while returning a plausible aggregate. The completeness check must reject it; duplicate completion messages cannot compensate for a missing node or inflate coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/execution_recovery.c`, `src/agent.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/planning.py` (`PlannerStore._recover`). Donor baseline: Recovers expired running goals or cancels them when ancestor goals are cancelled or failed. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
