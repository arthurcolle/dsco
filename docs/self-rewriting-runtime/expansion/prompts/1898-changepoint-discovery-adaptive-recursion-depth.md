# 1898 — Change-point discoverer: Adaptive recursion-depth policy

Implement the adaptive recursion-depth policy feature for the Change-point discoverer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a drift RLM to segment changing runtime behavior without mistaking single incidents for new operating regimes.

External OBSERVATION_STREAM and SEGMENT_STATE retain time order, covariates, missingness, policy revisions, and prior detections. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs sequential tests, recursively compares segment explanations, and buffers causal decisions using only observations available then. Role output: Return REGIME_SEGMENTS with detection times, evidence windows, uncertainty, and scoped adaptation recommendations. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create development instances whose useful decomposition ranges from direct environment computation to nested child RLMs. Train the root to decide between a leaf call and another recursive environment based on task structure and remaining budget. Record the actual tree rather than a declared depth.

Training streams contain abrupt drift, gradual drift, recurring regimes, seasonal variation, isolated spikes, and workload shifts. Known simulated change schedules and independent predictive scoring validate controlled detection; real change attribution remains provisional. Hold out drift generators, time scales, and regime recurrence patterns. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: One latency spike must not trigger permanent relearning, while persistent conditional degradation must not be averaged away. A simple case must not gain reward from unnecessary recursion. A nested case must preserve dependent intermediate results, while attempts to exceed the host depth limit fail without changing that limit.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/event_stream.c`, `src/cost_model.c`, `src/self_improve.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/metacognitive_engine.py` (`MetacognitiveEngine._select_strategy`, `MetacognitiveEngine._evaluate_strategy_adjustment`). Donor baseline: Strategy selection uses fixed confidence weights and coarse success-rate thresholds. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
