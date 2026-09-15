# 230 — Fixed-width arithmetic transformer: Invariant-conditioned program generation

Implement the invariant-conditioned program generation feature for the Fixed-width arithmetic transformer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn native arithmetic rewrites that reduce operations while preserving declared signedness, width, overflow, and division behavior.

REPL variables `arithmetic_ir`, `integer_domains`, and `counterexamples` retain expressions, type contracts, and distinguishing inputs. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code partitions expressions, asks children for local identities, and recombines only independently checked substitutions. Role output: Final `ArithmeticRewrite` handle contains typed before/after IR, native byte hashes, applicability predicates, and solver witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Attach explicit executable invariants to development tasks and train the root to carry them through decomposition, child-result consumption, and final assembly. Check invariants outside the policy, distinguishing proven bounded conditions from sampled checks and unresolved assumptions.

Train on executable rewrite attempts labeled by bitvector equivalence results and observed instruction-count changes. A frozen exhaustive small-width checker or bitvector solver validates every rewrite within explicitly stated input bounds. Hold out operator combinations, widths, signed boundary ranges, and algebraic templates; equivalent renamed expressions remain grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Replacing division with a shift changes negative-number rounding despite matching all positive training inputs. A candidate that satisfies visible examples while violating a required invariant on a boundary case must fail. Removing the check or replacing it with the root's assurance must not change the verdict.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/eval.c`, `src/math_fastpath.c`, `src/vm.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`MathToolkit.evaluate_expression`). Donor baseline: Evaluates arithmetic expressions; fixed-width native equivalence and rewrite learning are proposed extensions. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
