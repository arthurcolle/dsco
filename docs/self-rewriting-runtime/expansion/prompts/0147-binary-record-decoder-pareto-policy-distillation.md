# 147 — Bounded binary-record decoder: Constrained Pareto policy distillation

Implement the constrained pareto policy distillation feature for the Bounded binary-record decoder RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn to synthesize bounded native decoders for length-prefixed journal records with explicit byte-order and checksum semantics.

External REPL variables `record_bytes`, `format_spec`, and `decode_traces` retain complete binary corpora and parser transitions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code slices headers and payloads, recursively assigns field interpretations, then composes checked decoding states. Role output: Final environment handle resolves `DecoderCandidate` containing native artifact hash, field map, bounds contract, and executable witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Collect independently verified root trajectories with quality, latency, memory, and cost measurements. Construct a constrained frontier and distill policies conditioned on declared operating budgets. Keep correctness and capability rules as hard requirements rather than exchangeable score terms.

Training episodes pair generated valid records and structural corruptions with independently executed decoder outcomes and root actions. A frozen reference decoder and checksum implementation verify exact fields, consumed offsets, rejection classes, and memory boundaries. Hold out format layouts, endian combinations, nesting depths, and corruption operators with all mutated siblings grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A forged length wraps arithmetic and makes a checksum-valid prefix conceal an out-of-bounds payload access. A fast invalid trajectory must never enter the feasible frontier. Tightening a resource budget should produce a valid lower-cost policy or explicit infeasibility, not an unreported reduction in required task coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/chronicle.c`, `src/execution_recovery.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`CodeAnalysisToolkit.parse_python_ast`). Donor baseline: Extracts syntax structure from Python; native binary parsing is a proposed transfer. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
