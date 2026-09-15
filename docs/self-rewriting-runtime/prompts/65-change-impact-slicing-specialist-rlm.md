# 65 — Train an RLM for executable change-impact slicing

Train a change-impact specialist RLM that determines which executable behaviors must be requalified after a proposed self-rewrite. Build bounded dependency records for definitions, uses, control predicates, state schemas, imported contracts, and specialization assumptions. Compute a conservative mechanical slice first; let the RLM propose justified refinements and missing dependencies. Evidence-claim invalidation and lexical file matching are not substitutes for executable dataflow.

Keep source, typed IR, dependency graphs, and test-coverage observations in REPL variables. The root receives graph statistics and a change identifier, then uses code to query slices and recursively delegates independent subgraphs. Child results retain stable node identities. The final environment handle names affected native cells, test obligations, and unresolved dependencies; unknown indirect calls expand the slice rather than disappearing.

Train the role's root checkpoint or adapter on program mutations with independently measured downstream outcome changes. Reward full affected-behavior recall before rewarding slice reduction or fewer qualification runs. Penalize missed ABI, alias, and control dependencies heavily. Hold out mutation types and call-graph shapes, perform a real bounded update, and compare the trained root against the frozen parent and conservative baseline.

Implement the minimal graph interface locally if needed; the existing source AST offers anchors but not complete C semantics. Falsifying test: change a branch guard that controls a shared value while leaving function names and call edges untouched. Every dependent contract must enter the qualification set. Change an unrelated pure cell and require unaffected cells to remain reusable when dependencies are known. Missing analysis coverage must produce conservative expansion or unknown, never a falsely precise green result. Measure actual avoided qualification work on valid slices.

Freeze child checkpoints during root training; record child identities and enforce a cumulative recursive-call budget.

In `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree, inspect `src/ast.c`, `src/vm.c`, `src/lingo_workflow.c`, `src/autoresearch.c`. Read `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` symbols `WorkflowToolkit.analyze_dependencies`: The donor analyzes workflow dependencies, not native data/control dependencies or learned change-impact slicing. The specialist RLM and parameter training described here are proposed extensions.

Preserve unrelated work; use focused modules and small hooks. Route tool effects through `tools_execute_for_tier()`. Keep builds/state isolated: `DSCO_NO_INSTALL=1 make -j2 dsco`. Never install worker binaries.
