# 2525 — Lease fencing examiner: Counterfactual branch preference training

Implement the counterfactual branch preference training feature for the Lease fencing examiner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Detect stale worker authority before results or code candidates can alter shared runtime state after reassignment.

Expose lease epochs, worker identities, renewal receipts, candidate versions, and reordered events through REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively reconstruct authority intervals, compare each mutation with its fencing token, and derive minimal rejection witnesses. Role output: Produce a lease-audit root adapter, authority timeline, stale-write counterexample, and concrete fencing correction proposal. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Fork identical development states into isolated resettable environment snapshots or a fixed replay oracle with matched randomness. Reject comparisons when reset fidelity is uncertain. Execute alternative root programs under equal budgets, derive preferences from correctness then resource use, and train a declared preference objective on root continuations.

Train roots on partitioned workers, expired attempts, delayed completions, renewal races, and valid ownership transfers. An independent lease state machine determines which transitions and writes are authorized at each epoch. Hold out event interleavings, clock-skew patterns, and worker failure combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: An old worker completing successfully after a replacement receives the lease must still be fenced out. A shorter incorrect branch must lose to a longer correct branch. Swapping presentation order must preserve the preference, and branches with different starting states must be rejected as unmatched.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/activation_lease.c`, `src/blackboard.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/planning.py` (`PlannerStore._lease`). Donor baseline: Rejects nonrunning, mismatched, expired, or dependency-blocked goal attempts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
