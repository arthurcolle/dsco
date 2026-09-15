# 3030 — Release admission examiner: Joint model and runtime admission

Implement the joint model and runtime admission feature for the Release admission examiner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Decide whether a tested code/model pair satisfies release obligations without confusing recorded assertions with verified evidence.

Keep candidate bytes, role adapter identity, compatibility results, acceptance contracts, and unresolved failures in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively evaluate each obligation, challenge unsupported claims, and distinguish promotable, rejected, and incomplete candidate pairs. Role output: Produce a release-admission root adapter, obligation verdict, rejected claims, and reproducible qualification or rollback instructions. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role to identify incompatible model, adapter, child-policy, environment, and native-procedure versions. Store a typed admission plan, pinned tuple, receipts, and decision in feature_artifact; role_result retains the domain artifact. A separate authority validates compatibility and serializes admission against revocation.

Train roots on valid releases, forged receipts, mismatched binaries, failed hidden tests, and unsupported activation claims. An independent release harness checks actual instruction changes, retained state, contract behavior, and serving identity. Hold out native/model pairings, release failure combinations, and ABI transitions. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A process retaining its PID after exec must not count as uninterrupted in-memory code replacement. An unchanged parent generation with a revoked receipt must still fail admission. Native-improvement claims require changed instruction bytes, retained heap/session state, and no exec; a trained model's confidence alone cannot authorize activation.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/self_improve.c`, `src/hotplug.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/planning.py` (`PlannerStore._evidence`). Donor baseline: Requires nonempty evidence strings for every declared acceptance criterion before completion. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
