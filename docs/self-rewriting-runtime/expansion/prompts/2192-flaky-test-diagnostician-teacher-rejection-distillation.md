# 2192 — Flaky test diagnostician: Teacher rejection distillation

Implement the teacher rejection distillation feature for the Flaky test diagnostician RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Distinguish nondeterministic product defects from unreliable test infrastructure before proposing code changes or quarantining tests.

Expose repeated executions, seeds, timing, environment fingerprints, and pass/fail traces as external REPL objects. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively compare failure clusters, generate discriminating reruns, and estimate which environmental factors actually change outcomes. Role output: Produce a flakiness-diagnosis root adapter, cause ranking, reproducible witness, and justified repair or quarantine proposal. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Collect multiple executable teacher trajectories per development task. Replay their programs against the fixed oracle, reject incorrect or incomplete episodes, and deduplicate shared derivations before extracting root action targets. Distill accepted trajectories into the role policy with balanced family sampling.

Use controlled race, clock, network, order-dependence, and faulty-assertion episodes with known injected causes. A fixture controller independently verifies causal classification and whether the proposed intervention removes the underlying failure. Hold out scheduling patterns, infrastructure perturbations, and test frameworks from training. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A real race that disappears under slower instrumentation must not be dismissed as harmless flakiness. A fluent teacher episode containing the expected answer but an incorrect executable result must be excluded. Compare the trained root against its initial checkpoint on untouched families.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/self_improve.c`, `src/execution_events.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/deep.py` (`DeepReasoningProgram._run_stages`). Donor baseline: Runs sequential scan, methodology, lens, and critique model calls before synthesis. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
