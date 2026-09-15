# 1451 — Entity identity resolver: Coverage-directed recursive traversal

Implement the coverage-directed recursive traversal feature for the Entity identity resolver RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train an entity RLM to resolve identities across traces and documents without collapsing similar names.

External ENTITY_MENTIONS, IDENTIFIERS, and RELATION_CONTEXT preserve namespaces, revisions, timestamps, aliases, and observed attributes. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs blocking and joins, recursively investigates ambiguous pairs, and buffers identity clusters with reversible merge evidence. Role output: Return ENTITY_BINDINGS with namespace-qualified identities, evidence-backed aliases, rejected merges, and unresolved candidates. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Represent task partitions and their dependencies as an explicit coverage graph. Train the root to traverse unresolved frontiers, dispatch independent work, and mark a node complete only when its typed result satisfies the role contract. Preserve coverage certificates with the output object.

Training combines generated entity registries, renamed functions, corporate aliases, same-name subjects, and incomplete identifiers. Controlled registries provide exact identities; blinded adjudicators label natural ambiguity and allow unresolved pairs. Hold out naming conventions, namespace structures, and alias histories. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Two functions with the same name in different libraries must not share behavioral evidence automatically. Drop one required region while returning a plausible aggregate. The completeness check must reject it; duplicate completion messages cannot compensate for a missing node or inflate coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/context_fabric.c`, `src/semantic.c`, `src/vfs.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/chunked_analyzer.py` (`KnowledgeExtractionSignature`, `ChunkedDocumentAnalyzer._aggregate_results`). Donor baseline: Extraction includes utterances and beliefs, but aggregation discards utterances and keeps only coarse chunk positions. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
