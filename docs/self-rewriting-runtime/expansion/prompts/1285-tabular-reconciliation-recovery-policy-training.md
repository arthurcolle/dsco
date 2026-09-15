# 1285 — Tabular evidence reconciler: Checkpoint recovery policy training

Implement the checkpoint recovery policy training feature for the Tabular evidence reconciler RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a table RLM to reconcile conflicting extracted cells before numerical evidence influences executable decisions.

External TABLE_IMAGES, CELL_GRIDS, and HEADER_MAPS preserve page identity, coordinates, units, footnotes, and extraction alternatives. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code aligns grids, recursively delegates disputed regions, and buffers verified corrections without overwriting original observations. Role output: Return RECONCILED_TABLE with cell provenance, competing readings, units, and explicit unresolved regions. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create interrupted role episodes with committed buffers, pending child calls, and explicit effect receipts. Train the root to reconstruct logical state, reconcile pending work, and continue from a compatible model/environment tuple. Treat unavailable state as a recoverable diagnostic rather than invented memory.

Training combines generated tables with exact source values and annotated real tables featuring merged headers and footnotes. A separate renderer-generation oracle validates synthetic cells; independent transcription review judges ambiguous real images. Hold out table layouts, header hierarchies, and visual degradation families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A shifted column must not turn baseline memory into candidate latency despite plausible numeric ranges. Resume after an effect completed but before its acknowledgement was recorded. Reconcile the receipt instead of repeating the effect; incompatible state schemas or changed model identities must prevent a misleading continuation claim.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/tools.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/pdf_processor.py` (`PDFVisionAnalyzer._analyze_image`, `PDFVisionAnalyzer.process_pdf`). Donor baseline: Vision produces page transcription text; there is no structured equation, table, or cross-modal verification. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
