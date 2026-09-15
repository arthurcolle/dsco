# 1149 — Source provenance joiner: Evidence-based stopping policy

Implement the evidence-based stopping policy feature for the Source provenance joiner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an evidence RLM to join reported claims with originating executions rather than copied descriptions.

External CLAIMS, SOURCE_OBJECTS, and EXECUTION_RECEIPTS retain content hashes, derivation edges, revisions, and collection boundaries. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. The root programs indexed joins, recursively delegates ambiguous ancestry checks, and buffers resolved or explicitly uncertain support paths. Role output: Return a typed PROVENANCE_JOIN containing origin edges, unresolved links, and evidence handles. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train a root decision among continue, return a verified environment result, and terminate unresolved. Development episodes include misleading high confidence, repetitive verification, and unresolved obligations. Optimize task completion with explicit observation requirements and costs, using oracle results rather than self-reported confidence.

Training cases combine generated provenance graphs, real local execution receipts, copied reports, and missing parents. An independent fixture oracle checks graph ancestry and receipt identity; human adjudication handles genuinely ambiguous attribution. Hold out derivation graph shapes and report-generation families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A copied benchmark summary must not become an independent confirming experiment merely because its filename differs. Repeated identical child opinions cannot satisfy a missing obligation. A complete verified result should survive the stop decision unchanged; exhausted budgets must produce unresolved status instead of fabricated success.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/trace_kg_store.c`, `src/vfs.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` (`UniversalContentRetriever.retrieve`, `NotesExtractor`). Donor baseline: Retrieval merges heterogeneous scores; extracted notes have no statement-level evidence references. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
