# 3056 — Resource forecast analyst: Hierarchical context navigation

Implement the hierarchical context navigation feature for the Resource forecast analyst RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Forecast role training and recursive execution resources using measured cohorts, uncertainty, and correct dimensional relationships.

Expose workload features, observed tokens, peak memory, device time, pricing units, and comparable episodes in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively decompose demand, estimate intervals from eligible cohorts, and identify measurements dominating forecast uncertainty. Role output: Produce a resource-forecast root adapter, units-checked bounds, cohort qualification report, and predicted-versus-observed resource receipt. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Build lossless external indexes over task contexts and train the root to navigate coarse descriptors into exact evidence regions. Child RLMs inspect selected subtrees; summaries guide navigation but never replace authoritative content. Record access paths and evidence references for the final artifact.

Train roots on actual resource receipts, capacity failures, censored runs, and misleading average-only summaries. A held-out execution meter scores forecast error, interval coverage, and violations of declared hard limits. Hold out device types, input-length regimes, and recursion-topology combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Bytes per request multiplied by requests per second is bandwidth, not memory without a retention duration. A summary that omits a decisive exception must not erase the exception from the answer. Force a query requiring descent into a rarely accessed subtree and verify the exact underlying evidence is consumed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/learned_cost.c`, `src/cost_model.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/budget.py` (`UsageMeter.total_tokens`). Donor baseline: Computes total usage including input, output, cache-read, and cache-write tokens. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
