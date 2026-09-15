# 2878 — Multiagent handoff planner: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Multiagent handoff planner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Transfer unfinished role work with enough executable state for another agent to continue without repeating settled actions.

Keep context handles, completed obligations, pending effects, candidate identities, and unresolved questions in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively summarize branch state by dependency, preserve exact references, and construct a minimal resumable handoff contract. Role output: Produce a handoff root adapter, receiver-ready state artifact, unresolved-action ledger, and successful independent continuation evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Train roots on interrupted collaborations, complete handoffs, missing state, and duplicated-action counterexamples. An independent continuation runner checks whether the receiver completes the original task with no lost obligations. Hold out agent pairings, task-tree shapes, and interruption locations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A fluent handoff omitting an uncertain remote mutation must fail if the receiver blindly repeats it. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/swarm.c`, `src/blackboard.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/signatures.py` (`ChildResult`). Donor baseline: Defines structured child answers, evidence, uncertainties, and tool-call evidence. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
