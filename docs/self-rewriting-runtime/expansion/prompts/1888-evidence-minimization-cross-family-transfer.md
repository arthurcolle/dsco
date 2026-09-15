# 1888 — Evidence set minimizer: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Evidence set minimizer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an evidence RLM to find small sufficient support sets without deleting decisive counterexamples or required conditions.

External CLAIM_OBLIGATIONS and EVIDENCE_SETS preserve proof dependencies, source independence, contradictions, costs, and scope restrictions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs deletion tests, recursively checks obligation coverage, and buffers minimal validated supports with indispensability witnesses. Role output: Return MINIMAL_EVIDENCE with retained handles, removal justifications, blocking dissent, and unresolved sufficiency. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Training uses generated proof graphs, controlled contract evidence, redundant observations, conditional conclusions, and adversarial distractors. Independent obligation predicates check sufficiency on formal fixtures; natural support judgments use blinded review and allow uncertainty. Hold out dependency structures, redundancy ratios, and claim families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Removing the only cancellation counterexample must not make a smaller evidence packet falsely endorse an unsafe rewrite. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/plan_dag.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` (`UniversalContentRetriever.retrieve`, `NotesExtractor`). Donor baseline: Retrieval merges heterogeneous scores; extracted notes have no statement-level evidence references. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
