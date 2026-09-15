# 2259 — Tool argument repairer: Evidence-based stopping policy

Implement the evidence-based stopping policy feature for the Tool argument repairer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Repair malformed tool arguments while preserving the user's requested operation and the existing capability boundary.

Keep original intent, exact schema, rejected payload, validation errors, and prior effects in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively align schema fields with intent, propose minimal corrections, and validate without dispatching speculative mutations. Role output: Produce an argument-repair root adapter, corrected payload or abstention, intent mapping, and governed dispatch evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train a root decision among continue, return a verified environment result, and terminate unresolved. Development episodes include misleading high confidence, repetitive verification, and unresolved obligations. Optimize task completion with explicit observation requirements and costs, using oracle results rather than self-reported confidence.

Train root examples from recoverable type errors, ambiguous inputs, schema changes, and irreversible-action counterexamples. A deterministic schema checker and fixture backend independently score validity, semantic preservation, and execution count. Hold out tool families, nested schema shapes, and previously unseen validation diagnostics. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Changing a denied read request into an allowed destructive verb must fail even with valid JSON. Repeated identical child opinions cannot satisfy a missing obligation. A complete verified result should survive the stop decision unchanged; exhausted budgets must produce unresolved status instead of fabricated success.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/tools.c`, `src/toolmgmt.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/router_client.py` (`ToolRouterClient._check_allowed`). Donor baseline: Rejects tool names outside an explicitly configured allowlist. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
