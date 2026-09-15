# 2594 — Training dataset curator: Contradiction-guided program revision

Implement the contradiction-guided program revision feature for the Training dataset curator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Choose role-training episodes that provide valid executable supervision without leaking holdout answers or amplifying duplicated failures.

Expose root trajectories, oracle outcomes, source ancestry, duplicate clusters, and licensing constraints as external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively classify sample eligibility, group related episodes, and construct balanced curricula with explicit exclusion reasons. Role output: Produce a dataset-curation root adapter, immutable split manifest, accepted root samples, and a quantified contamination audit. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Generate episodes where two independently sourced intermediate results conflict. Train the root to localize the conflicting assumptions, recursively investigate discriminating evidence, and revise dependent program steps. Retain both original claims and the evidence that resolved or preserved the disagreement.

Train roots on curated-versus-contaminated corpora, mislabeled success traces, duplicate families, and independently audited split decisions. A separate leakage checker and executable replay oracle score accepted samples and train/evaluation separation. Hold out repository ancestry groups, contamination mechanisms, and task-generation families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A successful-looking trajectory containing future tool observations in an earlier input must be excluded. Several copied supporting reports must not overwhelm one valid counterexample. If the conflict remains unresolved, the output must preserve that state rather than select whichever child answered last.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/chronicle.c`, `src/self_improve.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py` (`compile_with_gepa`). Donor baseline: Passes a caller-provided training set to DSPy GEPA without repository-aware data partitioning. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
