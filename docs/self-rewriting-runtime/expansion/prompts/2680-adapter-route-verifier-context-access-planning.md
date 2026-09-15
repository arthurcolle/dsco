# 2680 — Adapter route verifier: Learned context-access planning

Implement the learned context-access planning feature for the Adapter route verifier RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Ensure each role and recursive tree receives its intended immutable adapter rather than a similarly named model.

Keep admission identities, serving receipts, cache keys, adapter digests, and in-flight generations in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively trace root and child routing, compare actual loaded identities, and isolate cross-role contamination. Role output: Produce an adapter-routing root policy, mismatched-request witnesses, corrected route bindings, and verified serving identity receipts. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Expose context-region descriptors and bounded reads while keeping the full input outside the root window. Train the root to generate access programs that retrieve discriminating regions and recursively examine relevant slices. Log coverage, read amplification, and every region supporting the final artifact.

Train roots on concurrent adapter reloads, alias collisions, stale caches, and misreported backend model names. A controlled serving backend and independent artifact checker verify the exact tensors used for each request. Hold out provider protocols, reload timings, and role-to-adapter assignment patterns. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A backend echoing the requested role name while serving another adapter must be rejected. Plant decisive information outside the usual prefix and supply a plausible distractor near the beginning. A policy that answers from previews alone must fail the oracle, and an unbounded full-context copy must be detected.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/provider.c`, `src/provider_pool.c`, `src/agent_profile.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/config.py` (`normalize_model_id`). Donor baseline: Maps configured aliases to canonical model strings without validating loaded adapter identity. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
