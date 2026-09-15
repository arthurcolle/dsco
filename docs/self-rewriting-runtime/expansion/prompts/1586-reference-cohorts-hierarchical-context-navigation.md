# 1586 — Empirical reference-cohort builder: Hierarchical context navigation

Implement the hierarchical context navigation feature for the Empirical reference-cohort builder RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a cohort RLM to select empirically comparable improvement attempts without excluding inconvenient failures or unsupported contexts.

External EPISODES and ELIGIBILITY_RULES retain workloads, hardware, candidate families, failed outcomes, censoring, and selection history. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs eligibility joins, recursively inspects borderline comparators, and buffers cohort estimates with exclusion reasons. Role output: Return REFERENCE_COHORT with member identities, exclusions, denominators, estimates, and transport limitations. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Build lossless external indexes over task contexts and train the root to navigate coarse descriptors into exact evidence regions. Child RLMs inspect selected subtrees; summaries guide navigation but never replace authoritative content. Record access paths and evidence references for the final artifact.

Training uses generated cohorts with known selection bias and actual local benchmark episodes including rejected candidates. Independent scripts verify membership and estimators; future development outcomes test predictive validity separately from final evaluation. Hold out algorithm families, hardware regimes, and selection mechanisms. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A survivor-only cohort must not portray three successes among ten attempts as a perfect success rate. A summary that omits a decisive exception must not erase the exception from the answer. Force a query requiring descent into a rarely accessed subtree and verify the exact underlying evidence is consumed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/learned_cost.c`, `src/cost_model.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/metacognitive_engine.py` (`MetacognitiveEngine._select_strategy`, `MetacognitiveEngine._evaluate_strategy_adjustment`). Donor baseline: Strategy selection uses fixed confidence weights and coarse success-rate thresholds. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
