# 1690 — Dependency obligation auditor: Learned context-access planning

Implement the learned context-access planning feature for the Dependency obligation auditor RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an obligation RLM to verify that prerequisite completion actually supplies the evidence needed by dependent work.

External TASK_GRAPH, RESULT_OBJECTS, and REQUIRED_PREDICATES preserve dependency identities, result schemas, procedure revisions, and failure states. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs graph validation, recursively inspects evidence bindings, and buffers unsatisfied obligations rather than trusting completed labels. Role output: Return OBLIGATION_AUDIT with ready nodes, failed bindings, blocking evidence, and exact dependency provenance. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Expose context-region descriptors and bounded reads while keeping the full input outside the root window. Train the root to generate access programs that retrieve discriminating regions and recursively examine relevant slices. Log coverage, read amplification, and every region supporting the final artifact.

Training includes generated DAGs, local tool receipts, stale versions, missing outputs, cycles, and mismatched schemas. Independent graph and predicate checkers determine readiness on fixtures; unsupported semantic prerequisites require explicit review. Hold out DAG topology, schema combinations, and invalidation histories. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A completed predecessor returning the wrong revision must not unblock a candidate requiring current-revision evidence. Plant decisive information outside the usual prefix and supply a plausible distractor near the beginning. A policy that answers from previews alone must fail the oracle, and an unbounded full-context copy must be detected.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/plan_dag.c`, `src/context_fabric.c`, `src/execution_kernel.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/cognitive_architecture.py` (`ActionStack.pop_ready`, `CognitiveArchitecture._action_executor`). Donor baseline: Dependencies order actions, but outputs are not passed as typed prerequisite values. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
