# 225 — Fixed-width arithmetic transformer: Cost-aware child-policy selection

Implement the cost-aware child-policy selection feature for the Fixed-width arithmetic transformer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native arithmetic rewrites that reduce operations while preserving declared signedness, width, overflow, and division behavior.

REPL variables `arithmetic_ir`, `integer_domains`, and `counterexamples` retain expressions, type contracts, and distinguishing inputs. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code partitions expressions, asks children for local identities, and recombines only independently checked substitutions. Role output: Final `ArithmeticRewrite` handle contains typed before/after IR, native byte hashes, applicability predicates, and solver witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Offer a pinned pool of child policies with measured capabilities and costs. Train the root to choose a child and context slice for each subproblem while recording actual serving identities. Optimize verified role outcomes under a shared budget, preserving conditional performance data.

Train on executable rewrite attempts labeled by bitvector equivalence results and observed instruction-count changes. A frozen exhaustive small-width checker or bitvector solver validates every rewrite within explicitly stated input bounds. Hold out operator combinations, widths, signed boundary ranges, and algebraic templates; equivalent renamed expressions remain grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Replacing division with a shift changes negative-number rounding despite matching all positive training inputs. A cheap child that fails a rare required case must not be selected solely from its average score. Swap an endpoint's model revision and require identity failure rather than silently crediting the trained root.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/eval.c`, `src/math_fastpath.c`, `src/vm.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`MathToolkit.evaluate_expression`). Donor baseline: Evaluates arithmetic expressions; fixed-width native equivalence and rewrite learning are proposed extensions. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
