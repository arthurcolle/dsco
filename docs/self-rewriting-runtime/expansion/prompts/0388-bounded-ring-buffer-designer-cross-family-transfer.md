# 388 — Bounded ring-buffer algorithm designer: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Bounded ring-buffer algorithm designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn bounded native ring-buffer procedures with explicit overwrite, backpressure, ordering, and consumer visibility contracts.

External `ring_operations`, `producer_batches`, and `consumer_views` retain operation sequences, capacities, and observed logical indices. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code separates wraparound epochs, delegates visibility cases, and synthesizes index arithmetic plus publication-order proposals. Role output: Final `RingImplementation` handle contains native code, capacity policy, synchronization contract, and replayable wraparound counterexamples. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Train from generated push/read histories, controlled interleavings, wrap defects, and exact model-checked outcome labels. A frozen sequential model or bounded linearizability checker verifies item identity, order, capacity behavior, and permitted loss. Hold out capacity shapes, producer counts, wrap frequencies, and delayed-consumer schedules with shared histories grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Index wrap makes a stale slot appear newly published, duplicating one sample while silently dropping another. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ring_buffer.c`, `src/swarm_reactor.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`QueueToolkit.publish`). Donor baseline: Stores priority messages in Python lists or Redis; it does not implement native ring-buffer semantics. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
