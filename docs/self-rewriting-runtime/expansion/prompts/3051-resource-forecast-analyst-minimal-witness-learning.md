# 3051 — Resource forecast analyst: Learned minimal-witness extraction

Implement the learned minimal-witness extraction feature for the Resource forecast analyst RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Forecast role training and recursive execution resources using measured cohorts, uncertainty, and correct dimensional relationships.

Expose workload features, observed tokens, peak memory, device time, pricing units, and comparable episodes in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively decompose demand, estimate intervals from eligible cohorts, and identify measurements dominating forecast uncertainty. Role output: Produce a resource-forecast root adapter, units-checked bounds, cohort qualification report, and predicted-versus-observed resource receipt. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to shrink a failure of this role's program, preserving its original domain obligations and fixed failure identity. It removes candidate context or program regions, recursively checks reductions, and stores a verified failure witness separately from the normal role result. Measure size and replay reliability separately.

Train roots on actual resource receipts, capacity failures, censored runs, and misleading average-only summaries. A held-out execution meter scores forecast error, interval coverage, and violations of declared hard limits. Hold out device types, input-length regimes, and recursion-topology combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Bytes per request multiplied by requests per second is bandwidth, not memory without a retention duration. Deleting the evidence until a different error appears must be rejected as failure drift. The final witness must reproduce the original oracle mismatch, and timeout cannot establish that a removed region was unnecessary.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/learned_cost.c`, `src/cost_model.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/budget.py` (`UsageMeter.total_tokens`). Donor baseline: Computes total usage including input, output, cache-read, and cache-write tokens. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
