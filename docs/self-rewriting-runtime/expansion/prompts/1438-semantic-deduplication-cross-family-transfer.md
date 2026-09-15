# 1438 — Semantic occurrence deduplicator: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Semantic occurrence deduplicator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an evidence RLM to deduplicate repeated observations without merging genuinely distinct or contradictory occurrences.

External CHUNK_OBSERVATIONS and PARENT_SPANS preserve document identity, absolute coordinates, normalized propositions, and extraction revisions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code clusters candidate duplicates, recursively tests semantic distinctions, and buffers canonical occurrences with all observation aliases. Role output: Return OCCURRENCE_INDEX with canonical IDs, alias edges, preserved disagreements, and uncertainty flags. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Training uses overlapping windows, controlled paraphrases, repeated real occurrences, negation changes, and quantity substitutions. Known span identities settle occurrence duplication; independent annotations judge paraphrase equivalence when exact identity is absent. Hold out chunk configurations, paraphrase operators, and document domains. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Two overlapping extractions count once, but a later identical assertion remains a separate occurrence. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/semantic.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/chunked_analyzer.py` (`ChunkedDocumentAnalyzer.create_chunks`, `ChunkedDocumentAnalyzer._aggregate_results`). Donor baseline: Overlapping chunks are aggregated by normalized strings without exact occurrence reconciliation. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
