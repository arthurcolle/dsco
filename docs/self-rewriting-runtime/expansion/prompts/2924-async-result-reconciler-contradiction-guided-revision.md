# 2924 — Asynchronous result reconciler: Contradiction-guided program revision

Implement the contradiction-guided program revision feature for the Asynchronous result reconciler RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Assemble asynchronous role outputs into a coherent decision without losing completed work or accepting stale results.

Store future identities, candidate generations, result fragments, cancellations, and dependency requirements in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively match results to requested obligations, deduplicate completions, and isolate missing or incompatible branches. Role output: Produce a result-reconciliation root adapter, coherent aggregate artifact, unresolved branch list, and replayable ownership proof. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Generate episodes where two independently sourced intermediate results conflict. Train the root to localize the conflicting assumptions, recursively investigate discriminating evidence, and revise dependent program steps. Retain both original claims and the evidence that resolved or preserved the disagreement.

Train roots on reordered replies, duplicate deliveries, partial successes, cancelled calls, and stale candidate evaluations. A deterministic event-replay oracle checks exact result ownership, completion status, and final aggregate correctness. Hold out completion permutations, failure mixtures, and aggregation structures. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A late answer from an earlier candidate generation must not satisfy the current candidate's verification obligation. Several copied supporting reports must not overwhelm one valid counterexample. If the conflict remains unresolved, the output must preserve that state rather than select whichever child answered last.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/swarm_reactor.c`, `src/execution_events.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/pipeline.py` (`FourModelHierarchicalSwarm._degraded`). Donor baseline: Returns completed child results and usage in a structured degraded response. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
