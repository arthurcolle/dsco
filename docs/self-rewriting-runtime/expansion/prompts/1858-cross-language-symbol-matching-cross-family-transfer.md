# 1858 — Cross-language symbol mapper: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Cross-language symbol mapper RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a symbol RLM to map corresponding program entities across languages while preserving ABI and behavioral distinctions.

External SOURCE_TREES and INTERFACE_SCHEMAS retain namespaces, signatures, ownership, numeric widths, call relations, and candidate aliases. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs structural matching, recursively resolves ambiguous bindings, and buffers mappings with explicit unsupported semantic differences. Role output: Return SYMBOL_MAPPING with qualified bindings, ABI constraints, behavioral tests, and rejected equivalences. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Training uses paired generated Python and C programs, translated local fixtures, overloads, renamed symbols, and divergent integer semantics. Independent parser metadata and executable differential tests validate controlled correspondences; natural ports require reviewed mapping evidence. Hold out translation templates, namespace structures, and type combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A Python integer operation must not map unconditionally to overflowing fixed-width C arithmetic. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ast.c`, `src/context_fabric.c`, `src/semantic.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`ASTAnalyzer._extract_structure`, `MetaToolRegistry.register_code`). Donor baseline: AST inspection extracts declarations and syntax metadata but does not validate behavioral contracts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
