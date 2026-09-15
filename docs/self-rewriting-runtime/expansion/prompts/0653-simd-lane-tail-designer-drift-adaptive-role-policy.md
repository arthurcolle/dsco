# 653 — SIMD lane-and-tail kernel designer: Drift-adaptive role policy

Implement the drift-adaptive role policy feature for the SIMD lane-and-tail kernel designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native vector kernels that choose lane operations and tail strategies while preserving an explicit scalar contract.

REPL `scalar_ir`, `batch_inputs`, and `isa_contract` retain full expressions, alignment cases, and allowed vector instructions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code splits loop bodies, delegates lane mapping, and joins vector blocks with checked scalar tails. Role output: Final `VectorKernelCandidate` handle contains native code hashes, lane maps, tail guards, and scalar-parity receipts. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to distinguish transient anomalies, changed input distributions, and changed task semantics from chronological observations. It selects bounded diagnostic probes and proposes scoped relearning. Keep an immutable pre-change checkpoint and evaluate detection delay, false alarms, and post-change correctness.

Train from compiled vectorization attempts with externally measured parity, guard-page behavior, instruction identity, and complete-call timing. A frozen scalar implementation checks every output and permitted overflow behavior before performance contributes reward. Hold out expression structures, alignments, vector-boundary lengths, and ISA variants with equivalent kernel families grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A masked-looking tail load crosses a guard page even though unused lanes never reach the final output. One extreme outlier must not trigger wholesale relearning. A sustained change affecting the role's contract must be detected without access to future observations, and unaffected task families must retain their prior behavior.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `include/simd.h`, `src/vecstore.c`, `src/vm.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`ASTAnalyzer.transform`). Donor baseline: Transforms Python ASTs; native SIMD lane selection and scalar parity qualification are proposed. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
