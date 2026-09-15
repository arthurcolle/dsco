# 2559 — Artifact provenance investigator: Evidence-based stopping policy

Implement the evidence-based stopping policy feature for the Artifact provenance investigator RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Explain which exact source, dataset, model, and execution produced an artifact without treating labels as proof.

Store artifact digests, parent references, build recipes, tensor manifests, and execution receipts in external REPL objects. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively traverse lineage, verify transformations, and isolate missing or contradictory provenance edges before recommending reuse. Role output: Produce a provenance root adapter, verified lineage explanation, unsupported claims, and a replayable artifact qualification report. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train a root decision among continue, return a verified environment result, and terminate unresolved. Development episodes include misleading high confidence, repetitive verification, and unresolved obligations. Optimize task completion with explicit observation requirements and costs, using oracle results rather than self-reported confidence.

Train roots on reproducible artifact chains, forged metadata, substituted checkpoints, and interrupted publication episodes. Independent digest verification and recipe replay determine whether claimed ancestry and production steps are supported. Hold out artifact formats, transformation chains, and provenance-forgery strategies. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A renamed unchanged checkpoint claiming a trained role update must fail even with a plausible training report. Repeated identical child opinions cannot satisfy a missing obligation. A complete verified result should survive the stop decision unchanged; exhausted budgets must produce unresolved status instead of fabricated success.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/skill_trace.c`, `src/trace_kg_store.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/cognitive_agent/decomposition.py` (`_inference_receipt`). Donor baseline: Builds inference receipts using prediction and language-model usage information. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
