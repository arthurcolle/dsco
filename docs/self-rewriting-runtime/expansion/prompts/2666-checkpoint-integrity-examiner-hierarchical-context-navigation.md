# 2666 — Checkpoint integrity examiner: Hierarchical context navigation

Implement the hierarchical context navigation feature for the Checkpoint integrity examiner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Qualify trained role artifacts by actual tensor integrity and compatibility before any runtime serves them.

Expose checkpoint shards, tensor metadata, base revisions, optimizer receipts, and adapter configuration through external REPL handles. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively inspect shard consistency, compare declared and actual parameter changes, and diagnose compatibility violations. Role output: Produce a checkpoint-integrity root adapter, tensor qualification report, compatibility verdict, and concrete recovery requirements. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Build lossless external indexes over task contexts and train the root to navigate coarse descriptors into exact evidence regions. Child RLMs inspect selected subtrees; summaries guide navigation but never replace authoritative content. Record access paths and evidence references for the final artifact.

Train roots on complete updates, truncated shards, stale indexes, nonfinite tensors, and incompatible adapter examples. Independent tensor loaders and fixed-input numerical checks decide whether artifacts are complete, changed, and reloadable. Hold out checkpoint layouts, adapter ranks, and corruption combinations from training. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A zero-byte or metadata-only checkpoint must not count as a completed neural update after trainer exit zero. A summary that omits a decisive exception must not erase the exception from the answer. Force a query requiring descent into a rarely accessed subtree and verify the exact underlying evidence is consumed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/provider.c`, `src/self_improve.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py` (`load_program`). Donor baseline: Loads a serialized DSPy program through dspy.load rather than validating neural checkpoint tensors. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
