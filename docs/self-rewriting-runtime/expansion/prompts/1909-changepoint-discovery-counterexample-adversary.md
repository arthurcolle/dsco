# 1909 — Change-point discoverer: Role-specific counterexample adversary

Implement the role-specific counterexample adversary feature for the Change-point discoverer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a drift RLM to segment changing runtime behavior without mistaking single incidents for new operating regimes.

External OBSERVATION_STREAM and SEGMENT_STATE retain time order, covariates, missingness, policy revisions, and prior detections. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs sequential tests, recursively compares segment explanations, and buffers causal decisions using only observations available then. Role output: Return REGIME_SEGMENTS with detection times, evidence windows, uncertainty, and scoped adaptation recommendations. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train an auxiliary root policy to generate valid challenging inputs for the role's current candidate behavior. A fixed independent referee checks input validity and confirms violations. Retain diverse minimized witnesses, and evaluate the main role against non-adaptive sealed cases as well.

Training streams contain abrupt drift, gradual drift, recurring regimes, seasonal variation, isolated spikes, and workload shifts. Known simulated change schedules and independent predictive scoring validate controlled detection; real change attribution remains provisional. Hold out drift generators, time scales, and regime recurrence patterns. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: One latency spike must not trigger permanent relearning, while persistent conditional degradation must not be averaged away. A malformed input, altered reference, or induced harness failure cannot earn adversarial success. A genuine in-contract error must produce a reproducible witness tied to the exact candidate and environment revisions.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/event_stream.c`, `src/cost_model.c`, `src/self_improve.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/metacognitive_engine.py` (`MetacognitiveEngine._select_strategy`, `MetacognitiveEngine._evaluate_strategy_adjustment`). Donor baseline: Strategy selection uses fixed confidence weights and coarse success-rate thresholds. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
