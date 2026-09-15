# 2711 — Model drift diagnostician: Coverage-directed recursive traversal

Implement the coverage-directed recursive traversal feature for the Model drift diagnostician RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Distinguish learned-policy regression from workload drift, serving substitution, and changes in the evaluation instrument.

Expose longitudinal outcomes, task features, frozen probe results, serving identities, and scoring revisions in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively compare matched cohorts, isolate changed components, and propose discriminating replay experiments before retraining. Role output: Produce a drift-diagnosis root adapter, attribution report, matched replay plan, and justified retrain or rollback recommendation. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Represent task partitions and their dependencies as an explicit coverage graph. Train the root to traverse unresolved frontiers, dispatch independent work, and mark a node complete only when its typed result satisfies the role contract. Preserve coverage certificates with the output object.

Train roots on controlled model swaps, shifted input distributions, grader changes, and genuine capability regressions. An independent change-injection controller checks diagnosis against the known causal intervention and probe outcomes. Hold out drift types, domain transitions, and interacting model/environment changes. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A new difficult task mixture must not be mislabeled model degradation when matched-task accuracy stays constant. Drop one required region while returning a plausible aggregate. The completeness check must reject it; duplicate completion messages cannot compensate for a missing node or inflate coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/provider_profiles.c`, `src/self_improve.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py` (`evaluate`). Donor baseline: Runs a DSPy evaluator over caller-supplied development examples and a metric. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
