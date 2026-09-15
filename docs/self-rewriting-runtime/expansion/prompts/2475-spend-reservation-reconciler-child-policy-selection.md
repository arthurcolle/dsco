# 2475 — Spend reservation reconciler: Cost-aware child-policy selection

Implement the cost-aware child-policy selection feature for the Spend reservation reconciler RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Reconcile estimated, reserved, settled, and uncertain spend so concurrent role calls cannot reuse the same remaining budget.

Expose request reservations, provider usage receipts, completion states, pricing revisions, and budget limits in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively match receipts to reservations, identify duplicate settlements, and propose releases only when liability is resolved. Role output: Produce a reservation-reconciliation root adapter, settlement proposals, unresolved liabilities, and a balanced budget ledger report. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Offer a pinned pool of child policies with measured capabilities and costs. Train the root to choose a child and context slice for each subproblem while recording actual serving identities. Optimize verified role outcomes under a shared budget, preserving conditional performance data.

Train roots on concurrent admissions, lower final costs, missing usage, retries, and delayed billing events. An independent accounting state machine checks conservation of available funds and exactly-once settlement. Hold out settlement orderings, currency configurations, and provider usage-field variants. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A timed-out request that may have incurred charges must not release its entire reservation as free capacity. A cheap child that fails a rare required case must not be selected solely from its average score. Swap an endpoint's model revision and require identity failure rather than silently crediting the trained root.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/inference_cost.c`, `src/swarm_accounting.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/budget.py` (`UsageMeter.add`). Donor baseline: Accumulates input, output, cache, and per-model token usage under a lock. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
