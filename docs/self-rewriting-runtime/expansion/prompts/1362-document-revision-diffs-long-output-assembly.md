# 1362 — Document revision differ: Verified long-output assembly

Implement the verified long-output assembly feature for the Document revision differ RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a revision RLM to identify substantive evidence changes across documents despite layout and wording churn.

External OLD_DOCUMENT, NEW_DOCUMENT, and SPAN_ALIGNMENT preserve content hashes, structural sections, tables, and claim references. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code aligns sections, recursively compares changed regions, and buffers changed propositions separately from formatting-only edits. Role output: Return REVISION_DELTA with changed claim handles, preserved equivalences, deleted evidence, and affected dependencies. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to assemble semantically required role outputs exceeding one generation, using an expected coverage manifest and externally checked fragments. Return the typed result handle and digest with streaming backpressure. Padding cannot satisfy output length; validate the whole artifact against domain obligations after assembly.

Training includes generated revisions, real local source snapshots, moved paragraphs, corrected numbers, deletions, and renamed entities. Controlled edits supply exact ground truth; independently reviewed natural diffs retain uncertainty about semantic equivalence. Hold out edit operators, document structures, and paraphrase families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Replacing one denominator at an unchanged path must invalidate the derived comparison despite nearly identical text. A correct prefix followed by silent truncation must fail completeness. Reordering asynchronous fragments or repeating one fragment must be caught before final publication, without copying the whole output into the root prompt.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/vfs.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/analysis_cache.py` (`AnalysisCache._get_cache_key`). Donor baseline: Analysis keys contain path and configuration but omit content digests and derived dependency identities. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
