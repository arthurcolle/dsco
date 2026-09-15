# 2867 — Multiagent handoff planner: Learned typed role handoffs

Implement the learned typed role handoffs feature for the Multiagent handoff planner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Transfer unfinished role work with enough executable state for another agent to continue without repeating settled actions.

Keep context handles, completed obligations, pending effects, candidate identities, and unresolved questions in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively summarize branch state by dependency, preserve exact references, and construct a minimal resumable handoff contract. Role output: Produce a handoff root adapter, receiver-ready state artifact, unresolved-action ledger, and successful independent continuation evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to delegate through explicit input, output, evidence, and uncertainty schemas while its partner policies stay frozen. Child prompts contain sufficient scoped context and exact verified dependency values. Failed or incompatible handoffs return typed diagnostics that the root can repair.

Train roots on interrupted collaborations, complete handoffs, missing state, and duplicated-action counterexamples. An independent continuation runner checks whether the receiver completes the original task with no lost obligations. Hold out agent pairings, task-tree shapes, and interruption locations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A fluent handoff omitting an uncertain remote mutation must fail if the receiver blindly repeats it. A partner omitting required evidence or returning a result from another task must be rejected. Matching prose labels cannot replace schema and identity checks, and a repaired handoff must preserve its original authority.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/swarm.c`, `src/blackboard.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/signatures.py` (`ChildResult`). Donor baseline: Defines structured child answers, evidence, uncertainties, and tool-call evidence. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
