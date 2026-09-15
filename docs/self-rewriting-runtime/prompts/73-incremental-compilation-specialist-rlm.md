# 73 — Train an RLM for semantic incremental native compilation

Train an incremental-compilation specialist RLM that makes resident self-rewrites cheap by rebuilding only semantically affected native units. Define content identities over normalized IR, ABI, imported semantic contracts, target ISA, and compiler configuration. Partition bounded programs into reusable compilation units; distinguish source spelling changes from changes that affect behavior or generated assumptions. Reuse cannot be decided from a filename timestamp or function name.

Keep program versions, dependency graphs, build receipts, and unit artifacts in external REPL variables. The root receives change summaries, inspects semantic neighborhoods through code, and delegates independent partition or dependency questions to child RLMs. Retain compiled-unit handles and reuse justifications. The final environment variable identifies a complete linkable candidate and its reuse proof obligations.

Train a root adapter on edit/build trajectories, independently rewarding semantic parity with clean rebuilds before saved compilation work. Penalize missed dependency changes and overfragmentation overhead. Hold out edit classes, module shapes, and compiler settings. Perform a bounded real parameter update and compare the trained checkpoint with its frozen parent and always-rebuild baseline at equal workloads.

Implement a minimal isolated cache/link interface if needed. Falsifying test: change an inline constant or imported record layout without renaming its symbol; every affected specialization must rebuild. A comment-only change may reuse code if normalized semantics and debugging policy allow it. Compare incremental and clean artifacts by executable behavior across hidden cases, not byte identity alone. Activate a qualified linked candidate with heap/session continuity and audited absence of resident exec. Deliberately substitute an object built for another target and require rejection before linking or publication.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/ast.c`, `src/plugin.c`, `src/vm.c`, `src/autoresearch.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.get_ast`, `MetaToolRegistry.transform_tool`: The donor caches AST descriptions and transforms whole registered tools, not semantic native dependency units. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
