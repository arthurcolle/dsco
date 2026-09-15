# 994 — Sparse matrix native kernel designer: Learned execution-error recovery

Implement the learned execution-error recovery feature for the Sparse matrix native kernel designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native sparse matrix kernels and representation choices that preserve indexing, duplicate-entry, and numerical semantics.

REPL `sparse_matrices`, `sparsity_profiles`, and `dense_references` retain complete matrices, layouts, and reference computations. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code partitions row structures, delegates format choices, and synthesizes CSR/blocked traversal candidates with checked indices. Role output: Final `SparseKernelCandidate` handle contains native code, representation contract, bounds, and dense-parity counterexamples. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Construct paired episodes containing a recoverable syntax, schema, or reference error and an independently verified repair. Train the root to inspect execution feedback, preserve completed work, and generate the smallest valid continuation. Keep original and corrected code plus the repair transformation.

Train from generated sparsity patterns, duplicate entries, layout conversions, and independently scored compiled-kernel execution episodes. A frozen dense implementation verifies every output under declared arithmetic tolerance and duplicate-coalescing rules. Hold out sparsity motifs, dimensions, empty rows, duplicate patterns, and value distributions with matrix lineage grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Unsorted duplicate column entries are silently dropped, producing plausible but wrong rows on only irregular matrices. Inject an undefined buffer and a completed effect before the failure. Recovery must resolve the buffer without repeating the effect; inventing a final answer cannot count as repair.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/extension/eigen_backend.c`, `src/vecstore.c`, `src/vm.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/agents/embeddings.py` (`EmbeddingAgent._cosine_sim`). Donor baseline: Computes vector similarity over Python lists; sparse native matrix kernel generation is a proposed extension. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
