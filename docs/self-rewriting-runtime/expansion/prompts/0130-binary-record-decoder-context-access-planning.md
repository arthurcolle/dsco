# 130 — Bounded binary-record decoder: Learned context-access planning

Implement the learned context-access planning feature for the Bounded binary-record decoder RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn to synthesize bounded native decoders for length-prefixed journal records with explicit byte-order and checksum semantics.

External REPL variables `record_bytes`, `format_spec`, and `decode_traces` retain complete binary corpora and parser transitions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code slices headers and payloads, recursively assigns field interpretations, then composes checked decoding states. Role output: Final environment handle resolves `DecoderCandidate` containing native artifact hash, field map, bounds contract, and executable witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Expose context-region descriptors and bounded reads while keeping the full input outside the root window. Train the root to generate access programs that retrieve discriminating regions and recursively examine relevant slices. Log coverage, read amplification, and every region supporting the final artifact.

Training episodes pair generated valid records and structural corruptions with independently executed decoder outcomes and root actions. A frozen reference decoder and checksum implementation verify exact fields, consumed offsets, rejection classes, and memory boundaries. Hold out format layouts, endian combinations, nesting depths, and corruption operators with all mutated siblings grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A forged length wraps arithmetic and makes a checksum-valid prefix conceal an out-of-bounds payload access. Plant decisive information outside the usual prefix and supply a plausible distractor near the beginning. A policy that answers from previews alone must fail the oracle, and an unbounded full-context copy must be detected.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/chronicle.c`, `src/execution_recovery.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`CodeAnalysisToolkit.parse_python_ast`). Donor baseline: Extracts syntax structure from Python; native binary parsing is a proposed transfer. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
