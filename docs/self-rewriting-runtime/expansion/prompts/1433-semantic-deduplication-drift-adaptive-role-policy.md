# 1433 — Semantic occurrence deduplicator: Drift-adaptive role policy

Implement the drift-adaptive role policy feature for the Semantic occurrence deduplicator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an evidence RLM to deduplicate repeated observations without merging genuinely distinct or contradictory occurrences.

External CHUNK_OBSERVATIONS and PARENT_SPANS preserve document identity, absolute coordinates, normalized propositions, and extraction revisions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code clusters candidate duplicates, recursively tests semantic distinctions, and buffers canonical occurrences with all observation aliases. Role output: Return OCCURRENCE_INDEX with canonical IDs, alias edges, preserved disagreements, and uncertainty flags. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to distinguish transient anomalies, changed input distributions, and changed task semantics from chronological observations. It selects bounded diagnostic probes and proposes scoped relearning. Keep an immutable pre-change checkpoint and evaluate detection delay, false alarms, and post-change correctness.

Training uses overlapping windows, controlled paraphrases, repeated real occurrences, negation changes, and quantity substitutions. Known span identities settle occurrence duplication; independent annotations judge paraphrase equivalence when exact identity is absent. Hold out chunk configurations, paraphrase operators, and document domains. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Two overlapping extractions count once, but a later identical assertion remains a separate occurrence. One extreme outlier must not trigger wholesale relearning. A sustained change affecting the role's contract must be detected without access to future observations, and unaffected task families must retain their prior behavior.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/semantic.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/chunked_analyzer.py` (`ChunkedDocumentAnalyzer.create_chunks`, `ChunkedDocumentAnalyzer._aggregate_results`). Donor baseline: Overlapping chunks are aggregated by normalized strings without exact occurrence reconciliation. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
