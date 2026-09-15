# 68 — Train an RLM to generate SIMD native batch kernels

Train a SIMD-codegen specialist RLM that rewrites a bounded scalar native cell into a batch implementation using the current host's supported vector ISA. Begin with integer scoring or byte classification where exact scalar parity is well defined. Generate vector-width operations, checked loads, masked or scalar tails, and a scalar fallback. Inspect emitted instructions to establish that the candidate actually vectorizes rather than merely advertising a batch interface.

Keep scalar IR, record layouts, profiles, and candidate assembly in external REPL variables. The root sees operation counts and ISA metadata, uses code to inspect hot loops, and delegates independent vectorization or tail obligations to child RLMs. Retain candidate kernels and test receipts; return the selected kernel through an environment handle. Bound recursion and compilation attempts.

Train a root adapter from executable vectorization trajectories, independently rewarding parity first and end-to-end throughput second, including packing and dispatch cost. Hold out lengths, alignment patterns, and expression structures. Perform a real bounded update and compare the trained policy with its frozen parent under equal compile and evaluation budgets; retain the scalar champion if no candidate qualifies.

Provide a minimal native-cell loader if absent. Falsifying test: test lengths around every vector boundary, guard-page adjacent buffers, misaligned input, and extreme integer values. Any unsupported ISA must select a truthful fallback without illegal instructions. Activate a qualified batch kernel inside the resident process and prove changed native bytes, heap-sentinel/session continuity, old-frame completion, and audited absence of resident exec. A fast kernel that reads past the admitted input or changes overflow semantics must never activate; report whole-call performance rather than arithmetic-only speed.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `include/simd.h`, `src/vm.c`, `src/vecstore.c`, `src/plugin.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer.transform`, `MetaToolRegistry.register_code`: The donor transforms executable Python functions but does not generate vector instructions or establish scalar parity. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
