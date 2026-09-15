# 1403 — Contradiction localizer: Drift-adaptive role policy

Implement the drift-adaptive role policy feature for the Contradiction localizer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a contradiction RLM to locate the smallest incompatible claim pair and the scope making it contradictory.

External CLAIM_SET and SOURCE_SPANS carry revision, time, population, polarity, units, and conditional assumptions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code groups comparable claims, recursively checks candidate conflicts, and buffers exact supporting spans with scope explanations. Role output: Return LOCALIZED_CONFLICTS with minimal evidence pairs, compatibility conditions, and unresolved interpretation. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to distinguish transient anomalies, changed input distributions, and changed task semantics from chronological observations. It selects bounded diagnostic probes and proposes scoped relearning. Keep an immutable pre-change checkpoint and evaluate detection delay, false alarms, and post-change correctness.

Training contains generated incompatible propositions, conditionally compatible statements, and annotated disagreements in technical documents. Formal fixtures use a reference satisfiability checker; natural claims receive independent entailment review rather than assumed deterministic truth. Hold out predicate families, scope combinations, and contradiction wording. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Different worker counts can explain different latency claims; equal-scope incompatible bounds must still be detected. One extreme outlier must not trigger wholesale relearning. A sustained change affecting the role's contract must be detected without access to future observations, and unaffected task families must retain their prior behavior.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/semantic.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` (`UniversalContentRetriever.retrieve`, `NotesExtractor`). Donor baseline: Retrieval merges heterogeneous scores; extracted notes have no statement-level evidence references. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
