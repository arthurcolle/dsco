# 1748 — Task grammar inducer: Adaptive recursion-depth policy

Implement the adaptive recursion-depth policy feature for the Task grammar inducer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a grammar RLM to infer typed task-composition rules from verified examples rather than memorize surface templates.

External TASK_PROGRAMS, TYPE_CONTRACTS, and EXECUTION_RESULTS preserve effects, resource limits, successful compositions, and invalid examples. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs structural factoring, recursively proposes productions, and buffers rules only after type and witness checks. Role output: Return TASK_GRAMMAR with typed productions, effect constraints, witnesses, and rejected generalizations. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create development instances whose useful decomposition ranges from direct environment computation to nested child RLMs. Train the root to decide between a leaf call and another recursive environment based on task structure and remaining budget. Record the actual tree rather than a declared depth.

Training uses generated compositional tasks, local capability fixtures, renamed primitives, nested combinations, and impossible postconditions. Independent type checking and reference execution establish solvability for controlled tasks; natural task ambiguity remains explicit. Hold out production combinations, primitive names, and nesting depths. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A grammar must reject a read-only task production requiring writes even when its training wording seems familiar. A simple case must not gain reward from unnecessary recursion. A nested case must preserve dependent intermediate results, while attempts to exceed the host depth limit fail without changing that limit.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/prompt_program.c`, `src/plan_dag.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`MetaToolRegistry.compose`). Donor baseline: Composition chains named callables without checked cross-tool type, effect, or semantic contracts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
