# 76 — Train an RLM to search native optimization phase orders

Train an optimization-order specialist RLM that learns when known executable transformations help or interfere with one another. Search sequences of already-defined passes such as constant folding, fusion, vectorization, loop unrolling, and allocation elimination over a bounded IR. Each pass declares preconditions and invalidated analyses; the search regenerates those analyses rather than applying stale facts. This feature learns pass ordering, not new rewrite rules or prompt topology.

Keep IR generations, pass histories, analysis caches, and measured outcomes in REPL variables. The root sees compact structural features, uses code to branch candidate sequences, and invokes child RLMs on independent sequence prefixes. Retain losing traces as well as winners. The final environment handle names a fully qualified pass sequence and resulting native candidate.

Train a root adapter on executable optimization trajectories. Reward external semantic equivalence first, then end-to-end latency or throughput including code-size and compilation penalties. Charge every explored candidate to the search budget. Hold out program families and pass-interaction patterns; run a bounded real parameter update and compare the trained policy with its frozen parent, fixed orders, and budget-matched random search.

Implement a small pass pipeline if none exists, preserving a generic champion. Falsifying test: provide a pair of passes where one order exposes vectorization and the other inflates code enough to slow the complete workload. The learned policy must be evaluated on untouched variants, not selected training timings. A semantically invalid order must fail before promotion regardless of speed. Activate a qualifying native result with heap/session continuity and audited absence of resident exec. Report failures to improve honestly rather than equating a changed phase list with learned optimization.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/autoresearch.c`, `src/vm.c`, `src/ast.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer.transform`: The donor applies transformations in supplied list order without learning interactions or checking optimization outcomes. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
