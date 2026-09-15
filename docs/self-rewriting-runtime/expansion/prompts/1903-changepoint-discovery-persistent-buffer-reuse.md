# 1903 — Change-point discoverer: Learned persistent-buffer reuse

Implement the learned persistent-buffer reuse feature for the Change-point discoverer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a drift RLM to segment changing runtime behavior without mistaking single incidents for new operating regimes.

External OBSERVATION_STREAM and SEGMENT_STATE retain time order, covariates, missingness, policy revisions, and prior detections. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs sequential tests, recursively compares segment explanations, and buffers causal decisions using only observations available then. Role output: Return REGIME_SEGMENTS with detection times, evidence windows, uncertainty, and scoped adaptation recommendations. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to recognize reusable intermediate results by their context, procedure, and dependency identities. Maintain external result buffers with explicit invalidation conditions; recursive children receive scoped references. Measure saved work alongside semantic correctness, including reuse across resumed episodes.

Training streams contain abrupt drift, gradual drift, recurring regimes, seasonal variation, isolated spikes, and workload shifts. Known simulated change schedules and independent predictive scoring validate controlled detection; real change attribution remains provisional. Hold out drift generators, time scales, and regime recurrence patterns. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: One latency spike must not trigger permanent relearning, while persistent conditional degradation must not be averaged away. Change one dependency while leaving the buffer's human-readable label unchanged. The policy must recompute affected results and retain unaffected ones; stale reuse or unnecessary replay of completed effects fails.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/event_stream.c`, `src/cost_model.c`, `src/self_improve.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/metacognitive_engine.py` (`MetacognitiveEngine._select_strategy`, `MetacognitiveEngine._evaluate_strategy_adjustment`). Donor baseline: Strategy selection uses fixed confidence weights and coarse success-rate thresholds. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
