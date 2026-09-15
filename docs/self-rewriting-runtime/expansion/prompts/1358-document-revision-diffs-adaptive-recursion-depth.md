# 1358 — Document revision differ: Adaptive recursion-depth policy

Implement the adaptive recursion-depth policy feature for the Document revision differ RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a revision RLM to identify substantive evidence changes across documents despite layout and wording churn.

External OLD_DOCUMENT, NEW_DOCUMENT, and SPAN_ALIGNMENT preserve content hashes, structural sections, tables, and claim references. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code aligns sections, recursively compares changed regions, and buffers changed propositions separately from formatting-only edits. Role output: Return REVISION_DELTA with changed claim handles, preserved equivalences, deleted evidence, and affected dependencies. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create development instances whose useful decomposition ranges from direct environment computation to nested child RLMs. Train the root to decide between a leaf call and another recursive environment based on task structure and remaining budget. Record the actual tree rather than a declared depth.

Training includes generated revisions, real local source snapshots, moved paragraphs, corrected numbers, deletions, and renamed entities. Controlled edits supply exact ground truth; independently reviewed natural diffs retain uncertainty about semantic equivalence. Hold out edit operators, document structures, and paraphrase families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Replacing one denominator at an unchanged path must invalidate the derived comparison despite nearly identical text. A simple case must not gain reward from unnecessary recursion. A nested case must preserve dependent intermediate results, while attempts to exceed the host depth limit fail without changing that limit.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/vfs.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/analysis_cache.py` (`AnalysisCache._get_cache_key`). Donor baseline: Analysis keys contain path and configuration but omit content digests and derived dependency identities. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
