# 1594 — Measurement comparability auditor: Learned execution-error recovery

Implement the learned execution-error recovery feature for the Measurement comparability auditor RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a measurement RLM to establish whether two observed performance results support a fair candidate comparison.

External RUN_MANIFESTS and SAMPLE_SERIES contain workload identity, hardware, concurrency, warmup, stopping rules, and missing outcomes. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code matches experimental conditions, recursively resolves metadata discrepancies, and buffers compatible comparisons or explicit rejection reasons. Role output: Return COMPARABILITY_REPORT with matched factors, unresolved differences, usable metrics, and excluded conclusions. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Construct paired episodes containing a recoverable syntax, schema, or reference error and an independently verified repair. Train the root to inspect execution feedback, preserve completed work, and generate the smallest valid continuation. Keep original and corrected code plus the repair transformation.

Training includes controlled benchmark perturbations, censored runs, changed denominators, clock differences, and asynchronous completion. An independent manifest checker and arithmetic reference validate controlled comparisons; missing metadata cannot be inferred as matching. Hold out benchmark harnesses, confound combinations, and timing regimes. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A candidate measured only on completed requests must not beat a baseline measured on the entire workload. Inject an undefined buffer and a completed effect before the failure. Recovery must resolve the buffer without repeating the effect; inventing a final answer cannot count as repair.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/tool_telemetry.c`, `src/chronicle.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`ProbabilisticReasoningSignature`, `ScientificReasoningWorkflow.forward`). Donor baseline: The workflow supplies a fixed neutral prior and placeholder likelihood rather than fitted evidence models. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
