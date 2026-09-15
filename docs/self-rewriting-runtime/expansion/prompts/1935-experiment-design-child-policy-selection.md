# 1935 — Discriminating experiment designer: Cost-aware child-policy selection

Implement the cost-aware child-policy selection feature for the Discriminating experiment designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an experiment RLM to choose permitted probes that distinguish current hypotheses under realistic costs and controls.

External HYPOTHESES, PROBE_MENU, and OBSERVATIONS preserve predicted outcomes, costs, confounders, constraints, and uncertainty. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs design comparisons, recursively inspects discriminants, and buffers preregistered predictions before any governed probe executes. Role output: Return EXPERIMENT_DESIGN with controls, predicted branches, stopping rules, required permissions, and actual observation bindings. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Offer a pinned pool of child policies with measured capabilities and costs. Train the root to choose a child and context slice for each subproblem while recording actual serving identities. Optimize verified role outcomes under a shared budget, preserving conditional performance data.

Training uses causal simulators and owned diagnostic fixtures with irrelevant tests, expensive discriminants, and interacting mechanisms. Independent environment outcomes score final diagnosis and cost; generated prediction prose cannot serve as experimental truth. Hold out fault mechanisms, probe names, and cost schedules. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A historically useful cheap test must be rejected when both current hypotheses predict the same result. A cheap child that fails a rare required case must not be selected solely from its average score. Swap an endpoint's model revision and require identity failure rather than silently crediting the trained root.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/execution_kernel.c`, `src/plan_optimizer.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`CausalReasoningSignature`, `ScientificReasoningWorkflow.forward`). Donor baseline: Causal relationships are requested as text; the scientific workflow leaves causal analysis unimplemented. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
