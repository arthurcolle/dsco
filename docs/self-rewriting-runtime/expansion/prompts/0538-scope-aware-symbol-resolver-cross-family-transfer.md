# 538 — Scope-aware native symbol resolver: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Scope-aware native symbol resolver RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native symbol resolution procedures that preserve scope, visibility, overload contracts, and generation-specific definitions.

REPL variables `symbol_tables`, `import_edges`, and `resolution_queries` retain complete candidate namespaces and caller contracts. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code partitions namespaces, delegates ambiguous bindings, and builds deterministic resolution tables with explicit rejection cases. Role output: Final `SymbolResolutionPlan` handle contains native lookup code, caller-to-target bindings, contract fingerprints, and ambiguity witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Train on generated shadowing, aliasing, visibility, and duplicate-definition episodes labeled by a separately fixed resolver. An immutable reference resolver checks exact target identity, ambiguity detection, visibility rules, and unresolved-symbol diagnostics. Hold out namespace depths, visibility patterns, generation overlaps, and alias chains with renamed-equivalent graphs grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A same-named private symbol in another generation accidentally satisfies an external import with an incompatible contract. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ast.c`, `src/plugin.c`, `src/vm.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`CodeAnalysisToolkit.parse_python_ast`). Donor baseline: Extracts function and import names; native scope and linkage resolution are new work. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
