# 2811 — Rollout staleness manager: Learned minimal-witness extraction

Implement the learned minimal-witness extraction feature for the Rollout staleness manager RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Admit asynchronous training rollouts only when their behavior-policy versions and statistical assumptions remain valid.

Keep sampling versions, optimizer versions, queue delays, log probabilities, and grouped episode identities in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively reconcile rollout groups, calculate version age, and propose retain, reweight, or discard decisions under fixed policy. Role output: Produce a staleness-management root adapter, batch admission report, rejected samples, and exact policy-version accounting. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to shrink a failure of this role's program, preserving its original domain obligations and fixed failure identity. It removes candidate context or program regions, recursively checks reductions, and stores a verified failure witness separately from the normal role result. Measure size and replay reliability separately.

Train roots on delayed batches, missing behavior probabilities, mixed-version groups, and valid asynchronous trajectories. A deterministic admission oracle checks version bounds, group consistency, and required importance-weight inputs. Hold out queue-delay distributions, update rates, and mixed-version failure patterns. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: An old rollout relabeled with the current model version must not pass merely because its task succeeded. Deleting the evidence until a different error appears must be rejected as failure drift. The final witness must reproduce the original oracle mismatch, and timeout cannot establish that a removed region was unnecessary.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/swarm_accounting.c`, `src/provider_events.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/budget.py` (`UsageMeter.snapshot`). Donor baseline: Reports aggregate usage but does not associate rollouts with optimizer-policy staleness. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
