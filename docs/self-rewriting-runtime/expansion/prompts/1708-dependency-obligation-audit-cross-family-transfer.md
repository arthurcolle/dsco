# 1708 — Dependency obligation auditor: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Dependency obligation auditor RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an obligation RLM to verify that prerequisite completion actually supplies the evidence needed by dependent work.

External TASK_GRAPH, RESULT_OBJECTS, and REQUIRED_PREDICATES preserve dependency identities, result schemas, procedure revisions, and failure states. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs graph validation, recursively inspects evidence bindings, and buffers unsatisfied obligations rather than trusting completed labels. Role output: Return OBLIGATION_AUDIT with ready nodes, failed bindings, blocking evidence, and exact dependency provenance. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Training includes generated DAGs, local tool receipts, stale versions, missing outputs, cycles, and mismatched schemas. Independent graph and predicate checkers determine readiness on fixtures; unsupported semantic prerequisites require explicit review. Hold out DAG topology, schema combinations, and invalidation histories. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A completed predecessor returning the wrong revision must not unblock a candidate requiring current-revision evidence. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/plan_dag.c`, `src/context_fabric.c`, `src/execution_kernel.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/cognitive_architecture.py` (`ActionStack.pop_ready`, `CognitiveArchitecture._action_executor`). Donor baseline: Dependencies order actions, but outputs are not passed as typed prerequisite values. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
