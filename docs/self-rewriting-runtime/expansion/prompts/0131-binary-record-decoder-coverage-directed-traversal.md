# 131 — Bounded binary-record decoder: Coverage-directed recursive traversal

Implement the coverage-directed recursive traversal feature for the Bounded binary-record decoder RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn to synthesize bounded native decoders for length-prefixed journal records with explicit byte-order and checksum semantics.

External REPL variables `record_bytes`, `format_spec`, and `decode_traces` retain complete binary corpora and parser transitions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code slices headers and payloads, recursively assigns field interpretations, then composes checked decoding states. Role output: Final environment handle resolves `DecoderCandidate` containing native artifact hash, field map, bounds contract, and executable witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Represent task partitions and their dependencies as an explicit coverage graph. Train the root to traverse unresolved frontiers, dispatch independent work, and mark a node complete only when its typed result satisfies the role contract. Preserve coverage certificates with the output object.

Training episodes pair generated valid records and structural corruptions with independently executed decoder outcomes and root actions. A frozen reference decoder and checksum implementation verify exact fields, consumed offsets, rejection classes, and memory boundaries. Hold out format layouts, endian combinations, nesting depths, and corruption operators with all mutated siblings grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A forged length wraps arithmetic and makes a checksum-valid prefix conceal an out-of-bounds payload access. Drop one required region while returning a plausible aggregate. The completeness check must reject it; duplicate completion messages cannot compensate for a missing node or inflate coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/chronicle.c`, `src/execution_recovery.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`CodeAnalysisToolkit.parse_python_ast`). Donor baseline: Extracts syntax structure from Python; native binary parsing is a proposed transfer. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
