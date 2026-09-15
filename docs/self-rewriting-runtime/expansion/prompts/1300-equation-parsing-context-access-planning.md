# 1300 — Equation structure interpreter: Learned context-access planning

Implement the learned context-access planning feature for the Equation structure interpreter RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an equation RLM to turn page-level mathematical expressions into typed, checkable syntax trees.

External EQUATION_REGIONS, SYMBOL_CONTEXT, and PARSE_CANDIDATES retain image hashes, textual alternatives, definitions, and operator ambiguity. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code recursively delegates subexpressions, assembles bounded ASTs, and checks dimensional and syntactic constraints before buffering results. Role output: Return EQUATION_AST with symbol bindings, image-span provenance, alternatives, and validation status. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Expose context-region descriptors and bounded reads while keeping the full input outside the root window. Train the root to generate access programs that retrieve discriminating regions and recursively examine relevant slices. Log coverage, read amplification, and every region supporting the final artifact.

Training uses rendered formulas with known ASTs, notation variants, superscripts, piecewise conditions, and reviewed scientific examples. A reference parser and evaluation probes check generated formulas; specialists adjudicate unresolved notation in real documents. Hold out expression grammars, fonts, and scientific notation conventions. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A missing minus sign or exponent grouping must not silently produce an equivalent-looking but different formula. Plant decisive information outside the usual prefix and supply a plausible distractor near the beginning. A policy that answers from previews alone must fail the oracle, and an unbounded full-context copy must be detected.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ast.c`, `src/context_fabric.c`, `src/tools.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/pdf_processor.py` (`PDFVisionAnalyzer._analyze_image`, `PDFVisionAnalyzer.process_pdf`). Donor baseline: Vision produces page transcription text; there is no structured equation, table, or cross-modal verification. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
