# 1346 — Unit and scale normalizer: Hierarchical context navigation

Implement the hierarchical context navigation feature for the Unit and scale normalizer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a unit RLM to normalize quantitative observations without losing dimensions, scales, or measurement conventions.

External QUANTITIES and UNIT_DEFINITIONS preserve source values, prefixes, reference conditions, denominator units, and uncertainty intervals. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs exact conversions, recursively resolves ambiguous unit context, and buffers normalized values alongside untouched source quantities. Role output: Return NORMALIZED_QUANTITIES with conversion derivations, assumptions, original references, and incompatibility errors. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Build lossless external indexes over task contexts and train the root to navigate coarse descriptors into exact evidence regions. Child RLMs inspect selected subtrees; summaries guide navigation but never replace authoritative content. Record access paths and evidence references for the final artifact.

Training contains generated dimensional expressions, mixed-scale benchmark tables, temperature offsets, currencies with dated rates, and unknown units. An independent dimensional algebra engine verifies supported conversions; unavailable conversion evidence must produce unresolved results. Hold out compound dimensions, unit aliases, and scale combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Milliseconds and microseconds must not be compared directly, and affine temperatures cannot use multiplicative conversion alone. A summary that omits a decisive exception must not erase the exception from the answer. Force a query requiring descent into a rarely accessed subtree and verify the exact underlying evidence is consumed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/cost_model.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/pdf_processor.py` (`PDFVisionAnalyzer._analyze_image`, `PDFVisionAnalyzer.process_pdf`). Donor baseline: Vision produces page transcription text; there is no structured equation, table, or cross-modal verification. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
