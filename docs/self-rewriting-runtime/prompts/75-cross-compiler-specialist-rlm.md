# 75 — Train an RLM for cross-compiler native qualification

Train a cross-compiler specialist RLM that qualifies self-generated native code through independent toolchains and optimization settings. Compile the same bounded candidate with explicitly identified compiler versions, targets, ABI flags, and semantic modes. Run each executable against an external reference oracle and compare outcomes, sanitizer diagnostics where supported, and resource measurements. Compiler majority agreement is not a correctness proof.

Store candidate source, build configurations, diagnostics, and run traces in REPL variables. The root receives an inventory and bounded failure summaries, generates code selecting the next informative configuration, and delegates particular diagnostic or language-rule slices to child RLMs. Retain all results. Return a final environment variable containing qualification scope and unresolved discrepancies, not a synthesized vote.

Train a role-owned root adapter on known undefined-behavior, optimization-sensitivity, and ABI defect families. Reward independently reproduced distinguishing cases and valid qualified candidates; penalize redundant compiler runs and unsupported portability claims. Hold out compiler-version combinations and bug patterns. Perform a bounded actual parameter update and compare its experiment policy with the frozen parent and fixed-matrix baseline.

Provide a minimal shell-free build/run adapter if absent. Falsifying test: seed signed-overflow or aliasing assumptions whose behavior changes under an admitted configuration. The role must produce a concrete discrepancy and block qualification until the contract or source is corrected. Supply two agreeing wrong binaries and require the external oracle to reject both. If only one compiler is installed, report the missing cross-compiler evidence instead of relabeling two optimization flags as independent toolchains. The output must identify exact executable hashes; source-only inspection cannot satisfy runtime qualification.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/autoresearch.c`, `src/plugin.c`, `src/ast.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/scaling.py` symbols `ScaledSwarm._select_best`: The donor selects answer replicas by judge or heuristics, not native executable behavior across independent compilers. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
