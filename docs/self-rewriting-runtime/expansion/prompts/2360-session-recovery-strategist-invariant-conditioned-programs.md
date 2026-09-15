# 2360 — Session recovery strategist: Invariant-conditioned program generation

Implement the invariant-conditioned program generation feature for the Session recovery strategist RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Reconstruct the next valid action after interruption without replaying effects whose outcomes are already committed or uncertain.

Keep session records, checkpoints, pending operations, effect receipts, and interrupted call trees in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively reconcile each branch against durable receipts, identify resumable state, and isolate unknown remote outcomes. Role output: Produce a recovery root adapter, resumable action plan, unresolved-outcome set, and independently verified replay receipt. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Attach explicit executable invariants to development tasks and train the root to carry them through decomposition, child-result consumption, and final assembly. Check invariants outside the policy, distinguishing proven bounded conditions from sampled checks and unresolved assumptions.

Use crash-injected episodes spanning pre-dispatch, post-effect, partial-stream, and checkpoint-publication boundaries as role training data. A deterministic effect ledger and replay controller score state continuity and absence of duplicated side effects. Hold out interruption boundaries, operation mixtures, and checkpoint formats from training. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A crash after remote commit but before acknowledgement must not trigger another non-idempotent execution. A candidate that satisfies visible examples while violating a required invariant on a boundary case must fail. Removing the check or replacing it with the root's assurance must not change the verdict.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/execution_recovery.c`, `src/agent.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/planning.py` (`PlannerStore._recover`). Donor baseline: Recovers expired running goals or cancels them when ancestor goals are cancelled or failed. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
