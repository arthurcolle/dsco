# 1238 — Citation resolver: Adaptive recursion-depth policy

Implement the adaptive recursion-depth policy feature for the Citation resolver RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a citation RLM to resolve each technical assertion to an actual retrievable supporting passage.

External DOCUMENT_INDEX, CITATION_MARKERS, and PAGE_SPANS retain stable document identities, references, quotations, and extraction offsets. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code follows reference chains, recursively searches candidate passages, and buffers exact matches plus unsupported citation claims. Role output: Return RESOLVED_CITATIONS with passage handles, identity evidence, support labels, and unresolved references. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create development instances whose useful decomposition ranges from direct environment computation to nested child RLMs. Train the root to decide between a leaf call and another recursive environment based on task structure and remaining budget. Record the actual tree rather than a declared depth.

Training cases include synthetic bibliographies, local papers, shifted pagination, duplicate titles, and fabricated citations. Exact-byte and identifier checks establish resolution; independent reviewers judge whether resolved text substantively supports the assertion. Hold out citation styles, document families, and cross-reference layouts. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A real paper title paired with an invented quotation must fail support even when citation formatting is valid. A simple case must not gain reward from unnecessary recursion. A nested case must preserve dependent intermediate results, while attempts to exceed the host depth limit fail without changing that limit.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/semantic.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/pdf_cache.py` (`get_pdf_text_cached`, `PDFTextCache._get_file_hash`). Donor baseline: PDF extraction is content-hashed but concatenates page text without retaining page-addressed provenance. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
