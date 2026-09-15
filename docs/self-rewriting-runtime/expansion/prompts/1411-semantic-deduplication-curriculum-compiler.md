# 1411 — Semantic occurrence deduplicator: Executable curriculum compiler

Implement the executable curriculum compiler feature for the Semantic occurrence deduplicator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an evidence RLM to deduplicate repeated observations without merging genuinely distinct or contradictory occurrences.

External CHUNK_OBSERVATIONS and PARENT_SPANS preserve document identity, absolute coordinates, normalized propositions, and extraction revisions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code clusters candidate duplicates, recursively tests semantic distinctions, and buffers canonical occurrences with all observation aliases. Role output: Return OCCURRENCE_INDEX with canonical IDs, alias edges, preserved disagreements, and uncertainty flags. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Compile a task grammar with explicit difficulty axes, valid transformations, and known failure labels. Generate positive and negative episodes, retaining their originating family identifiers. Train the role's root policy on a balanced development curriculum and expose a reproducible curriculum-build command.

Training uses overlapping windows, controlled paraphrases, repeated real occurrences, negation changes, and quantity substitutions. Known span identities settle occurrence duplication; independent annotations judge paraphrase equivalence when exact identity is absent. Hold out chunk configurations, paraphrase operators, and document domains. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Two overlapping extractions count once, but a later identical assertion remains a separate occurrence. Reject a generator that changes the answer without updating its reference or produces duplicate families under different seeds. Measure valid-case coverage and trained-root success separately.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/semantic.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/chunked_analyzer.py` (`ChunkedDocumentAnalyzer.create_chunks`, `ChunkedDocumentAnalyzer._aggregate_results`). Donor baseline: Overlapping chunks are aggregated by normalized strings without exact occurrence reconciliation. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
