# 1328 — Unit and scale normalizer: Adaptive recursion-depth policy

Implement the adaptive recursion-depth policy feature for the Unit and scale normalizer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a unit RLM to normalize quantitative observations without losing dimensions, scales, or measurement conventions.

External QUANTITIES and UNIT_DEFINITIONS preserve source values, prefixes, reference conditions, denominator units, and uncertainty intervals. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs exact conversions, recursively resolves ambiguous unit context, and buffers normalized values alongside untouched source quantities. Role output: Return NORMALIZED_QUANTITIES with conversion derivations, assumptions, original references, and incompatibility errors. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create development instances whose useful decomposition ranges from direct environment computation to nested child RLMs. Train the root to decide between a leaf call and another recursive environment based on task structure and remaining budget. Record the actual tree rather than a declared depth.

Training contains generated dimensional expressions, mixed-scale benchmark tables, temperature offsets, currencies with dated rates, and unknown units. An independent dimensional algebra engine verifies supported conversions; unavailable conversion evidence must produce unresolved results. Hold out compound dimensions, unit aliases, and scale combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Milliseconds and microseconds must not be compared directly, and affine temperatures cannot use multiplicative conversion alone. A simple case must not gain reward from unnecessary recursion. A nested case must preserve dependent intermediate results, while attempts to exceed the host depth limit fail without changing that limit.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/cost_model.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/pdf_processor.py` (`PDFVisionAnalyzer._analyze_image`, `PDFVisionAnalyzer.process_pdf`). Donor baseline: Vision produces page transcription text; there is no structured equation, table, or cross-modal verification. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
