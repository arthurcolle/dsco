# 1948 — Discriminating experiment designer: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Discriminating experiment designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an experiment RLM to choose permitted probes that distinguish current hypotheses under realistic costs and controls.

External HYPOTHESES, PROBE_MENU, and OBSERVATIONS preserve predicted outcomes, costs, confounders, constraints, and uncertainty. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs design comparisons, recursively inspects discriminants, and buffers preregistered predictions before any governed probe executes. Role output: Return EXPERIMENT_DESIGN with controls, predicted branches, stopping rules, required permissions, and actual observation bindings. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Training uses causal simulators and owned diagnostic fixtures with irrelevant tests, expensive discriminants, and interacting mechanisms. Independent environment outcomes score final diagnosis and cost; generated prediction prose cannot serve as experimental truth. Hold out fault mechanisms, probe names, and cost schedules. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A historically useful cheap test must be rejected when both current hypotheses predict the same result. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/execution_kernel.c`, `src/plan_optimizer.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`CausalReasoningSignature`, `ScientificReasoningWorkflow.forward`). Donor baseline: Causal relationships are requested as text; the scientific workflow leaves causal analysis unimplemented. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
