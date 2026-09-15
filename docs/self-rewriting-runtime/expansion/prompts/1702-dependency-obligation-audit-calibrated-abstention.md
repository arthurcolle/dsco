# 1702 — Dependency obligation auditor: Calibrated role abstention

Implement the calibrated role abstention feature for the Dependency obligation auditor RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an obligation RLM to verify that prerequisite completion actually supplies the evidence needed by dependent work.

External TASK_GRAPH, RESULT_OBJECTS, and REQUIRED_PREDICATES preserve dependency identities, result schemas, procedure revisions, and failure states. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs graph validation, recursively inspects evidence bindings, and buffers unsatisfied obligations rather than trusting completed labels. Role output: Return OBLIGATION_AUDIT with ready nodes, failed bindings, blocking evidence, and exact dependency provenance. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train an explicit abstention decision using development outcomes and calibrate its thresholds on a separate calibration split. Preserve task difficulty, evidence availability, and error categories. Evaluate selective accuracy together with retained coverage so refusing every task cannot appear useful.

Training includes generated DAGs, local tool receipts, stale versions, missing outputs, cycles, and mismatched schemas. Independent graph and predicate checkers determine readiness on fixtures; unsupported semantic prerequisites require explicit review. Hold out DAG topology, schema combinations, and invalidation histories. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A completed predecessor returning the wrong revision must not unblock a candidate requiring current-revision evidence. A timeout or unavailable oracle must remain unknown. Report the retained-task denominator and coverage; a high-confidence wrong answer and an always-abstain policy must both fail their predeclared acceptance conditions.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/plan_dag.c`, `src/context_fabric.c`, `src/execution_kernel.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/cognitive_architecture.py` (`ActionStack.pop_ready`, `CognitiveArchitecture._action_executor`). Donor baseline: Dependencies order actions, but outputs are not passed as typed prerequisite values. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
