# 271 — Canonical record serializer: Executable curriculum compiler

Implement the executable curriculum compiler feature for the Canonical record serializer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native serializers whose canonical bytes preserve typed record semantics across optional fields and schema ordering variations.

REPL variables `logical_records`, `wire_schema`, and `encoding_pairs` retain complete record graphs and byte-level reference outputs. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs inspect field groups, recursively propose encoding rules, and join them through explicit canonical-order contracts. Role output: Final `SerializerCandidate` handle supplies native code, canonical ordering rules, schema fingerprint, and round-trip evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Compile a task grammar with explicit difficulty axes, valid transformations, and known failure labels. Generate positive and negative episodes, retaining their originating family identifiers. Train the role's root policy on a balanced development curriculum and expose a reproducible curriculum-build command.

Train from generated records, alternate encodings, schema perturbations, and round-trip failures with independently labeled root actions. An immutable reference encoder/decoder checks canonical bytes, decoded types, unknown-field handling, and exact round-trip semantics. Hold out optional-field combinations, integer widths, nested schemas, and equivalent presentation permutations grouped by logical record. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A missing field and an explicit null collapse to identical bytes although their application meanings differ. Reject a generator that changes the answer without updating its reference or produces duplicate families under different seeds. Measure valid-case coverage and trained-root success separately.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/json_util.c`, `src/json_fast.c`, `src/capsule.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/agents/sqlite.py` (`SQLiteAgent.schema`). Donor baseline: Returns schemas for dictionary-backed tables; it does not provide native canonical serialization. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
