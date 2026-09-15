# 1253 — Citation resolver: Drift-adaptive role policy

Implement the drift-adaptive role policy feature for the Citation resolver RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a citation RLM to resolve each technical assertion to an actual retrievable supporting passage.

External DOCUMENT_INDEX, CITATION_MARKERS, and PAGE_SPANS retain stable document identities, references, quotations, and extraction offsets. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code follows reference chains, recursively searches candidate passages, and buffers exact matches plus unsupported citation claims. Role output: Return RESOLVED_CITATIONS with passage handles, identity evidence, support labels, and unresolved references. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to distinguish transient anomalies, changed input distributions, and changed task semantics from chronological observations. It selects bounded diagnostic probes and proposes scoped relearning. Keep an immutable pre-change checkpoint and evaluate detection delay, false alarms, and post-change correctness.

Training cases include synthetic bibliographies, local papers, shifted pagination, duplicate titles, and fabricated citations. Exact-byte and identifier checks establish resolution; independent reviewers judge whether resolved text substantively supports the assertion. Hold out citation styles, document families, and cross-reference layouts. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A real paper title paired with an invented quotation must fail support even when citation formatting is valid. One extreme outlier must not trigger wholesale relearning. A sustained change affecting the role's contract must be detected without access to future observations, and unaffected task families must retain their prior behavior.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/semantic.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/pdf_cache.py` (`get_pdf_text_cached`, `PDFTextCache._get_file_hash`). Donor baseline: PDF extraction is content-hashed but concatenates page text without retaining page-addressed provenance. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
