# 2168 — Behavioral test selector: Adaptive recursion-depth policy

Implement the adaptive recursion-depth policy feature for the Behavioral test selector RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Select the smallest informative test set that can reject a proposed runtime change without hiding untested obligations.

Store changed functions, dependency graphs, historical failures, test costs, and contract coverage in REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Decompose affected obligations recursively, compare test discrimination, and emit an ordered selection with residual coverage gaps. Role output: Produce a test-selection root adapter, executable test plan, projected cost, and explicit uncovered obligations. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create development instances whose useful decomposition ranges from direct environment computation to nested child RLMs. Train the root to decide between a leaf call and another recursive environment based on task structure and remaining budget. Record the actual tree rather than a declared depth.

Train root trajectories on seeded regressions, observed test outcomes, coverage relationships, and counterfactual omitted-test failures. A withheld full suite and independent mutation matrix determine whether selected tests detect material defects. Hold out defect classes, dependency shapes, and repositories rather than merely test names. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Selecting only fast passing tests while omitting the unique overflow detector must score failure. A simple case must not gain reward from unnecessary recursion. A nested case must preserve dependent intermediate results, while attempts to exceed the host depth limit fail without changing that limit.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/self_improve.c`, `src/plan_optimizer.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/pipeline.py` (`FourModelHierarchicalSwarm._coerce_subtasks`). Donor baseline: Bounds and normalizes subtask objects, supplying a fallback task when none remain. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
