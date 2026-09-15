# 1230 — Temporal claim reasoner: Joint model and runtime admission

Implement the joint model and runtime admission feature for the Temporal claim reasoner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a temporal RLM to distinguish historical observations, current applicability, and forecasts about changing procedures.

External CLAIM_TIMELINE carries assertion time, event intervals, revision scopes, configurations, and missing temporal qualifiers. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs interval comparisons, recursively resolves temporal references, and buffers compatible claims separately from genuine overlaps. Role output: Return TEMPORAL_APPLICABILITY with valid intervals, revision bindings, conflicts, and uncertainty reasons. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role to identify incompatible model, adapter, child-policy, environment, and native-procedure versions. Store a typed admission plan, pinned tuple, receipts, and decision in feature_artifact; role_result retains the domain artifact. A separate authority validates compatibility and serializes admission against revocation.

Training uses generated timelines, revisioned local fixtures, delayed reports, timezone changes, and uncertain date ranges. A reference interval engine checks controlled cases; natural temporal interpretation receives independent annotated review. Hold out interval structures, date expressions, and revision histories. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A once-correct claim about revision A must not justify adopting incompatible revision B today. An unchanged parent generation with a revoked receipt must still fail admission. Native-improvement claims require changed instruction bytes, retained heap/session state, and no exec; a trained model's confidence alone cannot authorize activation.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/vfs.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/chunked_analyzer.py` (`KnowledgeExtractionSignature`, `ChunkedDocumentAnalyzer._aggregate_results`). Donor baseline: Extraction includes utterances and beliefs, but aggregation discards utterances and keeps only coarse chunk positions. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
