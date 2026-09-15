# 2219 — Flaky test diagnostician: Capability-constrained program learning

Implement the capability-constrained program learning feature for the Flaky test diagnostician RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Distinguish nondeterministic product defects from unreliable test infrastructure before proposing code changes or quarantining tests.

Expose repeated executions, seeds, timing, environment fingerprints, and pass/fail traces as external REPL objects. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively compare failure clusters, generate discriminating reruns, and estimate which environmental factors actually change outcomes. Role output: Produce a flakiness-diagnosis root adapter, cause ranking, reproducible witness, and justified repair or quarantine proposal. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to generate useful programs within a declared host-service contract, using structured action schemas and explicit capability denials as observations. Constraints shape valid proposals, but the host still authorizes every effect independently. Evaluate task completion under different legitimate grant sets.

Use controlled race, clock, network, order-dependence, and faulty-assertion episodes with known injected causes. A fixture controller independently verifies causal classification and whether the proposed intervention removes the underlying failure. Hold out scheduling patterns, infrastructure perturbations, and test frameworks from training. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A real race that disappears under slower instrumentation must not be dismissed as harmless flakiness. A recursive child requesting broader authority cannot obtain it from generated code or a rewritten schema. The root must select an allowed alternative or report inability, and a denied action cannot be recorded as completed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/self_improve.c`, `src/execution_events.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/deep.py` (`DeepReasoningProgram._run_stages`). Donor baseline: Runs sequential scan, methodology, lens, and critique model calls before synthesis. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
