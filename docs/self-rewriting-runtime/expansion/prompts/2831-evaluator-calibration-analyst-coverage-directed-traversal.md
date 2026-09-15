# 2831 — Evaluator calibration analyst: Coverage-directed recursive traversal

Implement the coverage-directed recursive traversal feature for the Evaluator calibration analyst RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Measure whether a role evaluator's confidence and labels predict independently verified outcomes instead of reinforcing self-assessment.

Expose predictions, rubric versions, adjudicated outcomes, abstentions, and failure dispositions as external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively stratify error patterns, compute calibration diagnostics, and propose thresholds using only designated fitting data. Role output: Produce an evaluator-calibration root adapter, calibrated decision thresholds, uncertainty report, and untouched-holdout scoring receipt. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Represent task partitions and their dependencies as an explicit coverage graph. Train the root to traverse unresolved frontiers, dispatch independent work, and mark a node complete only when its typed result satisfies the role contract. Preserve coverage certificates with the output object.

Train roots on confident mistakes, valid abstentions, unavailable verification, and independently scored evaluation episodes. A fixed executable scorer calculates proper scoring rules and held-out false-acceptance rates from resolved outcomes. Hold out task families, rubric shifts, and confidence-distribution changes. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A verifier timeout must remain unknown and cannot improve accuracy by becoming zero remaining defects. Drop one required region while returning a plausible aggregate. The completeness check must reject it; duplicate completion messages cannot compensate for a missing node or inflate coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/self_improve.c`, `src/chronicle.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/eval_bias_reduction.py` (`evaluate_test_case`). Donor baseline: Computes heuristic coherence and can treat absent post-analysis as no newly detected biases. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
