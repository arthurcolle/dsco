# 1389 — Contradiction localizer: Evidence-based stopping policy

Implement the evidence-based stopping policy feature for the Contradiction localizer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a contradiction RLM to locate the smallest incompatible claim pair and the scope making it contradictory.

External CLAIM_SET and SOURCE_SPANS carry revision, time, population, polarity, units, and conditional assumptions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code groups comparable claims, recursively checks candidate conflicts, and buffers exact supporting spans with scope explanations. Role output: Return LOCALIZED_CONFLICTS with minimal evidence pairs, compatibility conditions, and unresolved interpretation. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train a root decision among continue, return a verified environment result, and terminate unresolved. Development episodes include misleading high confidence, repetitive verification, and unresolved obligations. Optimize task completion with explicit observation requirements and costs, using oracle results rather than self-reported confidence.

Training contains generated incompatible propositions, conditionally compatible statements, and annotated disagreements in technical documents. Formal fixtures use a reference satisfiability checker; natural claims receive independent entailment review rather than assumed deterministic truth. Hold out predicate families, scope combinations, and contradiction wording. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Different worker counts can explain different latency claims; equal-scope incompatible bounds must still be detected. Repeated identical child opinions cannot satisfy a missing obligation. A complete verified result should survive the stop decision unchanged; exhausted budgets must produce unresolved status instead of fabricated success.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/semantic.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` (`UniversalContentRetriever.retrieve`, `NotesExtractor`). Donor baseline: Retrieval merges heterogeneous scores; extracted notes have no statement-level evidence references. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
