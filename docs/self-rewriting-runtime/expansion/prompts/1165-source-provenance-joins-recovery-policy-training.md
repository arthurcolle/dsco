# 1165 — Source provenance joiner: Checkpoint recovery policy training

Implement the checkpoint recovery policy training feature for the Source provenance joiner RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an evidence RLM to join reported claims with originating executions rather than copied descriptions.

External CLAIMS, SOURCE_OBJECTS, and EXECUTION_RECEIPTS retain content hashes, derivation edges, revisions, and collection boundaries. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. The root programs indexed joins, recursively delegates ambiguous ancestry checks, and buffers resolved or explicitly uncertain support paths. Role output: Return a typed PROVENANCE_JOIN containing origin edges, unresolved links, and evidence handles. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create interrupted role episodes with committed buffers, pending child calls, and explicit effect receipts. Train the root to reconstruct logical state, reconcile pending work, and continue from a compatible model/environment tuple. Treat unavailable state as a recoverable diagnostic rather than invented memory.

Training cases combine generated provenance graphs, real local execution receipts, copied reports, and missing parents. An independent fixture oracle checks graph ancestry and receipt identity; human adjudication handles genuinely ambiguous attribution. Hold out derivation graph shapes and report-generation families. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A copied benchmark summary must not become an independent confirming experiment merely because its filename differs. Resume after an effect completed but before its acknowledgement was recorded. Reconcile the receipt instead of repeating the effect; incompatible state schemas or changed model identities must prevent a misleading continuation claim.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/trace_kg_store.c`, `src/vfs.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` (`UniversalContentRetriever.retrieve`, `NotesExtractor`). Donor baseline: Retrieval merges heterogeneous scores; extracted notes have no statement-level evidence references. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
