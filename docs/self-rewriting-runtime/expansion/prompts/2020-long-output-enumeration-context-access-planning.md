# 2020 — Complete long-output enumerator: Learned context-access planning

Implement the learned context-access planning feature for the Complete long-output enumerator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an enumeration RLM to extract every qualifying item from large corpora without truncation, duplicates, or invented completeness.

External CORPUS_PARTITIONS, MATCH_RULES, and OUTPUT_BUFFER preserve partition boundaries, item identities, progress receipts, and ambiguity. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs partitioned scans, recursively delegates bounded extraction, and buffers deduplicated results with coverage checks before finalization. Role output: Return COMPLETE_ENUMERATION with result handle, counts, partition coverage, duplicates resolved, and unresolved membership. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Expose context-region descriptors and bounded reads while keeping the full input outside the root window. Train the root to generate access programs that retrieve discriminating regions and recursively examine relevant slices. Log coverage, read amplification, and every region supporting the final artifact.

Training uses generated inventories, long local symbol lists, scattered matches, repeated entries, negative cases, and interrupted scans. Exact controlled predicates provide reference sets; semantic inclusion tasks use independently labeled corpora and explicit unresolved items. Hold out corpus size regimes, partition layouts, and predicate combinations. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Retrieving several relevant examples must not become a claim that all matching functions were enumerated. Plant decisive information outside the usual prefix and supply a plausible distractor near the beginning. A policy that answers from previews alone must fail the oracle, and an unbounded full-context copy must be detected.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/vfs.c`, `src/semantic.c` and `/Users/arthurcolle/Dsco/dsco-mcp-openrouter-client/universal_dspy_multihop.py` (`UniversalContentRetriever.retrieve`, `NotesExtractor`). Donor baseline: Retrieval merges heterogeneous scores; extracted notes have no statement-level evidence references. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
