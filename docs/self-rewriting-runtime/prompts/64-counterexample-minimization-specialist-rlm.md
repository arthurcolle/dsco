# 64 — Train an RLM to minimize native counterexamples structurally

Train a counterexample-minimization specialist RLM that turns a large native failure input into a small explanatory witness. Go beyond deleting arbitrary bytes: represent structured records, program fragments, and schedule segments with grammar-aware reductions. Every proposed reduction must preserve the declared failure identity, including oracle mismatch class, implicated generation, and necessary preconditions. A different crash is not a successful minimization.

Place the original input, grammar, candidate history, and execution receipts in external REPL variables. The root sees bounded metadata and writes code selecting subtrees or field groups for reduction. Child RLMs inspect disjoint structural regions; the root retains their proposals and executes reductions through the independent oracle. Return a final environment variable holding the minimized input, exact reproduction invocation, and surviving failure evidence.

Train a root adapter on reduction trajectories from seeded native parser and scorer defects. Reward verified size reduction and stable failure identity; penalize invalid syntax, oracle calls, and switching to easier unrelated failures. Use family-disjoint holdouts containing unseen grammars and defect locations. Demonstrate a bounded parameter update and compare trained versus frozen-root reduction quality at the same oracle-call budget.

Provide a standalone reducer/oracle protocol if absent, allowing deterministic caching only by candidate bytes, oracle version, and executable generation. Falsifying test: start with an input exposing an overflow bug and offer a much smaller input causing an out-of-bounds crash. The second must be rejected because the failure changed. Require the final witness to reproduce the original defect repeatedly and reach declared one-step minimality under the supported transformations. Labels such as minimal must state that transformation set; global minimality must not be inferred from exhausted search time.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/autoresearch.c`, `src/ast.c`, `src/execution_kernel.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer.parse`, `ASTAnalyzer._extract_structure`: The donor extracts code structure but does not minimize behavioral witnesses while preserving a failure predicate. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
