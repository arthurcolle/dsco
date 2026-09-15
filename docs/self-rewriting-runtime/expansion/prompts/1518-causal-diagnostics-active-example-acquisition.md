# 1518 — Causal diagnostic investigator: Active example acquisition

Implement the active example acquisition feature for the Causal diagnostic investigator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a diagnostic RLM to discriminate competing runtime fault mechanisms using controlled interventions and preserved counterevidence.

External FAILURE_TRACES, HYPOTHESES, and INTERVENTION_RESULTS retain procedure revisions, workload controls, predicted outcomes, and uncertainties. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root programs intervention plans, recursively compares explanations, and buffers observed discriminants before emitting a scoped diagnosis. Role output: Return CAUSAL_DIAGNOSIS with competing causes, intervention receipts, ruled-out alternatives, and remaining ambiguity. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to select the next development example or oracle query using uncertainty, task importance, and expected decision benefit. Record selection probabilities and the population from which examples were chosen. Keep a fixed independent evaluation distribution for measuring actual improvement.

Training uses owned buggy C fixtures and causal simulators with confounders, interacting faults, and misleading stack frames. Independent fixture mechanisms determine causal effects; observational real incidents remain uncertain unless interventions establish distinctions. Hold out fault mechanisms, code lineages, and confounding structures. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: The last function in a crash trace must not be blamed when earlier ownership transfer caused corruption. A policy that repeatedly samples easy successes must lose coverage credit. A highly uncertain but irrelevant example must not outrank a decisive missing case merely because it produces a larger confidence change.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/execution_kernel.c`, `src/trace_kg_store.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`CausalReasoningSignature`, `ScientificReasoningWorkflow.forward`). Donor baseline: Causal relationships are requested as text; the scientific workflow leaves causal analysis unimplemented. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
