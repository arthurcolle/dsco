# 525 — Scope-aware native symbol resolver: Cost-aware child-policy selection

Implement the cost-aware child-policy selection feature for the Scope-aware native symbol resolver RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native symbol resolution procedures that preserve scope, visibility, overload contracts, and generation-specific definitions.

REPL variables `symbol_tables`, `import_edges`, and `resolution_queries` retain complete candidate namespaces and caller contracts. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code partitions namespaces, delegates ambiguous bindings, and builds deterministic resolution tables with explicit rejection cases. Role output: Final `SymbolResolutionPlan` handle contains native lookup code, caller-to-target bindings, contract fingerprints, and ambiguity witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Offer a pinned pool of child policies with measured capabilities and costs. Train the root to choose a child and context slice for each subproblem while recording actual serving identities. Optimize verified role outcomes under a shared budget, preserving conditional performance data.

Train on generated shadowing, aliasing, visibility, and duplicate-definition episodes labeled by a separately fixed resolver. An immutable reference resolver checks exact target identity, ambiguity detection, visibility rules, and unresolved-symbol diagnostics. Hold out namespace depths, visibility patterns, generation overlaps, and alias chains with renamed-equivalent graphs grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A same-named private symbol in another generation accidentally satisfies an external import with an incompatible contract. A cheap child that fails a rare required case must not be selected solely from its average score. Swap an endpoint's model revision and require identity failure rather than silently crediting the trained root.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ast.c`, `src/plugin.c`, `src/vm.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`CodeAnalysisToolkit.parse_python_ast`). Donor baseline: Extracts function and import names; native scope and linkage resolution are new work. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
