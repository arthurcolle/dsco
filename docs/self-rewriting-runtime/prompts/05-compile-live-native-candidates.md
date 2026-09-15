# 05 — Compile candidate C into a live native module

Have DSCO generate a bounded C function, compile it into a loadable native image, evaluate it, and redirect a resident program cell to that entrypoint without process restart. Start with a pure fixed-signature scorer over a bounded input record. Keep compilation, candidate loading, qualification, and activation as distinct states, each tied to exact source and object hashes. This is native executable replacement, not prompt optimization.

Use a configured compiler with shell-free argv and a private candidate directory. Compile against a small versioned host ABI that exposes no ambient tool callbacks. Inspect the resulting image for expected architecture, exports, forbidden imports, constructors, and ABI descriptor before loading; run dangerous or malformed qualification cases in a child process before admitting the image to the resident host. Loading a shared image can itself execute initialization code, so reject unsupported initializer behavior before resident loading rather than treating dlopen as harmless validation.

Reuse platform loader facilities but introduce the minimal generation ownership and invocation contract needed to avoid unloading active frames. Do not call this a JIT or modify the original executable file. A source compiler and dynamic loader are the mechanism. Include diagnostics for missing toolchains, unsupported target architecture, compilation failure, unresolved imports, and failed qualification. The active implementation remains callable throughout candidate preparation.

Falsifying test: compile two scorers producing distinct known results, qualify B against fixed and hidden generated cases, and activate B in the same resident PID and process-start identity. Supply syntax errors, an incompatible descriptor, and an unexpected imported symbol; all must fail without changing active behavior. Preserve compilation evidence and prove the invoked address belongs to B's loaded image.

Prove continuity with a resident heap sentinel's address and contents, a live session handle/counter, completion of an old-generation frame, and audited absence of exec; PID equality alone is insufficient.

Work in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Inspect `src/plugin.c`, `include/plugin.h`, `src/autoresearch.c`, `src/execution_layer.c`, then `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `MetaToolRegistry.register_code`, `MetaToolRegistry.transform_tool`. Observed donor behavior: The donor turns generated Python source into callable functions; the proposed extension compiles C into actual native code.

Preserve unrelated dirty work; add focused modules and small hooks. Govern every effectful tool call through `tools_execute_for_tier()`. Isolate state and build with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
