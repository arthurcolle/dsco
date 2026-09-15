# 1959 — Critical-path evidence analyst: Evidence-based stopping policy

Implement the evidence-based stopping policy feature for the Critical-path evidence analyst RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a critical-path RLM to identify which dependency delays actually constrain end-to-end completion rather than summing durations.

External EXECUTION_DAG and TIMING_INTERVALS retain dependency edges, overlap, queueing, clock domains, and missing spans. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs DAG timing calculations, recursively resolves uncertain edges, and buffers path alternatives with sensitivity to assumptions. Role output: Return CRITICAL_PATH_REPORT with checked edges, delay attribution, uncertainty bounds, and measurable optimization opportunities. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train a root decision among continue, return a verified environment result, and terminate unresolved. Development episodes include misleading high confidence, repetitive verification, and unresolved obligations. Optimize task completion with explicit observation requirements and costs, using oracle results rather than self-reported confidence.

Training uses generated schedules and owned concurrent fixtures with overlap, contention, delayed readiness, and incomplete instrumentation. An independent scheduler oracle checks controlled makespans and critical paths; uncertain clocks require interval bounds. Hold out DAG shapes, concurrency limits, and clock-error regimes. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: The longest individual task need not be critical when another dependency chain determines completion. Repeated identical child opinions cannot satisfy a missing obligation. A complete verified result should survive the stop decision unchanged; exhausted budgets must produce unresolved status instead of fabricated success.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/plan_dag.c`, `src/event_stream.c`, `src/chronicle.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/cognitive_architecture.py` (`ActionStack.pop_ready`, `CognitiveArchitecture._action_executor`). Donor baseline: Dependencies order actions, but outputs are not passed as typed prerequisite values. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
