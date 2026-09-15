# 2454 — Rate allocation strategist: Dependency-scoped retraining

Implement the dependency-scoped retraining feature for the Rate allocation strategist RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Allocate limited provider request capacity among role workloads without starving critical tasks or violating endpoint quotas.

Keep queue deadlines, token estimates, quota windows, cooldowns, and role priorities as external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively partition competing demands, simulate admissible schedules, and choose allocations robust to uncertain completion times. Role output: Produce a rate-allocation root adapter, bounded admission schedule, rejected-demand reasons, and measured fairness/performance receipt. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Bind training episodes and learned procedures to content and environment dependencies. When one dependency changes, select the affected development slices and train a bounded role update with replay from unaffected families. Version the dependency graph and resulting checkpoint separately.

Train roots on throttled endpoint episodes, burst arrivals, heterogeneous token sizes, and known fair-scheduling outcomes. An independent quota simulator scores hard-limit compliance, completed task value, lateness, and starvation. Hold out arrival distributions, quota-reset patterns, and previously unseen role mixes. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: An allocation that achieves high throughput by permanently postponing low-volume urgent work must fail. Editing content at the same path must reopen dependent learning claims. Retraining only on the changed slice must not hide regressions elsewhere, and unrelated episodes must not be relabeled stale merely to improve reported coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/provider_pool.c`, `src/swarm_reactor.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/budget.py` (`UsageMeter.snapshot`). Donor baseline: Returns aggregate and per-model token accounting in a structured snapshot. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
