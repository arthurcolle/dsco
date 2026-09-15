# 2055 — Multimodal evidence aligner: Cost-aware child-policy selection

Implement the cost-aware child-policy selection feature for the Multimodal evidence aligner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an alignment RLM to connect textual claims with the correct visual regions and preserve cross-modal disagreements.

External PAGE_IMAGES, TEXT_SPANS, and REGION_CANDIDATES retain page hashes, coordinates, labels, captions, and extraction variants. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs cross-reference alignment, recursively inspects disputed regions, and buffers verified links plus inconsistent readings. Role output: Return MULTIMODAL_ALIGNMENT with region handles, textual anchors, alternatives, disagreement records, and validation provenance. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Offer a pinned pool of child policies with measured capabilities and costs. Train the root to choose a child and context slice for each subproblem while recording actual serving identities. Optimize verified role outcomes under a shared budget, preserving conditional performance data.

Training combines generated charts and diagrams with known layouts, annotated papers, displaced captions, rotations, and extraction errors. Renderer metadata establishes synthetic alignment; independent visual annotation judges natural examples without pretending exact semantic ground truth. Hold out figure layouts, modality combinations, and visual degradation families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A nearby caption must not attach the claim to the wrong panel when panel labels contradict proximity. A cheap child that fails a rare required case must not be selected solely from its average score. Swap an endpoint's model revision and require identity failure rather than silently crediting the trained root.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/tools.c`, `src/execution_kernel.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/pdf_processor.py` (`PDFVisionAnalyzer._analyze_image`, `PDFVisionAnalyzer.process_pdf`). Donor baseline: Vision produces page transcription text; there is no structured equation, table, or cross-modal verification. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
