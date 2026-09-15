# 2489 — Spend reservation reconciler: Capability-constrained program learning

Implement the capability-constrained program learning feature for the Spend reservation reconciler RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Reconcile estimated, reserved, settled, and uncertain spend so concurrent role calls cannot reuse the same remaining budget.

Expose request reservations, provider usage receipts, completion states, pricing revisions, and budget limits in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively match receipts to reservations, identify duplicate settlements, and propose releases only when liability is resolved. Role output: Produce a reservation-reconciliation root adapter, settlement proposals, unresolved liabilities, and a balanced budget ledger report. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to generate useful programs within a declared host-service contract, using structured action schemas and explicit capability denials as observations. Constraints shape valid proposals, but the host still authorizes every effect independently. Evaluate task completion under different legitimate grant sets.

Train roots on concurrent admissions, lower final costs, missing usage, retries, and delayed billing events. An independent accounting state machine checks conservation of available funds and exactly-once settlement. Hold out settlement orderings, currency configurations, and provider usage-field variants. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A timed-out request that may have incurred charges must not release its entire reservation as free capacity. A recursive child requesting broader authority cannot obtain it from generated code or a rewritten schema. The root must select an allowed alternative or report inability, and a denied action cannot be recorded as completed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/inference_cost.c`, `src/swarm_accounting.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/budget.py` (`UsageMeter.add`). Donor baseline: Accumulates input, output, cache, and per-model token usage under a lock. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
