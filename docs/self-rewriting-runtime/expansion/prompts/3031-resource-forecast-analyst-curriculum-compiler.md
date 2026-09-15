# 3031 — Resource forecast analyst: Executable curriculum compiler

Implement the executable curriculum compiler feature for the Resource forecast analyst RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Forecast role training and recursive execution resources using measured cohorts, uncertainty, and correct dimensional relationships.

Expose workload features, observed tokens, peak memory, device time, pricing units, and comparable episodes in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively decompose demand, estimate intervals from eligible cohorts, and identify measurements dominating forecast uncertainty. Role output: Produce a resource-forecast root adapter, units-checked bounds, cohort qualification report, and predicted-versus-observed resource receipt. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Compile a task grammar with explicit difficulty axes, valid transformations, and known failure labels. Generate positive and negative episodes, retaining their originating family identifiers. Train the role's root policy on a balanced development curriculum and expose a reproducible curriculum-build command.

Train roots on actual resource receipts, capacity failures, censored runs, and misleading average-only summaries. A held-out execution meter scores forecast error, interval coverage, and violations of declared hard limits. Hold out device types, input-length regimes, and recursion-topology combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Bytes per request multiplied by requests per second is bandwidth, not memory without a retention duration. Reject a generator that changes the answer without updating its reference or produces duplicate families under different seeds. Measure valid-case coverage and trained-root success separately.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/learned_cost.c`, `src/cost_model.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/budget.py` (`UsageMeter.total_tokens`). Donor baseline: Computes total usage including input, output, cache-read, and cache-write tokens. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
