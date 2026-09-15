# 1569 — Empirical reference-cohort builder: Evidence-based stopping policy

Implement the evidence-based stopping policy feature for the Empirical reference-cohort builder RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a cohort RLM to select empirically comparable improvement attempts without excluding inconvenient failures or unsupported contexts.

External EPISODES and ELIGIBILITY_RULES retain workloads, hardware, candidate families, failed outcomes, censoring, and selection history. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs eligibility joins, recursively inspects borderline comparators, and buffers cohort estimates with exclusion reasons. Role output: Return REFERENCE_COHORT with member identities, exclusions, denominators, estimates, and transport limitations. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train a root decision among continue, return a verified environment result, and terminate unresolved. Development episodes include misleading high confidence, repetitive verification, and unresolved obligations. Optimize task completion with explicit observation requirements and costs, using oracle results rather than self-reported confidence.

Training uses generated cohorts with known selection bias and actual local benchmark episodes including rejected candidates. Independent scripts verify membership and estimators; future development outcomes test predictive validity separately from final evaluation. Hold out algorithm families, hardware regimes, and selection mechanisms. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A survivor-only cohort must not portray three successes among ten attempts as a perfect success rate. Repeated identical child opinions cannot satisfy a missing obligation. A complete verified result should survive the stop decision unchanged; exhausted budgets must produce unresolved status instead of fabricated success.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/learned_cost.c`, `src/cost_model.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/metacognitive_engine.py` (`MetacognitiveEngine._select_strategy`, `MetacognitiveEngine._evaluate_strategy_adjustment`). Donor baseline: Strategy selection uses fixed confidence weights and coarse success-rate thresholds. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
