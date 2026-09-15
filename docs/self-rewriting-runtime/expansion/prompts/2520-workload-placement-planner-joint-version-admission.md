# 2520 — Workload placement planner: Joint model and runtime admission

Implement the joint model and runtime admission feature for the Workload placement planner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Place role inference and training workloads where hardware, authority, latency, and data locality jointly satisfy their contracts.

Keep task graphs, device capacities, adapter residency, transport costs, and placement restrictions in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively partition placement choices, estimate transfer overhead, and verify feasible schedules before suggesting workload movement. Role output: Produce a placement root adapter, feasible device assignment, rejected alternatives, and predicted-versus-observed resource report. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role to identify incompatible model, adapter, child-policy, environment, and native-procedure versions. Store a typed admission plan, pinned tuple, receipts, and decision in feature_artifact; role_result retains the domain artifact. A separate authority validates compatibility and serializes admission against revocation.

Train roots on measured placement episodes, out-of-memory failures, locality restrictions, and resource-contention counterexamples. A deterministic capacity simulator plus executable task oracle scores feasibility and end-to-end useful completion. Hold out hardware mixes, model sizes, and topology/workload combinations from training. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A faster remote device cannot win when the role's required local-only data policy forbids that transfer. An unchanged parent generation with a revoked receipt must still fail admission. Native-improvement claims require changed instruction bytes, retained heap/session state, and no exec; a trained model's confidence alone cannot authorize activation.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/plan_optimizer.c`, `src/provider_profiles.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/pipeline.py` (`FourModelHierarchicalSwarm._route`). Donor baseline: Selects leaf or fresh tool-worker modules from requested tiers, tools, and task hints. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
