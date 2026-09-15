# 635 — SIMD lane-and-tail kernel designer: Counterfactual branch preference training

Implement the counterfactual branch preference training feature for the SIMD lane-and-tail kernel designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native vector kernels that choose lane operations and tail strategies while preserving an explicit scalar contract.

REPL `scalar_ir`, `batch_inputs`, and `isa_contract` retain full expressions, alignment cases, and allowed vector instructions. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code splits loop bodies, delegates lane mapping, and joins vector blocks with checked scalar tails. Role output: Final `VectorKernelCandidate` handle contains native code hashes, lane maps, tail guards, and scalar-parity receipts. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Fork identical development states into isolated resettable environment snapshots or a fixed replay oracle with matched randomness. Reject comparisons when reset fidelity is uncertain. Execute alternative root programs under equal budgets, derive preferences from correctness then resource use, and train a declared preference objective on root continuations.

Train from compiled vectorization attempts with externally measured parity, guard-page behavior, instruction identity, and complete-call timing. A frozen scalar implementation checks every output and permitted overflow behavior before performance contributes reward. Hold out expression structures, alignments, vector-boundary lengths, and ISA variants with equivalent kernel families grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A masked-looking tail load crosses a guard page even though unused lanes never reach the final output. A shorter incorrect branch must lose to a longer correct branch. Swapping presentation order must preserve the preference, and branches with different starting states must be rejected as unmatched.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `include/simd.h`, `src/vecstore.c`, `src/vm.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` (`ASTAnalyzer.transform`). Donor baseline: Transforms Python ASTs; native SIMD lane selection and scalar parity qualification are proposed. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
