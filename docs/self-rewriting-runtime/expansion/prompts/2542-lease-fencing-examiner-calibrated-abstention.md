# 2542 — Lease fencing examiner: Calibrated role abstention

Implement the calibrated role abstention feature for the Lease fencing examiner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Detect stale worker authority before results or code candidates can alter shared runtime state after reassignment.

Expose lease epochs, worker identities, renewal receipts, candidate versions, and reordered events through REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively reconstruct authority intervals, compare each mutation with its fencing token, and derive minimal rejection witnesses. Role output: Produce a lease-audit root adapter, authority timeline, stale-write counterexample, and concrete fencing correction proposal. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train an explicit abstention decision using development outcomes and calibrate its thresholds on a separate calibration split. Preserve task difficulty, evidence availability, and error categories. Evaluate selective accuracy together with retained coverage so refusing every task cannot appear useful.

Train roots on partitioned workers, expired attempts, delayed completions, renewal races, and valid ownership transfers. An independent lease state machine determines which transitions and writes are authorized at each epoch. Hold out event interleavings, clock-skew patterns, and worker failure combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: An old worker completing successfully after a replacement receives the lease must still be fenced out. A timeout or unavailable oracle must remain unknown. Report the retained-task denominator and coverage; a high-confidence wrong answer and an always-abstain policy must both fail their predeclared acceptance conditions.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/activation_lease.c`, `src/blackboard.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/planning.py` (`PlannerStore._lease`). Donor baseline: Rejects nonrunning, mismatched, expired, or dependency-blocked goal attempts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
