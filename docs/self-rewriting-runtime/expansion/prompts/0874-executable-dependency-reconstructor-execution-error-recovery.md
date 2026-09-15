# 874 — Executable dependency-graph reconstructor: Learned execution-error recovery

Implement the learned execution-error recovery feature for the Executable dependency-graph reconstructor RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn procedures that reconstruct executable dependency graphs from typed IR, imports, state access, and observed invocation relationships.

REPL `procedure_ir`, `call_observations`, and `state_accesses` retain full definitions and dynamic evidence with exact generation identities. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code clusters procedures, delegates indirect-call questions, and merges typed edges while preserving unresolved targets. Role output: Final `ExecutableDependencyGraph` handle names typed edges, provenance, unresolved targets, and graph-completeness counterexamples. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Construct paired episodes containing a recoverable syntax, schema, or reference error and an independently verified repair. Train the root to inspect execution feedback, preserve completed work, and generate the smallest valid continuation. Keep original and corrected code plus the repair transformation.

Train on synthetic modules with known dependency graphs and instrumented executions exposing missing static relationships. A fixed graph generator/reference analyzer verifies mandatory edges, target types, cycles, and explicit unknown-call coverage. Hold out callback shapes, import graphs, state-sharing patterns, and recursive structures with equivalent modules grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: An indirect callback changes shared state even though its target never appears in the direct-call list. Inject an undefined buffer and a completed effect before the failure. Recovery must resolve the buffer without repeating the effect; inventing a final answer cannot count as repair.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ast.c`, `src/plan_dag.c`, `src/trace_kg_store.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`WorkflowToolkit.analyze_dependencies`). Donor baseline: Analyzes declared workflow dependencies; recovering native semantic dependencies requires additional mechanisms. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
