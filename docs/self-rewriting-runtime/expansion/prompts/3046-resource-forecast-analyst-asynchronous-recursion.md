# 3046 — Resource forecast analyst: Learned asynchronous recursion

Implement the learned asynchronous recursion feature for the Resource forecast analyst RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Forecast role training and recursive execution resources using measured cohorts, uncertainty, and correct dimensional relationships.

Expose workload features, observed tokens, peak memory, device time, pricing units, and comparable episodes in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively decompose demand, estimate intervals from eligible cohorts, and identify measurements dominating forecast uncertainty. Role output: Produce a resource-forecast root adapter, units-checked bounds, cohort qualification report, and predicted-versus-observed resource receipt. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Expose bounded futures for recursive calls and train submit, await, batch, and cancel decisions on controlled completion distributions. Reservations span all descendants. Keep completed result identities stable and ensure training and inference scheduling never block the terminal input handler.

Train roots on actual resource receipts, capacity failures, censored runs, and misleading average-only summaries. A held-out execution meter scores forecast error, interval coverage, and violations of declared hard limits. Hold out device types, input-length regimes, and recursion-topology combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Bytes per request multiplied by requests per second is bandwidth, not memory without a retention duration. Delay a required child indefinitely while delivering other results out of order. The system must remain responsive, preserve partial work, and report unresolved completeness; dropping the slow child cannot manufacture a successful fast result.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/learned_cost.c`, `src/cost_model.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/budget.py` (`UsageMeter.total_tokens`). Donor baseline: Computes total usage including input, output, cache-read, and cache-write tokens. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
