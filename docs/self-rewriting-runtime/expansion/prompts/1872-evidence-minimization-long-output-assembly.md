# 1872 — Evidence set minimizer: Verified long-output assembly

Implement the verified long-output assembly feature for the Evidence set minimizer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an evidence RLM to find small sufficient support sets without deleting decisive counterexamples or required conditions.

External CLAIM_OBLIGATIONS and EVIDENCE_SETS preserve proof dependencies, source independence, contradictions, costs, and scope restrictions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs deletion tests, recursively checks obligation coverage, and buffers minimal validated supports with indispensability witnesses. Role output: Return MINIMAL_EVIDENCE with retained handles, removal justifications, blocking dissent, and unresolved sufficiency. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to assemble semantically required role outputs exceeding one generation, using an expected coverage manifest and externally checked fragments. Return the typed result handle and digest with streaming backpressure. Padding cannot satisfy output length; validate the whole artifact against domain obligations after assembly.

Training uses generated proof graphs, controlled contract evidence, redundant observations, conditional conclusions, and adversarial distractors. Independent obligation predicates check sufficiency on formal fixtures; natural support judgments use blinded review and allow uncertainty. Hold out dependency structures, redundancy ratios, and claim families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Removing the only cancellation counterexample must not make a smaller evidence packet falsely endorse an unsafe rewrite. A correct prefix followed by silent truncation must fail completeness. Reordering asynchronous fragments or repeating one fragment must be caught before final publication, without copying the whole output into the root prompt.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/plan_dag.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` (`UniversalContentRetriever.retrieve`, `NotesExtractor`). Donor baseline: Retrieval merges heterogeneous scores; extracted notes have no statement-level evidence references. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
