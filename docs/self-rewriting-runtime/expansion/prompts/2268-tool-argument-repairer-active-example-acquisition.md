# 2268 — Tool argument repairer: Active example acquisition

Implement the active example acquisition feature for the Tool argument repairer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Repair malformed tool arguments while preserving the user's requested operation and the existing capability boundary.

Keep original intent, exact schema, rejected payload, validation errors, and prior effects in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively align schema fields with intent, propose minimal corrections, and validate without dispatching speculative mutations. Role output: Produce an argument-repair root adapter, corrected payload or abstention, intent mapping, and governed dispatch evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to select the next development example or oracle query using uncertainty, task importance, and expected decision benefit. Record selection probabilities and the population from which examples were chosen. Keep a fixed independent evaluation distribution for measuring actual improvement.

Train root examples from recoverable type errors, ambiguous inputs, schema changes, and irreversible-action counterexamples. A deterministic schema checker and fixture backend independently score validity, semantic preservation, and execution count. Hold out tool families, nested schema shapes, and previously unseen validation diagnostics. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Changing a denied read request into an allowed destructive verb must fail even with valid JSON. A policy that repeatedly samples easy successes must lose coverage credit. A highly uncertain but irrelevant example must not outrank a decisive missing case merely because it produces a larger confidence change.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/tools.c`, `src/toolmgmt.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/router_client.py` (`ToolRouterClient._check_allowed`). Donor baseline: Rejects tool names outside an explicitly configured allowlist. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
