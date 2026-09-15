# 709 — Native hashmap collision policy designer: Role-specific counterexample adversary

Implement the role-specific counterexample adversary feature for the Native hashmap collision policy designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native hashmap lookup, insertion, deletion, and resizing procedures that preserve key identity under adversarial collisions.

REPL `key_operations`, `bucket_layouts`, and `hash_distributions` retain complete operation histories and candidate table states. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs partition probe chains, delegate deletion/resize cases, and synthesize checked bucket-transition rules. Role output: Final `HashmapCandidate` handle contains native operations, load policy, probe invariants, and minimal collision witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train an auxiliary root policy to generate valid challenging inputs for the role's current candidate behavior. A fixed independent referee checks input validity and confirms violations. Retain diverse minimized witnesses, and evaluate the main role against non-adaptive sealed cases as well.

Train from generated collision workloads, resize events, tombstone defects, and independently labeled dictionary-equivalence traces. A fixed ordered key-value model checks exact presence, values, deletion semantics, and declared bounded probe behavior. Hold out hash families, collision clusters, resize thresholds, and operation mixtures with shared histories grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Deleting one colliding key breaks the probe chain and makes a later still-present key appear absent. A malformed input, altered reference, or induced harness failure cannot earn adversarial success. A genuine in-contract error must produce a reproducible witness tied to the exact candidate and environment revisions.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/dht_impl.c`, `src/vm.c`, `src/plan_cache.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/agents/kv.py` (`KeyValueAgent.get`, `KeyValueAgent.set`). Donor baseline: Implements dictionary-backed key-value operations; native collision and resize algorithms are not exposed. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
