# 1834 — Cross-language symbol mapper: Learned execution-error recovery

Implement the learned execution-error recovery feature for the Cross-language symbol mapper RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a symbol RLM to map corresponding program entities across languages while preserving ABI and behavioral distinctions.

External SOURCE_TREES and INTERFACE_SCHEMAS retain namespaces, signatures, ownership, numeric widths, call relations, and candidate aliases. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs structural matching, recursively resolves ambiguous bindings, and buffers mappings with explicit unsupported semantic differences. Role output: Return SYMBOL_MAPPING with qualified bindings, ABI constraints, behavioral tests, and rejected equivalences. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Construct paired episodes containing a recoverable syntax, schema, or reference error and an independently verified repair. Train the root to inspect execution feedback, preserve completed work, and generate the smallest valid continuation. Keep original and corrected code plus the repair transformation.

Training uses paired generated Python and C programs, translated local fixtures, overloads, renamed symbols, and divergent integer semantics. Independent parser metadata and executable differential tests validate controlled correspondences; natural ports require reviewed mapping evidence. Hold out translation templates, namespace structures, and type combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A Python integer operation must not map unconditionally to overflowing fixed-width C arithmetic. Inject an undefined buffer and a completed effect before the failure. Recovery must resolve the buffer without repeating the effect; inventing a final answer cannot count as repair.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ast.c`, `src/context_fabric.c`, `src/semantic.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`ASTAnalyzer._extract_structure`, `MetaToolRegistry.register_code`). Donor baseline: AST inspection extracts declarations and syntax metadata but does not validate behavioral contracts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
