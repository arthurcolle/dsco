# 2369 — Session recovery strategist: Capability-constrained program learning

Implement the capability-constrained program learning feature for the Session recovery strategist RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Reconstruct the next valid action after interruption without replaying effects whose outcomes are already committed or uncertain.

Keep session records, checkpoints, pending operations, effect receipts, and interrupted call trees in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively reconcile each branch against durable receipts, identify resumable state, and isolate unknown remote outcomes. Role output: Produce a recovery root adapter, resumable action plan, unresolved-outcome set, and independently verified replay receipt. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to generate useful programs within a declared host-service contract, using structured action schemas and explicit capability denials as observations. Constraints shape valid proposals, but the host still authorizes every effect independently. Evaluate task completion under different legitimate grant sets.

Use crash-injected episodes spanning pre-dispatch, post-effect, partial-stream, and checkpoint-publication boundaries as role training data. A deterministic effect ledger and replay controller score state continuity and absence of duplicated side effects. Hold out interruption boundaries, operation mixtures, and checkpoint formats from training. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A crash after remote commit but before acknowledgement must not trigger another non-idempotent execution. A recursive child requesting broader authority cannot obtain it from generated code or a rewritten schema. The root must select an allowed alternative or report inability, and a denied action cannot be recorded as completed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/execution_recovery.c`, `src/agent.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/planning.py` (`PlannerStore._recover`). Donor baseline: Recovers expired running goals or cancels them when ancestor goals are cancelled or failed. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
