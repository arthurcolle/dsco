# 2336 — Capability request planner: Hierarchical context navigation

Implement the hierarchical context navigation feature for the Capability request planner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Find a least-authority execution plan that satisfies the objective or explains the precise missing permission.

Expose task requirements, available tools, argument-sensitive capability classifications, and current grants as REPL data. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively decompose effects, compare lower-authority alternatives, and construct a plan whose permissions can be checked before execution. Role output: Produce a capability-planning root adapter, minimal effect plan, unmet-grant explanation, and actual gate-decision trace. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Build lossless external indexes over task contexts and train the root to navigate coarse descriptors into exact evidence regions. Child RLMs inspect selected subtrees; summaries guide navigation but never replace authoritative content. Record access paths and evidence references for the final artifact.

Train role roots on authorized plans, denied requests, read-only alternatives, and misleading tool descriptions. The unchanged capability gate independently validates every proposed effect and a task oracle checks objective completion. Hold out tool combinations, trust tiers, and session-taint sequences from training. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A nested execution helper must not conceal denied network access or grant itself control authority. A summary that omits a decisive exception must not erase the exception from the answer. Force a query requiring descent into a rarely accessed subtree and verify the exact underlying evidence is consumed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/capability.c`, `src/tools.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/router_client.py` (`ToolRouterClient.set_allowed_tools`). Donor baseline: Stores a frozen set of permitted tool names used by subsequent allowlist checks. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
