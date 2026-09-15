# 1538 — Probabilistic evidence modeler: Adaptive recursion-depth policy

Implement the adaptive recursion-depth policy feature for the Probabilistic evidence modeler RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Train a probabilistic RLM to build explicit evidence models and execute their arithmetic rather than invent confidence.

External HYPOTHESES, OBSERVATIONS, and MODEL_PARAMETERS retain priors, likelihood families, selection mechanisms, and parameter uncertainty. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Root code fits supported models, recursively challenges assumptions, and buffers posterior calculations with sensitivity diagnostics. Role output: Return PROBABILITY_MODEL with executable likelihoods, fitted parameters, posterior summaries, and unsupported assumptions. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Create development instances whose useful decomposition ranges from direct environment computation to nested child RLMs. Train the root to decide between a leaf call and another recursive environment based on task structure and remaining budget. Record the actual tree rather than a declared depth.

Training contains generated distributions, likelihood misspecification, dependent observations, base-rate shifts, and limited local measurements. A separate numerical implementation verifies calculations; simulation truth evaluates coverage without asserting unknown real-world probabilities. Hold out distribution families, dependency structures, and prior shifts. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Ten copied observations must not be multiplied as independent evidence to create overwhelming posterior support. A simple case must not gain reward from unnecessary recursion. A nested case must preserve dependent intermediate results, while attempts to exceed the host depth limit fail without changing that limit.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/cost_model.c`, `src/context_fabric.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`ProbabilisticReasoningSignature`, `ScientificReasoningWorkflow.forward`). Donor baseline: The workflow supplies a fixed neutral prior and placeholder likelihood rather than fitted evidence models. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
