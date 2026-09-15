# 1314 — Equation structure interpreter: Dependency-scoped retraining

Implement the dependency-scoped retraining feature for the Equation structure interpreter RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an equation RLM to turn page-level mathematical expressions into typed, checkable syntax trees.

External EQUATION_REGIONS, SYMBOL_CONTEXT, and PARSE_CANDIDATES retain image hashes, textual alternatives, definitions, and operator ambiguity. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code recursively delegates subexpressions, assembles bounded ASTs, and checks dimensional and syntactic constraints before buffering results. Role output: Return EQUATION_AST with symbol bindings, image-span provenance, alternatives, and validation status. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Bind training episodes and learned procedures to content and environment dependencies. When one dependency changes, select the affected development slices and train a bounded role update with replay from unaffected families. Version the dependency graph and resulting checkpoint separately.

Training uses rendered formulas with known ASTs, notation variants, superscripts, piecewise conditions, and reviewed scientific examples. A reference parser and evaluation probes check generated formulas; specialists adjudicate unresolved notation in real documents. Hold out expression grammars, fonts, and scientific notation conventions. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A missing minus sign or exponent grouping must not silently produce an equivalent-looking but different formula. Editing content at the same path must reopen dependent learning claims. Retraining only on the changed slice must not hide regressions elsewhere, and unrelated episodes must not be relabeled stale merely to improve reported coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ast.c`, `src/context_fabric.c`, `src/tools.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/pdf_processor.py` (`PDFVisionAnalyzer._analyze_image`, `PDFVisionAnalyzer.process_pdf`). Donor baseline: Vision produces page transcription text; there is no structured equation, table, or cross-modal verification. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
