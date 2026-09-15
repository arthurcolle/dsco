# 128 — Bounded binary-record decoder: Adaptive recursion-depth policy

Implement the adaptive recursion-depth policy feature for the Bounded binary-record decoder RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn to synthesize bounded native decoders for length-prefixed journal records with explicit byte-order and checksum semantics.

External REPL variables `record_bytes`, `format_spec`, and `decode_traces` retain complete binary corpora and parser transitions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code slices headers and payloads, recursively assigns field interpretations, then composes checked decoding states. Role output: Final environment handle resolves `DecoderCandidate` containing native artifact hash, field map, bounds contract, and executable witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create development instances whose useful decomposition ranges from direct environment computation to nested child RLMs. Train the root to decide between a leaf call and another recursive environment based on task structure and remaining budget. Record the actual tree rather than a declared depth.

Training episodes pair generated valid records and structural corruptions with independently executed decoder outcomes and root actions. A frozen reference decoder and checksum implementation verify exact fields, consumed offsets, rejection classes, and memory boundaries. Hold out format layouts, endian combinations, nesting depths, and corruption operators with all mutated siblings grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A forged length wraps arithmetic and makes a checksum-valid prefix conceal an out-of-bounds payload access. A simple case must not gain reward from unnecessary recursion. A nested case must preserve dependent intermediate results, while attempts to exceed the host depth limit fail without changing that limit.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/chronicle.c`, `src/execution_recovery.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`CodeAnalysisToolkit.parse_python_ast`). Donor baseline: Extracts syntax structure from Python; native binary parsing is a proposed transfer. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
