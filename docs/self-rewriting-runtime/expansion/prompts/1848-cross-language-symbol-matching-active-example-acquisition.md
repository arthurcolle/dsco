# 1848 — Cross-language symbol mapper: Active example acquisition

Implement the active example acquisition feature for the Cross-language symbol mapper RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a symbol RLM to map corresponding program entities across languages while preserving ABI and behavioral distinctions.

External SOURCE_TREES and INTERFACE_SCHEMAS retain namespaces, signatures, ownership, numeric widths, call relations, and candidate aliases. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs structural matching, recursively resolves ambiguous bindings, and buffers mappings with explicit unsupported semantic differences. Role output: Return SYMBOL_MAPPING with qualified bindings, ABI constraints, behavioral tests, and rejected equivalences. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to select the next development example or oracle query using uncertainty, task importance, and expected decision benefit. Record selection probabilities and the population from which examples were chosen. Keep a fixed independent evaluation distribution for measuring actual improvement.

Training uses paired generated Python and C programs, translated local fixtures, overloads, renamed symbols, and divergent integer semantics. Independent parser metadata and executable differential tests validate controlled correspondences; natural ports require reviewed mapping evidence. Hold out translation templates, namespace structures, and type combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A Python integer operation must not map unconditionally to overflowing fixed-width C arithmetic. A policy that repeatedly samples easy successes must lose coverage credit. A highly uncertain but irrelevant example must not outrank a decisive missing case merely because it produces a larger confidence change.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ast.c`, `src/context_fabric.c`, `src/semantic.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`ASTAnalyzer._extract_structure`, `MetaToolRegistry.register_code`). Donor baseline: AST inspection extracts declarations and syntax metadata but does not validate behavioral contracts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
