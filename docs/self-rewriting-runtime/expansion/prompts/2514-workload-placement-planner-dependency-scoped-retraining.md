# 2514 — Workload placement planner: Dependency-scoped retraining

Implement the dependency-scoped retraining feature for the Workload placement planner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Place role inference and training workloads where hardware, authority, latency, and data locality jointly satisfy their contracts.

Keep task graphs, device capacities, adapter residency, transport costs, and placement restrictions in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively partition placement choices, estimate transfer overhead, and verify feasible schedules before suggesting workload movement. Role output: Produce a placement root adapter, feasible device assignment, rejected alternatives, and predicted-versus-observed resource report. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Bind training episodes and learned procedures to content and environment dependencies. When one dependency changes, select the affected development slices and train a bounded role update with replay from unaffected families. Version the dependency graph and resulting checkpoint separately.

Train roots on measured placement episodes, out-of-memory failures, locality restrictions, and resource-contention counterexamples. A deterministic capacity simulator plus executable task oracle scores feasibility and end-to-end useful completion. Hold out hardware mixes, model sizes, and topology/workload combinations from training. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A faster remote device cannot win when the role's required local-only data policy forbids that transfer. Editing content at the same path must reopen dependent learning claims. Retraining only on the changed slice must not hide regressions elsewhere, and unrelated episodes must not be relabeled stale merely to improve reported coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/plan_optimizer.c`, `src/provider_profiles.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/pipeline.py` (`FourModelHierarchicalSwarm._route`). Donor baseline: Selects leaf or fresh tool-worker modules from requested tiers, tools, and task hints. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
