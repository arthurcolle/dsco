# 1678 — Executable specification extractor: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Executable specification extractor RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a specification RLM to extract executable obligations from implementations, schemas, examples, and explicit technical requirements.

External SOURCE_UNITS, SCHEMAS, and EXAMPLES preserve revisions, positive cases, negative cases, and documented assumption scopes. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs predicate synthesis, recursively delegates contract clauses, and buffers independently checked obligations with source references. Role output: Return EXECUTABLE_SPECIFICATION with typed predicates, applicability conditions, evidence links, and unresolved conflicts. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Training uses generated APIs, local pure transforms, intentionally inconsistent docs, error atomicity, and boundary constraints. An independent contract runner checks controlled tasks; inferred regularities remain labeled assumptions until independently established. Hold out API families, schema compositions, and specification phrasing. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A nonempty successful response must not satisfy a contract requiring preserved request identity and sorted output. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ast.c`, `src/tools.c`, `src/execution_kernel.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`ASTAnalyzer._extract_structure`, `MetaToolRegistry.register_code`). Donor baseline: AST inspection extracts declarations and syntax metadata but does not validate behavioral contracts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
