# 2379 — Runaway recursion triager: Evidence-based stopping policy

Implement the evidence-based stopping policy feature for the Runaway recursion triager RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Locate recursive branches that consume resources without advancing the task and stop only work proven redundant or invalid.

Expose call-tree lineage, repeated subproblems, progress measures, reservations, and remaining obligations in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively classify cycles and productive expansion, compare state hashes, and propose bounded cancellation or decomposition repairs. Role output: Produce a recursion-triage root adapter, branch dispositions, repaired recursive program, and complete obligation accounting. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train a root decision among continue, return a verified environment result, and terminate unresolved. Development episodes include misleading high confidence, repetitive verification, and unresolved obligations. Optimize task completion with explicit observation requirements and costs, using oracle results rather than self-reported confidence.

Train roots on genuine recursive algorithms, accidental self-delegation loops, repeated failures, and misleading progress messages. An independent task oracle and scheduler trace verify preserved required work and reduced unproductive resource consumption. Hold out recursion shapes, workload sizes, and delayed-progress patterns from training. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A large but necessary pairwise computation must not be cancelled merely because its call count is high. Repeated identical child opinions cannot satisfy a missing obligation. A complete verified result should survive the stop decision unchanged; exhausted budgets must produce unresolved status instead of fabricated success.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/swarm_reactor.c`, `src/swarm_scale.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/budget.py` (`UsageMeter.check`). Donor baseline: Raises BudgetExceededError when accumulated token usage reaches the configured ceiling. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
