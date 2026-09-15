# 693 — Native hashmap collision policy designer: Faithful root-turn supervision

Implement the faithful root-turn supervision feature for the Native hashmap collision policy designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native hashmap lookup, insertion, deletion, and resizing procedures that preserve key identity under adversarial collisions.

REPL `key_operations`, `bucket_layouts`, and `hash_distributions` retain complete operation histories and candidate table states. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs partition probe chains, delegate deletion/resize cases, and synthesize checked bucket-transition rules. Role output: Final `HashmapCandidate` handle contains native operations, load policy, probe invariants, and minimal collision witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Export each root decision as its exact preceding visible history and that decision's generated code. Preserve environment object identities and recursive child receipts. Train only the current root target, masking earlier turns, child replies, and tool observations; verify masks after the actual renderer tokenizes each sample.

Train from generated collision workloads, resize events, tombstone defects, and independently labeled dictionary-equivalence traces. A fixed ordered key-value model checks exact presence, values, deletion semantics, and declared bounded probe behavior. Hold out hash families, collision clusters, resize thresholds, and operation mixtures with shared histories grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Deleting one colliding key breaks the probe chain and makes a later still-present key appear absent. Reject a sample whose prefix contains a later observation. A deliberately relabeled child response must receive no root target loss, and the exported sequence must replay exactly.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/dht_impl.c`, `src/vm.c`, `src/plan_cache.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/agents/kv.py` (`KeyValueAgent.get`, `KeyValueAgent.set`). Donor baseline: Implements dictionary-backed key-value operations; native collision and resize algorithms are not exposed. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
