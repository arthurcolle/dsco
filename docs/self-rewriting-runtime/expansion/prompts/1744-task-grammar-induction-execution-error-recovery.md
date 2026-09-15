# 1744 — Task grammar inducer: Learned execution-error recovery

Implement the learned execution-error recovery feature for the Task grammar inducer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a grammar RLM to infer typed task-composition rules from verified examples rather than memorize surface templates.

External TASK_PROGRAMS, TYPE_CONTRACTS, and EXECUTION_RESULTS preserve effects, resource limits, successful compositions, and invalid examples. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs structural factoring, recursively proposes productions, and buffers rules only after type and witness checks. Role output: Return TASK_GRAMMAR with typed productions, effect constraints, witnesses, and rejected generalizations. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Construct paired episodes containing a recoverable syntax, schema, or reference error and an independently verified repair. Train the root to inspect execution feedback, preserve completed work, and generate the smallest valid continuation. Keep original and corrected code plus the repair transformation.

Training uses generated compositional tasks, local capability fixtures, renamed primitives, nested combinations, and impossible postconditions. Independent type checking and reference execution establish solvability for controlled tasks; natural task ambiguity remains explicit. Hold out production combinations, primitive names, and nesting depths. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A grammar must reject a read-only task production requiring writes even when its training wording seems familiar. Inject an undefined buffer and a completed effect before the failure. Recovery must resolve the buffer without repeating the effect; inventing a final answer cannot count as repair.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/prompt_program.c`, `src/plan_dag.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`MetaToolRegistry.compose`). Donor baseline: Composition chains named callables without checked cross-tool type, effect, or semantic contracts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
