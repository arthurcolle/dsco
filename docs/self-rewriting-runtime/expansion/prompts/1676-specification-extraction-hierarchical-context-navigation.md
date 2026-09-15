# 1676 — Executable specification extractor: Hierarchical context navigation

Implement the hierarchical context navigation feature for the Executable specification extractor RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a specification RLM to extract executable obligations from implementations, schemas, examples, and explicit technical requirements.

External SOURCE_UNITS, SCHEMAS, and EXAMPLES preserve revisions, positive cases, negative cases, and documented assumption scopes. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs predicate synthesis, recursively delegates contract clauses, and buffers independently checked obligations with source references. Role output: Return EXECUTABLE_SPECIFICATION with typed predicates, applicability conditions, evidence links, and unresolved conflicts. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Build lossless external indexes over task contexts and train the root to navigate coarse descriptors into exact evidence regions. Child RLMs inspect selected subtrees; summaries guide navigation but never replace authoritative content. Record access paths and evidence references for the final artifact.

Training uses generated APIs, local pure transforms, intentionally inconsistent docs, error atomicity, and boundary constraints. An independent contract runner checks controlled tasks; inferred regularities remain labeled assumptions until independently established. Hold out API families, schema compositions, and specification phrasing. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A nonempty successful response must not satisfy a contract requiring preserved request identity and sorted output. A summary that omits a decisive exception must not erase the exception from the answer. Force a query requiring descent into a rarely accessed subtree and verify the exact underlying evidence is consumed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/ast.c`, `src/tools.c`, `src/execution_kernel.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`ASTAnalyzer._extract_structure`, `MetaToolRegistry.register_code`). Donor baseline: AST inspection extracts declarations and syntax metadata but does not validate behavioral contracts. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
