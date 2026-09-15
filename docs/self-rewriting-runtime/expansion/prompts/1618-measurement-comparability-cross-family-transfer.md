# 1618 — Measurement comparability auditor: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Measurement comparability auditor RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a measurement RLM to establish whether two observed performance results support a fair candidate comparison.

External RUN_MANIFESTS and SAMPLE_SERIES contain workload identity, hardware, concurrency, warmup, stopping rules, and missing outcomes. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code matches experimental conditions, recursively resolves metadata discrepancies, and buffers compatible comparisons or explicit rejection reasons. Role output: Return COMPARABILITY_REPORT with matched factors, unresolved differences, usable metrics, and excluded conclusions. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Training includes controlled benchmark perturbations, censored runs, changed denominators, clock differences, and asynchronous completion. An independent manifest checker and arithmetic reference validate controlled comparisons; missing metadata cannot be inferred as matching. Hold out benchmark harnesses, confound combinations, and timing regimes. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A candidate measured only on completed requests must not beat a baseline measured on the entire workload. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/tool_telemetry.c`, `src/chronicle.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`ProbabilisticReasoningSignature`, `ScientificReasoningWorkflow.forward`). Donor baseline: The workflow supplies a fixed neutral prior and placeholder likelihood rather than fitted evidence models. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
