# 2569 — Artifact provenance investigator: Role-specific counterexample adversary

Implement the role-specific counterexample adversary feature for the Artifact provenance investigator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Explain which exact source, dataset, model, and execution produced an artifact without treating labels as proof.

Store artifact digests, parent references, build recipes, tensor manifests, and execution receipts in external REPL objects. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively traverse lineage, verify transformations, and isolate missing or contradictory provenance edges before recommending reuse. Role output: Produce a provenance root adapter, verified lineage explanation, unsupported claims, and a replayable artifact qualification report. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train an auxiliary root policy to generate valid challenging inputs for the role's current candidate behavior. A fixed independent referee checks input validity and confirms violations. Retain diverse minimized witnesses, and evaluate the main role against non-adaptive sealed cases as well.

Train roots on reproducible artifact chains, forged metadata, substituted checkpoints, and interrupted publication episodes. Independent digest verification and recipe replay determine whether claimed ancestry and production steps are supported. Hold out artifact formats, transformation chains, and provenance-forgery strategies. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A renamed unchanged checkpoint claiming a trained role update must fail even with a plausible training report. A malformed input, altered reference, or induced harness failure cannot earn adversarial success. A genuine in-contract error must produce a reproducible witness tied to the exact candidate and environment revisions.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/skill_trace.c`, `src/trace_kg_store.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/decomposition.py` (`_inference_receipt`). Donor baseline: Builds inference receipts using prediction and language-model usage information. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
