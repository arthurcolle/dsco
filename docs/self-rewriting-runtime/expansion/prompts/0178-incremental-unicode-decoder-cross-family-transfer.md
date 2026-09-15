# 178 — Incremental Unicode decoder: Measured cross-family transfer

Implement the measured cross-family transfer feature for the Incremental Unicode decoder RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native Unicode decoding procedures that preserve exact scalar values and error positions across arbitrary input chunking.

REPL variables `utf8_bytes`, `chunk_schedules`, and `decoder_states` retain byte streams, boundary splits, and partial sequences. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code isolates multibyte boundaries, invokes children on sequence classes, and merges explicit decoder transition proposals. Role output: Final `UnicodeDecoderCandidate` handle contains transition tables, scalar policy, native code identity, and chunk-invariance test receipts. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role on selected development families, then compare adapted and unadapted roots on disjoint family structures with fixed leaves. Include component ablations and a negative-transfer condition. Retain task ancestry so apparent transfer cannot come from shared templates or answer leakage.

Train from reference-labeled scalar sequences, malformed encodings, split-point variations, and successful boundary-repair action trajectories. A fixed strict Unicode decoder checks decoded scalars, consumed bytes, incomplete status, and precise invalid-sequence locations. Hold out Unicode planes, malformed-sequence families, and adversarial chunk schedules rather than merely new text sentences. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A truncated multibyte prefix joined to an illegal continuation is incorrectly accepted after a chunk boundary. A learned shortcut that works only for one identifier or ordering must fail on renamed and structurally changed cases. Attribute any gain to the trained root and report families harmed by the update.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/buffer_store.c`, `src/buffer_textedit.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_openrouter_streaming.py` (`_safe_stream`). Donor baseline: Yields streamed response objects; it does not implement or verify Unicode decoding. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
