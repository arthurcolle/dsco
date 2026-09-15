# 2636 — Renderer loss-mask inspector: Hierarchical context navigation

Implement the hierarchical context navigation feature for the Renderer loss-mask inspector RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Check whether role-root SFT trains exactly the intended generated tokens under the selected model renderer.

Keep raw messages, ownership labels, rendered token IDs, masks, and tokenizer revisions in external REPL variables. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively align root targets with renderer segments, inspect boundary cases, and explain unintended supervised spans. Role output: Produce a mask-inspection root adapter, token-level error map, corrected serialization proposal, and gradient-check evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Build lossless external indexes over task contexts and train the root to navigate coarse descriptors into exact evidence regions. Child RLMs inspect selected subtrees; summaries guide navigation but never replace authoritative content. Record access paths and evidence references for the final artifact.

Train roots on independently labeled masks, Unicode templates, nested child messages, and injected boundary shifts. A reference renderer and gradient attribution probe independently verify target ownership and zero loss elsewhere. Hold out renderer families, template revisions, and adversarial message-boundary encodings. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: Literal assistant delimiters inside tool output must not turn that output into supervised root tokens. A summary that omits a decisive exception must not erase the exception from the answer. Force a query requiring descent into a rarely accessed subtree and verify the exact underlying evidence is consumed.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/provider_events.c`, `src/agent_interop.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/optimize.py` (`save_program`). Donor baseline: Serializes DSPy program artifacts but does not create tokenizer-aligned neural loss masks. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
