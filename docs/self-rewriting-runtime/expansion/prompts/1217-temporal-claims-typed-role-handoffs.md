# 1217 — Temporal claim reasoner: Learned typed role handoffs

Implement the learned typed role handoffs feature for the Temporal claim reasoner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a temporal RLM to distinguish historical observations, current applicability, and forecasts about changing procedures.

External CLAIM_TIMELINE carries assertion time, event intervals, revision scopes, configurations, and missing temporal qualifiers. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs interval comparisons, recursively resolves temporal references, and buffers compatible claims separately from genuine overlaps. Role output: Return TEMPORAL_APPLICABILITY with valid intervals, revision bindings, conflicts, and uncertainty reasons. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to delegate through explicit input, output, evidence, and uncertainty schemas while its partner policies stay frozen. Child prompts contain sufficient scoped context and exact verified dependency values. Failed or incompatible handoffs return typed diagnostics that the root can repair.

Training uses generated timelines, revisioned local fixtures, delayed reports, timezone changes, and uncertain date ranges. A reference interval engine checks controlled cases; natural temporal interpretation receives independent annotated review. Hold out interval structures, date expressions, and revision histories. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A once-correct claim about revision A must not justify adopting incompatible revision B today. A partner omitting required evidence or returning a result from another task must be rejected. Matching prose labels cannot replace schema and identity checks, and a repaired handoff must preserve its original authority.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/vfs.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/chunked_analyzer.py` (`KnowledgeExtractionSignature`, `ChunkedDocumentAnalyzer._aggregate_results`). Donor baseline: Extraction includes utterances and beliefs, but aggregation discards utterances and keeps only coarse chunk positions. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
