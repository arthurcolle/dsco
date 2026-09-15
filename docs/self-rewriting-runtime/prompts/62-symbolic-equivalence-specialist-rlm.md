# 62 — Train an RLM to prove bounded executable equivalence

Train an executable-equivalence specialist RLM that decides whether a proposed native rewrite preserves a declared bounded semantics. Support a precise initial fragment: fixed-width integers, bounded records, branches, and statically bounded loops. Translate original and candidate IR into symbolic constraints with explicit overflow and memory-access rules. An independent bounded solver or exhaustive checker establishes equivalence within the declared domain or returns a concrete counterexample; timeout means unknown.

Store both programs, specifications, and solver traces in REPL variables. The root sees metadata and accesses selected blocks through code; child RLMs can analyze separate path obligations, while their results remain keyed by obligation identity. The final environment handle contains checked obligations, domain bounds, and executable counterexamples. A child saying equivalent is merely a proposal until the deterministic checker validates it.

Train a role-owned root adapter on successful decomposition, solver-query construction, and counterexample interpretation trajectories. Reward independently discharged coverage and valid counterexamples, subtracting solver cost and redundant recursion. Include incorrect proofs as negatives. Hold out operator combinations and control-flow shapes, not just random inputs from known programs; load the trained checkpoint and compare proof coverage with the frozen parent under equal budgets.

Provide a small IR/checker interface if none exists; do not treat the current source-summary AST as a complete C semantics. Falsifying test: submit a rewrite valid over mathematical integers but wrong under the declared overflow behavior. Require a runnable distinguishing input and reject activation. For a genuinely equivalent bounded rewrite, verify every admitted path or return unknown. A fabricated certificate, omitted branch, or solver timeout must never become approval. Record a real parameter update and the separate executable validation results.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/vm.c`, `src/ast.c`, `src/autoresearch.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/dspy_metaprogramming.py` symbols `ASTAnalyzer.transform`: The donor transforms ASTs without proving output equivalence or training a proof-decomposition policy. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
