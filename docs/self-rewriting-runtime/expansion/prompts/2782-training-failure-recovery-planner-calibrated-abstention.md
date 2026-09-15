# 2782 — Training failure recovery planner: Calibrated role abstention

Implement the calibrated role abstention feature for the Training failure recovery planner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Recover failed role training without mixing checkpoints, repeating committed updates, or claiming configuration validation as training progress.

Expose optimizer steps, shard publication states, device errors, run manifests, and resume metadata in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively reconcile durable training state, identify the last coherent boundary, and construct a bounded restart plan. Role output: Produce a training-recovery root adapter, coherent resume specification, rejected recovery paths, and verified parameter-continuity report. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train an explicit abstention decision using development outcomes and calibrate its thresholds on a separate calibration split. Preserve task difficulty, evidence availability, and error categories. Evaluate selective accuracy together with retained coverage so refusing every task cannot appear useful.

Train roots on device exhaustion, interrupted saves, corrupt optimizer states, preemption, and incompatible resume attempts. An independent deterministic training fixture compares resumed parameters with an uninterrupted reference within declared tolerances. Hold out failure timings, trainer layouts, and device-error combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Resuming model weights from one step with optimizer state from another must fail before further updates. A timeout or unavailable oracle must remain unknown. Report the retained-task denominator and coverage; a high-confidence wrong answer and an always-abstain policy must both fail their predeclared acceptance conditions.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/agent_interop.c`, `src/execution_recovery.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/executor.py` (`execute_one`). Donor baseline: Claims bounded work, launches a binary, and records attempt-specific execution artifacts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
