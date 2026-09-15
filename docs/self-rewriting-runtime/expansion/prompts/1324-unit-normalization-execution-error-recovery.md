# 1324 — Unit and scale normalizer: Learned execution-error recovery

Implement the learned execution-error recovery feature for the Unit and scale normalizer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a unit RLM to normalize quantitative observations without losing dimensions, scales, or measurement conventions.

External QUANTITIES and UNIT_DEFINITIONS preserve source values, prefixes, reference conditions, denominator units, and uncertainty intervals. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs exact conversions, recursively resolves ambiguous unit context, and buffers normalized values alongside untouched source quantities. Role output: Return NORMALIZED_QUANTITIES with conversion derivations, assumptions, original references, and incompatibility errors. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Construct paired episodes containing a recoverable syntax, schema, or reference error and an independently verified repair. Train the root to inspect execution feedback, preserve completed work, and generate the smallest valid continuation. Keep original and corrected code plus the repair transformation.

Training contains generated dimensional expressions, mixed-scale benchmark tables, temperature offsets, currencies with dated rates, and unknown units. An independent dimensional algebra engine verifies supported conversions; unavailable conversion evidence must produce unresolved results. Hold out compound dimensions, unit aliases, and scale combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Milliseconds and microseconds must not be compared directly, and affine temperatures cannot use multiplicative conversion alone. Inject an undefined buffer and a completed effect before the failure. Recovery must resolve the buffer without repeating the effect; inventing a final answer cannot count as repair.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/cost_model.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/pdf_processor.py` (`PDFVisionAnalyzer._analyze_image`, `PDFVisionAnalyzer.process_pdf`). Donor baseline: Vision produces page transcription text; there is no structured equation, table, or cross-modal verification. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
