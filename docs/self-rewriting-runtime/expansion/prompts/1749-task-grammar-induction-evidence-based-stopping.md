# 1749 — Task grammar inducer: Evidence-based stopping policy

Implement the evidence-based stopping policy feature for the Task grammar inducer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a grammar RLM to infer typed task-composition rules from verified examples rather than memorize surface templates.

External TASK_PROGRAMS, TYPE_CONTRACTS, and EXECUTION_RESULTS preserve effects, resource limits, successful compositions, and invalid examples. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs structural factoring, recursively proposes productions, and buffers rules only after type and witness checks. Role output: Return TASK_GRAMMAR with typed productions, effect constraints, witnesses, and rejected generalizations. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train a root decision among continue, return a verified environment result, and terminate unresolved. Development episodes include misleading high confidence, repetitive verification, and unresolved obligations. Optimize task completion with explicit observation requirements and costs, using oracle results rather than self-reported confidence.

Training uses generated compositional tasks, local capability fixtures, renamed primitives, nested combinations, and impossible postconditions. Independent type checking and reference execution establish solvability for controlled tasks; natural task ambiguity remains explicit. Hold out production combinations, primitive names, and nesting depths. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A grammar must reject a read-only task production requiring writes even when its training wording seems familiar. Repeated identical child opinions cannot satisfy a missing obligation. A complete verified result should survive the stop decision unchanged; exhausted budgets must produce unresolved status instead of fabricated success.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/prompt_program.c`, `src/plan_dag.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`MetaToolRegistry.compose`). Donor baseline: Composition chains named callables without checked cross-tool type, effect, or semantic contracts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
