# 2875 — Multiagent handoff planner: Checkpoint recovery policy training

Implement the checkpoint recovery policy training feature for the Multiagent handoff planner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Transfer unfinished role work with enough executable state for another agent to continue without repeating settled actions.

Keep context handles, completed obligations, pending effects, candidate identities, and unresolved questions in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively summarize branch state by dependency, preserve exact references, and construct a minimal resumable handoff contract. Role output: Produce a handoff root adapter, receiver-ready state artifact, unresolved-action ledger, and successful independent continuation evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create interrupted role episodes with committed buffers, pending child calls, and explicit effect receipts. Train the root to reconstruct logical state, reconcile pending work, and continue from a compatible model/environment tuple. Treat unavailable state as a recoverable diagnostic rather than invented memory.

Train roots on interrupted collaborations, complete handoffs, missing state, and duplicated-action counterexamples. An independent continuation runner checks whether the receiver completes the original task with no lost obligations. Hold out agent pairings, task-tree shapes, and interruption locations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A fluent handoff omitting an uncertain remote mutation must fail if the receiver blindly repeats it. Resume after an effect completed but before its acknowledgement was recorded. Reconcile the receipt instead of repeating the effect; incompatible state schemas or changed model identities must prevent a misleading continuation claim.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/swarm.c`, `src/blackboard.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/signatures.py` (`ChildResult`). Donor baseline: Defines structured child answers, evidence, uncertainties, and tool-call evidence. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
