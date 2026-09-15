# 1133 — Executable resource-bound certifier: Drift-adaptive role policy

Implement the drift-adaptive role policy feature for the Executable resource-bound certifier RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Learn resource certificates for bounded executable cells that admit useful loops without trusting candidate runtime estimates.

REPL `cell_ir`, `loop_domains`, and `allocation_rules` retain complete programs, input predicates, and cost obligations. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs separate loops, recursively propose ranking bounds, and assemble certificates for an independent proof checker. Role output: Final `ResourceCertificate` handle binds verified bounds, admitted input domain, code hash, and concrete rejection witnesses. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to distinguish transient anomalies, changed input distributions, and changed task semantics from chronological observations. It selects bounded diagnostic probes and proposes scoped relearning. Keep an immutable pre-change checkpoint and evaluate detection delay, false alarms, and post-change correctness.

Train from generated terminating programs, wraparound traps, rejected proofs, and checker-validated bound-construction action trajectories. A frozen small proof kernel checks iteration, instruction, allocation, and recursion bounds against exact typed IR. Hold out loop templates, integer widths, nested recursion structures, and allocation patterns with program families grouped. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A decreasing unsigned counter wraps before exit, violating the proposed finite bound despite convincing sample runs. One extreme outlier must not trigger wholesale relearning. A sustained change affecting the role's contract must be detected without access to future observations, and unaffected task families must retain their prior behavior.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/vm.c`, `src/input_budget.c`, `src/scheduler.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/specialized_teams.py` (`WorkflowToolkit.validate_workflow`). Donor baseline: Checks workflow structure but does not certify instruction, allocation, or recursion bounds. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
