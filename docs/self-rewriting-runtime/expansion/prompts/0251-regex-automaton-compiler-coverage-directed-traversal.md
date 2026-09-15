# 251 — Bounded regex automaton compiler: Coverage-directed recursive traversal

Implement the coverage-directed recursive traversal feature for the Bounded regex automaton compiler RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native matcher generation for a declared regular-expression subset using bounded automata instead of uncontrolled backtracking.

External `regex_ast`, `text_corpus`, and `automata` variables retain patterns, complete strings, transition graphs, and failing matches. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code splits pattern subtrees, delegates transition obligations, and determinizes or minimizes candidate automata under limits. Role output: Final `RegexAutomaton` handle contains supported syntax, executable transition tables, state bounds, and counterexample match pairs. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Represent task partitions and their dependencies as an explicit coverage graph. Train the root to traverse unresolved frontiers, dispatch independent work, and mark a node complete only when its typed result satisfies the role contract. Preserve coverage certificates with the output object.

Train from generated pattern/string families, rejected unsupported constructs, and externally scored compile-and-match trajectories. A fixed reference matcher with identical anchoring semantics verifies acceptance, match spans, and bounded resource behavior. Hold out pattern grammars, alphabet classes, empty matches, and adversarial repetitions with shared pattern ancestry grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A nested ambiguous quantifier triggers exponential work or silently changes leftmost matching on an overlapping substring. Drop one required region while returning a plausible aggregate. The completeness check must reject it; duplicate completion messages cannot compensate for a missing node or inflate coverage.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/tools.c`, `src/ast.c`, `src/vm.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`DataPipelineToolkit.extract_structured_data`). Donor baseline: Uses regular-expression patterns for field extraction; it does not synthesize verified native automata. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
