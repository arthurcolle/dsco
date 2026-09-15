# 2623 — Renderer loss-mask inspector: Learned persistent-buffer reuse

Implement the learned persistent-buffer reuse feature for the Renderer loss-mask inspector RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Check whether role-root SFT trains exactly the intended generated tokens under the selected model renderer.

Keep raw messages, ownership labels, rendered token IDs, masks, and tokenizer revisions in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively align root targets with renderer segments, inspect boundary cases, and explain unintended supervised spans. Role output: Produce a mask-inspection root adapter, token-level error map, corrected serialization proposal, and gradient-check evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to recognize reusable intermediate results by their context, procedure, and dependency identities. Maintain external result buffers with explicit invalidation conditions; recursive children receive scoped references. Measure saved work alongside semantic correctness, including reuse across resumed episodes.

Train roots on independently labeled masks, Unicode templates, nested child messages, and injected boundary shifts. A reference renderer and gradient attribution probe independently verify target ownership and zero loss elsewhere. Hold out renderer families, template revisions, and adversarial message-boundary encodings. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Literal assistant delimiters inside tool output must not turn that output into supervised root tokens. Change one dependency while leaving the buffer's human-readable label unchanged. The policy must recompute affected results and retain unaffected ones; stale reuse or unnecessary replay of completed effects fails.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/provider_events.c`, `src/agent_interop.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py` (`save_program`). Donor baseline: Serializes DSPy program artifacts but does not create tokenizer-aligned neural loss masks. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
