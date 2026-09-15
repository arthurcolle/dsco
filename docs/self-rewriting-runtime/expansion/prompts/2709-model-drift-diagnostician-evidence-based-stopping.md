# 2709 — Model drift diagnostician: Evidence-based stopping policy

Implement the evidence-based stopping policy feature for the Model drift diagnostician RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Distinguish learned-policy regression from workload drift, serving substitution, and changes in the evaluation instrument.

Expose longitudinal outcomes, task features, frozen probe results, serving identities, and scoring revisions in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively compare matched cohorts, isolate changed components, and propose discriminating replay experiments before retraining. Role output: Produce a drift-diagnosis root adapter, attribution report, matched replay plan, and justified retrain or rollback recommendation. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train a root decision among continue, return a verified environment result, and terminate unresolved. Development episodes include misleading high confidence, repetitive verification, and unresolved obligations. Optimize task completion with explicit observation requirements and costs, using oracle results rather than self-reported confidence.

Train roots on controlled model swaps, shifted input distributions, grader changes, and genuine capability regressions. An independent change-injection controller checks diagnosis against the known causal intervention and probe outcomes. Hold out drift types, domain transitions, and interacting model/environment changes. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A new difficult task mixture must not be mislabeled model degradation when matched-task accuracy stays constant. Repeated identical child opinions cannot satisfy a missing obligation. A complete verified result should survive the stop decision unchanged; exhausted budgets must produce unresolved status instead of fabricated success.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/provider_profiles.c`, `src/self_improve.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py` (`evaluate`). Donor baseline: Runs a DSPy evaluator over caller-supplied development examples and a metric. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
