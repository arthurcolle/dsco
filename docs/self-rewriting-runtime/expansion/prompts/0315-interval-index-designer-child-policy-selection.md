# 315 — Revision-scoped interval index designer: Cost-aware child-policy selection

Implement the cost-aware child-policy selection feature for the Revision-scoped interval index designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn interval index procedures for source spans that answer overlap and containment queries accurately after revision changes.

External `span_sets`, `revision_maps`, and `query_batches` variables preserve all intervals, edits, and expected source identities. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code partitions coordinate ranges, invokes children on overlap cases, and selects checked tree or sweep structures. Role output: Final `IntervalIndexCandidate` handle contains coordinate policy, native index code, revision binding, and discriminating query witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Offer a pinned pool of child policies with measured capabilities and costs. Train the root to choose a child and context slice for each subproblem while recording actual serving identities. Optimize verified role outcomes under a shared budget, preserving conditional performance data.

Train on generated edit/query workloads with reference overlap labels and measured index construction plus query costs. A frozen exhaustive interval scan verifies result membership, endpoint conventions, ordering, and revision-specific coordinates. Hold out overlap densities, endpoint conventions, edit patterns, and revision histories rather than shuffled query rows. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Adjacent half-open spans are incorrectly merged, causing an edit in one procedure to contaminate another procedure. A cheap child that fails a rare required case must not be selected solely from its average score. Swap an endpoint's model revision and require identity failure rather than silently crediting the trained root.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/buffer_store.c`, `src/ast.c`, `src/trace_kg_store.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`CodeAnalysisToolkit.parse_python_ast`). Donor baseline: Extracts source locations and syntax structure; interval-index optimization is a new native role. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
