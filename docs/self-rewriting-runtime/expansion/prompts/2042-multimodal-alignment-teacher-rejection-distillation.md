# 2042 — Multimodal evidence aligner: Teacher rejection distillation

Implement the teacher rejection distillation feature for the Multimodal evidence aligner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an alignment RLM to connect textual claims with the correct visual regions and preserve cross-modal disagreements.

External PAGE_IMAGES, TEXT_SPANS, and REGION_CANDIDATES retain page hashes, coordinates, labels, captions, and extraction variants. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs cross-reference alignment, recursively inspects disputed regions, and buffers verified links plus inconsistent readings. Role output: Return MULTIMODAL_ALIGNMENT with region handles, textual anchors, alternatives, disagreement records, and validation provenance. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Collect multiple executable teacher trajectories per development task. Replay their programs against the fixed oracle, reject incorrect or incomplete episodes, and deduplicate shared derivations before extracting root action targets. Distill accepted trajectories into the role policy with balanced family sampling.

Training combines generated charts and diagrams with known layouts, annotated papers, displaced captions, rotations, and extraction errors. Renderer metadata establishes synthetic alignment; independent visual annotation judges natural examples without pretending exact semantic ground truth. Hold out figure layouts, modality combinations, and visual degradation families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A nearby caption must not attach the claim to the wrong panel when panel labels contradict proximity. A fluent teacher episode containing the expected answer but an incorrect executable result must be excluded. Compare the trained root against its initial checkpoint on untouched families.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/tools.c`, `src/execution_kernel.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/pdf_processor.py` (`PDFVisionAnalyzer._analyze_image`, `PDFVisionAnalyzer.process_pdf`). Donor baseline: Vision produces page transcription text; there is no structured equation, table, or cross-modal verification. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
