# 02 — Versioned live function dispatch

Give a running DSCO process a function registry whose native implementations can change atomically while concurrent calls continue. A stable cell identifier resolves to an immutable generation record containing a typed entrypoint, ABI digest, code owner, and state reference. Invocation acquires a generation lease before dereferencing the entrypoint and releases it after returning; activation swaps the active record rather than overwriting instructions being executed.

Define a small usable C interface for register, acquire, invoke, release, inspect, and activate. Activation includes the expected current generation so two improvement workers cannot silently overwrite each other's result. Reject duplicate generations with different bytes and reject ABI-incompatible replacements before publication. Keep lookup lifetime separate from code-image lifetime, and specify synchronization ordering for the descriptor pointer and its dependent state. If a supporting registry is absent, implement this minimal contract locally rather than requiring another feature.

Integrate one pure native scorer through the registry and expose a bounded resident demonstration path. Existing callers without a mutable cell must preserve their behavior. Distinguish a plugin's existing load/unload operation from the new safe invocation protocol: simply replacing a tool array and calling dlclose is insufficient. Dynamic tool cells must use a governed adapter; the current VM callback shortcut must not become an escape hatch.

Falsifying test: pause one version-A invocation after lease acquisition, activate B, start a new invocation, and verify the old call returns A's result while the new call returns B's. Race two compare-and-swap activations and require exactly one winner. Run repeated swaps under concurrent readers; any torn descriptor, stale successful activation, premature unload, or process replacement fails acceptance.

Prove continuity with a resident heap sentinel's address and contents, a live session handle/counter, completion of an old-generation frame, and audited absence of exec; PID equality alone is insufficient.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/vm.c`, `src/plugin.c`, `include/vm.h`, `include/plugin.h`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.register_code`, `MetaToolRegistry.transform_tool`. Observed donor behavior: Registering transformed Python source replaces a named callable in a dictionary; the donor has no concurrent native generation protocol.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
