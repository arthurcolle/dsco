# 10 — Atomic activation of cooperating executable cells

Let DSCO activate an improvement spanning several cooperating native functions as one indivisible resident change. A bundle names exact code generations, their ABI contracts, import edges, state roots, and required evidence. Calls acquire a bundle-generation root so an encoder, scorer, and reducer cannot accidentally come from incompatible releases during one computation.

Implement prepare, validate, activate, and retire phases over immutable bundle descriptors. Preparation resolves all internal references and checks dependency closure, shared-state compatibility, import versions, and qualification receipts. Activation compares the expected active bundle generation and publishes one root pointer with defined synchronization. Any missing cell, failed migration, or stale expectation leaves the old root active. Maintain a bounded history sufficient for rollback without making arbitrary individual cell updates visible mid-transaction.

Use a minimal three-function native pipeline whose input encoding changes alongside its scorer. The first version and second version should each be correct internally but produce an unmistakable error when mixed. Include optional state migration through a local interface if no reusable facility exists, rather than depending on another numbered feature. Explain how callers pin nested dispatches to the acquired bundle instead of rereading global active cells on each step.

Falsifying test: run concurrent calls continuously while repeatedly swapping complete bundles. Every observed result must belong to the valid old or valid new result set; the deliberate mixed-version sentinel must never appear. Fail preparation after the first candidate cell is ready and prove no partial publication occurred. Race two bundle activations and require one winner. Confirm old in-flight computations finish correctly, retired bundles release only after their references drain, and the process identity remains unchanged throughout.

Serialize acceptance-epoch validation and publication against revocation. Test receipt invalidation immediately before publication: an unchanged parent generation must not permit activation.

Prove continuity with a resident heap sentinel's address and contents, a live session handle/counter, completion of an old-generation frame, and audited absence of exec; PID equality alone is insufficient.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/plugin.c`, `src/vm.c`, `src/lingo_workflow.c`, `src/execution_kernel.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.compose`, `MetaToolRegistry.transform_tool`. Observed donor behavior: The donor composes named functions but updates registrations individually, without a transaction over interdependent executable versions.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
