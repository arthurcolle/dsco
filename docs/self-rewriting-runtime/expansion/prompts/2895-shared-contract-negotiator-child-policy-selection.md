# 2895 — Shared contract negotiator: Cost-aware child-policy selection

Implement the cost-aware child-policy selection feature for the Shared contract negotiator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Resolve incompatible interface assumptions between role implementations before their independently correct components are integrated.

Expose proposed schemas, callers, callee behaviors, invariants, and version constraints in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively compare obligations, generate compatible alternatives, and derive executable counterexamples for unresolved contract disagreements. Role output: Produce a contract-negotiation root adapter, agreed schema proposal, remaining disagreements, and bidirectional compatibility test artifacts. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Offer a pinned pool of child policies with measured capabilities and costs. Train the root to choose a child and context slice for each subproblem while recording actual serving identities. Optimize verified role outcomes under a shared budget, preserving conditional performance data.

Train roots on interface negotiations, compatible refinements, conflicting ownership assumptions, and silently weakened invariants. Independent consumer/provider contract tests score compatibility while preserving each party's nonnegotiable requirements. Hold out interface families, ownership models, and version-skew combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Resolving a conflict by dropping a required error case must fail even if both components compile. A cheap child that fails a rare required case must not be selected solely from its average score. Swap an endpoint's model revision and require identity failure rather than silently crediting the trained root.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/blackboard.c`, `src/tools.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/signatures.py` (`Subtask`). Donor baseline: Defines subtask identity, assigned tier, requested tools, and success criteria. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
