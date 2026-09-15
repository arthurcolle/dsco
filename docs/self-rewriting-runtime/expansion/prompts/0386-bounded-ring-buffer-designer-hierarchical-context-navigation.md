# 386 — Bounded ring-buffer algorithm designer: Hierarchical context navigation

Implement the hierarchical context navigation feature for the Bounded ring-buffer algorithm designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn bounded native ring-buffer procedures with explicit overwrite, backpressure, ordering, and consumer visibility contracts.

External `ring_operations`, `producer_batches`, and `consumer_views` retain operation sequences, capacities, and observed logical indices. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code separates wraparound epochs, delegates visibility cases, and synthesizes index arithmetic plus publication-order proposals. Role output: Final `RingImplementation` handle contains native code, capacity policy, synchronization contract, and replayable wraparound counterexamples. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Build lossless external indexes over task contexts and train the root to navigate coarse descriptors into exact evidence regions. Child RLMs inspect selected subtrees; summaries guide navigation but never replace authoritative content. Record access paths and evidence references for the final artifact.

Train from generated push/read histories, controlled interleavings, wrap defects, and exact model-checked outcome labels. A frozen sequential model or bounded linearizability checker verifies item identity, order, capacity behavior, and permitted loss. Hold out capacity shapes, producer counts, wrap frequencies, and delayed-consumer schedules with shared histories grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Index wrap makes a stale slot appear newly published, duplicating one sample while silently dropping another. A summary that omits a decisive exception must not erase the exception from the answer. Force a query requiring descent into a rarely accessed subtree and verify the exact underlying evidence is consumed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ring_buffer.c`, `src/swarm_reactor.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`QueueToolkit.publish`). Donor baseline: Stores priority messages in Python lists or Redis; it does not implement native ring-buffer semantics. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
