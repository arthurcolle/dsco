# 446 — Logical snapshot codec designer: Hierarchical context navigation

Implement the hierarchical context navigation feature for the Logical snapshot codec designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn compact native codecs for immutable logical snapshots while preserving shared references, cycles, and schema identities.

External `object_graphs`, `snapshot_schema`, and `encoded_snapshots` variables retain full state graphs and candidate byte streams. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs discover repeated structures, recurse over graph components, and synthesize reference tables plus decoding order. Role output: Final `SnapshotCodec` handle includes native encoder/decoder, logical schema, reference policy, and exact restoration witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Build lossless external indexes over task contexts and train the root to navigate coarse descriptors into exact evidence regions. Child RLMs inspect selected subtrees; summaries guide navigation but never replace authoritative content. Record access paths and evidence references for the final artifact.

Train on generated object graphs, corrupted snapshots, compression choices, and independently verified encode/decode action trajectories. A frozen graph-isomorphism checker validates restored values, alias relationships, cycles, and declared deterministic encoding rules. Hold out graph motifs, cycle lengths, scalar ranges, and corruption patterns with related snapshots grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Two aliases become independent copies after decoding, so later mutation changes the restored computation behavior. A summary that omits a decisive exception must not erase the exception from the answer. Force a query requiring descent into a rarely accessed subtree and verify the exact underlying evidence is consumed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/recovery.c`, `src/sequence_state.c`, `src/json_util.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`CodeSandbox.execute`). Donor baseline: Keeps interpreter locals resident but does not encode lossless portable snapshots. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
