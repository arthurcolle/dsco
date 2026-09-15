# 3082 — Synthetic fixture author: Calibrated role abstention

Implement the calibrated role abstention feature for the Synthetic fixture author RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Generate adversarial executable fixtures with known answers that teach role procedures rather than memorized superficial cues.

Keep task grammars, semantic rules, seeds, required edge cases, and independent reference implementations in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively compose cases, derive answers through separate reference code, and reject fixtures whose labels depend on the candidate. Role output: Produce a fixture-authoring root adapter, deterministic task generator, sealed oracle outputs, and independent generator-validation report. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train an explicit abstention decision using development outcomes and calibrate its thresholds on a separate calibration split. Preserve task difficulty, evidence availability, and error categories. Evaluate selective accuracy together with retained coverage so refusing every task cannot appear useful.

Train roots on validated fixture generators, ambiguous cases, accidental answer leaks, and independently discovered missing boundaries. A second implementation verifies answer consistency, mutation sensitivity, determinism, and absence of hidden label channels. Hold out grammar productions, semantic combinations, and generator seed families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A generated fixture placing its expected answer inside the visible filename must fail leakage validation. A timeout or unavailable oracle must remain unknown. Report the retained-task denominator and coverage; a high-confidence wrong answer and an always-abstain policy must both fail their predeclared acceptance conditions.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/tools.c`, `src/agent_interop.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/eval_bias_reduction.py` (`calculate_detection_metrics`). Donor baseline: Computes exact normalized-label precision, recall, and F1 without generating independent task fixtures. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
