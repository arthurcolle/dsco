# 949 — Semantic change-impact analyst: Role-specific counterexample adversary

Implement the role-specific counterexample adversary feature for the Semantic change-impact analyst RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn change-impact procedures that identify every affected executable contract while avoiding unnecessary requalification of independent cells.

REPL `before_after_ir`, `semantic_graph`, and `contract_coverage` retain changes, dependencies, and observed output obligations. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code propagates changed definitions, delegates control-dependence slices, and proposes conservative affected-contract sets. Role output: Final `ChangeImpactSlice` handle contains affected generations, required tests, justified exclusions, and unresolved dependency evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train an auxiliary root policy to generate valid challenging inputs for the role's current candidate behavior. A fixed independent referee checks input validity and confirms violations. Retain diverse minimized witnesses, and evaluate the main role against non-adaptive sealed cases as well.

Train on controlled program mutations and independently measured downstream changes, including false-negative slice examples. A frozen mutation oracle and conservative dependency checker validate affected-behavior recall before rewarding smaller qualification sets. Hold out mutation operators, shared-state patterns, graph shapes, and contract combinations with source families grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Changing a branch predicate alters downstream values without changing any function name or direct-call edge. A malformed input, altered reference, or induced harness failure cannot earn adversarial success. A genuine in-contract error must produce a reproducible witness tied to the exact candidate and environment revisions.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ast.c`, `src/autoresearch.c`, `src/plan_dag.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`ASTAnalyzer.transform`). Donor baseline: Applies local syntax transformations; determining downstream semantic impact is not implemented. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
