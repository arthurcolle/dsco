# 2859 — Multiagent handoff planner: Evidence-based stopping policy

Implement the evidence-based stopping policy feature for the Multiagent handoff planner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Transfer unfinished role work with enough executable state for another agent to continue without repeating settled actions.

Keep context handles, completed obligations, pending effects, candidate identities, and unresolved questions in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively summarize branch state by dependency, preserve exact references, and construct a minimal resumable handoff contract. Role output: Produce a handoff root adapter, receiver-ready state artifact, unresolved-action ledger, and successful independent continuation evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train a root decision among continue, return a verified environment result, and terminate unresolved. Development episodes include misleading high confidence, repetitive verification, and unresolved obligations. Optimize task completion with explicit observation requirements and costs, using oracle results rather than self-reported confidence.

Train roots on interrupted collaborations, complete handoffs, missing state, and duplicated-action counterexamples. An independent continuation runner checks whether the receiver completes the original task with no lost obligations. Hold out agent pairings, task-tree shapes, and interruption locations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A fluent handoff omitting an uncertain remote mutation must fail if the receiver blindly repeats it. Repeated identical child opinions cannot satisfy a missing obligation. A complete verified result should survive the stop decision unchanged; exhausted budgets must produce unresolved status instead of fabricated success.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/swarm.c`, `src/blackboard.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/signatures.py` (`ChildResult`). Donor baseline: Defines structured child answers, evidence, uncertainties, and tool-call evidence. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
