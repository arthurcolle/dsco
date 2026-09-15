# 70 — Train an RLM to remove allocations through escape analysis

Train an escape-analysis specialist RLM that rewrites a generated native cell to eliminate unnecessary heap allocation without changing lifetime semantics. Represent allocations, aliases, returns, callbacks, and retained-state references in a bounded ownership graph. A deterministic checker validates proposed nonescape facts before moving storage to an invocation arena, reusing a scratch slot, or replacing an allocation with scalar fields.

Keep IR, ownership graphs, and allocation traces in external REPL variables. The root receives graph sizes and hotspot metadata, queries selected alias paths through code, and delegates uncertain regions to child RLMs. Persist their proposed proofs and rejection witnesses. The final environment handle names the rewritten executable candidate and the checked lifetime facts enabling it.

Train the root checkpoint or adapter on successful allocation-elimination trajectories and adversarial escaping examples. Independent rewards require output parity and memory-valid execution before considering reduced allocations or latency. Hold out callback shapes, alias depths, and conditional escapes. Perform a bounded real parameter update; compare the trained policy with the frozen parent and a conservative no-rewrite baseline under equal verifier budgets.

Use explicit per-invocation arenas rather than assuming existing global scratch storage is thread-safe. Supply a minimal ownership IR if current source summaries cannot express required facts. Falsifying test: a returned closure or registered callback retains a reference beyond the call; the proposed temporary allocation must be rejected. A genuinely local temporary should be eliminated and show lower measured allocation counts. Activate the qualified native version with heap-sentinel/session continuity and audited no resident exec; concurrent old frames retain valid storage. Include cancellation and multiple arena chunks so a superficial head-offset rewind cannot establish full lifetime correctness.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/arena_alloc.c`, `include/arena_alloc.h`, `src/ast.c`, `src/vm.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer._extract_structure`: The donor identifies structural syntax but does not prove lifetimes or optimize allocation placement. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
