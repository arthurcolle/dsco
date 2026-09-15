# 2866 — Multiagent handoff planner: Learned asynchronous recursion

Implement the learned asynchronous recursion feature for the Multiagent handoff planner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Transfer unfinished role work with enough executable state for another agent to continue without repeating settled actions.

Keep context handles, completed obligations, pending effects, candidate identities, and unresolved questions in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively summarize branch state by dependency, preserve exact references, and construct a minimal resumable handoff contract. Role output: Produce a handoff root adapter, receiver-ready state artifact, unresolved-action ledger, and successful independent continuation evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Expose bounded futures for recursive calls and train submit, await, batch, and cancel decisions on controlled completion distributions. Reservations span all descendants. Keep completed result identities stable and ensure training and inference scheduling never block the terminal input handler.

Train roots on interrupted collaborations, complete handoffs, missing state, and duplicated-action counterexamples. An independent continuation runner checks whether the receiver completes the original task with no lost obligations. Hold out agent pairings, task-tree shapes, and interruption locations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A fluent handoff omitting an uncertain remote mutation must fail if the receiver blindly repeats it. Delay a required child indefinitely while delivering other results out of order. The system must remain responsive, preserve partial work, and report unresolved completeness; dropping the slow child cannot manufacture a successful fast result.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/swarm.c`, `src/blackboard.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/signatures.py` (`ChildResult`). Donor baseline: Defines structured child answers, evidence, uncertainties, and tool-call evidence. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
