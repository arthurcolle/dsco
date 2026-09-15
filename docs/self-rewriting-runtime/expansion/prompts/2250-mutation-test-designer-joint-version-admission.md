# 2250 — Mutation test designer: Joint model and runtime admission

Implement the joint model and runtime admission feature for the Mutation test designer RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Construct targeted mutations that reveal whether tests protect semantic obligations rather than superficial output formatting.

Place candidate source, contracts, existing assertions, and baseline execution results in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Generate bounded semantic mutants by obligation, recursively seek distinguishing inputs, and separate equivalent from surviving mutants. Role output: Produce a mutation-design root adapter, executable mutants, distinguishing tests, and an equivalence-aware coverage report. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the role to identify incompatible model, adapter, child-policy, environment, and native-procedure versions. Store a typed admission plan, pinned tuple, receipts, and decision in feature_artifact; role_result retains the domain artifact. A separate authority validates compatibility and serializes admission against revocation.

Train root episodes on known faulty transformations, oracle-killed mutants, equivalence cases, and unsuccessful test proposals. Independent reference implementations establish whether each mutation changes required behavior and whether the generated test detects it. Hold out operator families, numeric boundaries, and source structures when evaluating transfer. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A mutant that only renames a variable cannot inflate the score as an undetected semantic defect. An unchanged parent generation with a revoked receipt must still fail admission. Native-improvement claims require changed instruction bytes, retained heap/session state, and no exec; a trained model's confidence alone cannot authorize activation.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/self_improve.c`, `src/tools.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/comprehensive_reasoning_system.py` (`DeductiveReasoningSignature`). Donor baseline: A DSPy signature asks a model for logical validity and inference explanations. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
