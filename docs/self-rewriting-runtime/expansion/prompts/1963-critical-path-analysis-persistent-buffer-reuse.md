# 1963 — Critical-path evidence analyst: Learned persistent-buffer reuse

Implement the learned persistent-buffer reuse feature for the Critical-path evidence analyst RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a critical-path RLM to identify which dependency delays actually constrain end-to-end completion rather than summing durations.

External EXECUTION_DAG and TIMING_INTERVALS retain dependency edges, overlap, queueing, clock domains, and missing spans. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs DAG timing calculations, recursively resolves uncertain edges, and buffers path alternatives with sensitivity to assumptions. Role output: Return CRITICAL_PATH_REPORT with checked edges, delay attribution, uncertainty bounds, and measurable optimization opportunities. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to recognize reusable intermediate results by their context, procedure, and dependency identities. Maintain external result buffers with explicit invalidation conditions; recursive children receive scoped references. Measure saved work alongside semantic correctness, including reuse across resumed episodes.

Training uses generated schedules and owned concurrent fixtures with overlap, contention, delayed readiness, and incomplete instrumentation. An independent scheduler oracle checks controlled makespans and critical paths; uncertain clocks require interval bounds. Hold out DAG shapes, concurrency limits, and clock-error regimes. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: The longest individual task need not be critical when another dependency chain determines completion. Change one dependency while leaving the buffer's human-readable label unchanged. The policy must recompute affected results and retain unaffected ones; stale reuse or unnecessary replay of completed effects fails.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/plan_dag.c`, `src/event_stream.c`, `src/chronicle.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/cognitive_architecture.py` (`ActionStack.pop_ready`, `CognitiveArchitecture._action_executor`). Donor baseline: Dependencies order actions, but outputs are not passed as typed prerequisite values. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
