# 2338 — Capability request planner: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Capability request planner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Find a least-authority execution plan that satisfies the objective or explains the precise missing permission.

Expose task requirements, available tools, argument-sensitive capability classifications, and current grants as REPL data. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively decompose effects, compare lower-authority alternatives, and construct a plan whose permissions can be checked before execution. Role output: Produce a capability-planning root adapter, minimal effect plan, unmet-grant explanation, and actual gate-decision trace. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Train role roots on authorized plans, denied requests, read-only alternatives, and misleading tool descriptions. The unchanged capability gate independently validates every proposed effect and a task oracle checks objective completion. Hold out tool combinations, trust tiers, and session-taint sequences from training. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A nested execution helper must not conceal denied network access or grant itself control authority. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/capability.c`, `src/tools.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/router_client.py` (`ToolRouterClient.set_allowed_tools`). Donor baseline: Stores a frozen set of permitted tool names used by subsequent allowlist checks. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
