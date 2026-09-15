# 2675 — Adapter route verifier: Counterfactual branch preference training

Implement the counterfactual branch preference training feature for the Adapter route verifier RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Ensure each role and recursive tree receives its intended immutable adapter rather than a similarly named model.

Keep admission identities, serving receipts, cache keys, adapter digests, and in-flight generations in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively trace root and child routing, compare actual loaded identities, and isolate cross-role contamination. Role output: Produce an adapter-routing root policy, mismatched-request witnesses, corrected route bindings, and verified serving identity receipts. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Fork identical development states into isolated resettable environment snapshots or a fixed replay oracle with matched randomness. Reject comparisons when reset fidelity is uncertain. Execute alternative root programs under equal budgets, derive preferences from correctness then resource use, and train a declared preference objective on root continuations.

Train roots on concurrent adapter reloads, alias collisions, stale caches, and misreported backend model names. A controlled serving backend and independent artifact checker verify the exact tensors used for each request. Hold out provider protocols, reload timings, and role-to-adapter assignment patterns. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A backend echoing the requested role name while serving another adapter must be rejected. A shorter incorrect branch must lose to a longer correct branch. Swapping presentation order must preserve the preference, and branches with different starting states must be rejected as unmatched.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/provider.c`, `src/provider_pool.c`, `src/agent_profile.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/config.py` (`normalize_model_id`). Donor baseline: Maps configured aliases to canonical model strings without validating loaded adapter identity. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
