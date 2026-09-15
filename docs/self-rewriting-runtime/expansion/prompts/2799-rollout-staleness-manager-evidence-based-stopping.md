# 2799 — Rollout staleness manager: Evidence-based stopping policy

Implement the evidence-based stopping policy feature for the Rollout staleness manager RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Admit asynchronous training rollouts only when their behavior-policy versions and statistical assumptions remain valid.

Keep sampling versions, optimizer versions, queue delays, log probabilities, and grouped episode identities in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively reconcile rollout groups, calculate version age, and propose retain, reweight, or discard decisions under fixed policy. Role output: Produce a staleness-management root adapter, batch admission report, rejected samples, and exact policy-version accounting. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train a root decision among continue, return a verified environment result, and terminate unresolved. Development episodes include misleading high confidence, repetitive verification, and unresolved obligations. Optimize task completion with explicit observation requirements and costs, using oracle results rather than self-reported confidence.

Train roots on delayed batches, missing behavior probabilities, mixed-version groups, and valid asynchronous trajectories. A deterministic admission oracle checks version bounds, group consistency, and required importance-weight inputs. Hold out queue-delay distributions, update rates, and mixed-version failure patterns. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: An old rollout relabeled with the current model version must not pass merely because its task succeeded. Repeated identical child opinions cannot satisfy a missing obligation. A complete verified result should survive the stop decision unchanged; exhausted budgets must produce unresolved status instead of fabricated success.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/swarm_accounting.c`, `src/provider_events.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/budget.py` (`UsageMeter.snapshot`). Donor baseline: Reports aggregate usage but does not associate rollouts with optimizer-policy staleness. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
