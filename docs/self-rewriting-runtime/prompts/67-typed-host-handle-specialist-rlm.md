# 67 — Train an RLM to synthesize pointer-free native host bindings

Train a host-binding specialist RLM that gives generated native procedures useful host services without persisting or exchanging raw host pointers. Define opaque handles containing namespace, slot, generation, and type identity, with explicit ownership, borrowing, revocation, and permitted operations. A checked resolver validates every access; candidate ABI records describe handles rather than accidental process addresses. This supports replaceable cells while keeping host resource ownership explicit.

Place API declarations, schemas, and lifetime traces in REPL variables. The root sees metadata, queries selected interfaces through code, and asks child RLMs to inspect ownership paths or adapter signatures. Retain proposed bindings and test outcomes in the environment. The final handle identifies generated binding code, its contract, and independent qualification receipts.

Train the role-owned root adapter on API-to-binding trajectories with validated type mappings, retain/release balance, and capability behavior. Reward usable correct adapters and rejection of invalid access; penalize unnecessary copying only after safety predicates pass. Hold out resource kinds and nested ownership patterns. Run a bounded real parameter update and compare the trained checkpoint with its frozen parent on unseen APIs.

Implement one pure buffer-view binding and one governed observation service through a minimal local handle table. Native code still executes with process authority; handles do not create memory isolation for arbitrary malicious instructions. Falsifying test: release a handle, reuse its slot for a different object, and require the old generation to fail without exposing new bytes. Reject cross-type reinterpretation and use after cell retirement. Deny the observation capability and verify generated bindings preserve the denial. Qualify actual compiled adapter functions, not only matching schema strings.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/plugin.c`, `include/plugin.h`, `src/vm.c`, `src/capability.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dsco_core/decorators.py` symbols `operation`, `agent`: The donor derives operation metadata from Python signatures but does not generate checked native host-handle bindings. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
