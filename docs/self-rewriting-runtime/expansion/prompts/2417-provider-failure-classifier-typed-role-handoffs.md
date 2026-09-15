# 2417 — Provider failure classifier: Learned typed role handoffs

Implement the learned typed role handoffs feature for the Provider failure classifier RLM in `/Users/arthurcolle/Dsco/dsco-cli` or its assigned worktree. Classify inference failures precisely enough to choose retry, credential repair, alternate routing, or abstention without duplicate effects.

Store redacted status codes, headers, stream milestones, provider identity, and request deadlines as external REPL objects. The root sees bounded metadata; generated REPL programs recursively call child RLMs on selected slices and retain results. Recursively compare transport and protocol evidence, distinguish accepted requests from pre-dispatch failures, and propose bounded remedies. Role output: Produce a provider-triage root adapter, typed failure diagnosis, permitted next action, and request-level outcome evidence. Finalize an environment handle containing `role_result`, typed `feature_artifact`, and independent training/evaluation receipts.

Train the root to delegate through explicit input, output, evidence, and uncertainty schemas while its partner policies stay frozen. Child prompts contain sufficient scoped context and exact verified dependency values. Failed or incompatible handoffs return typed diagnostics that the root can repair.

Train role roots on controlled disconnects, quota resets, authentication failures, malformed streams, and incompatible-model responses. A local provider fixture independently labels causes and counts actual retry attempts and duplicate downstream actions. Hold out provider families, error phrasings, and combinations of transport and application failures. Partition families before extracting turns; sealed evaluation never supplies training rewards or checkpoint selection.

Domain challenge: A stream interrupted after a committed tool call must not be classified safe for complete replay. A partner omitting required evidence or returning a result from another task must be rejected. Matching prose labels cannot replace schema and identity checks, and a repaired handoff must preserve its original authority.

Train actual root weights or an adapter; mask child/observation targets, fix leaf revisions, reload the checkpoint, and compare held-out outcomes at matched budgets. Declare training step/token/time caps. Supply a minimal standalone interface when shared contracts are absent.

Inspect `src/provider.c`, `src/provider_pool.c` and `/Users/arthurcolle/Dsco/dspy_multidimensional_reasoning_and_cognitive_bias_reduction/swarm_router/pipeline.py` (`_sanitize_child_error`). Donor baseline: Sanitizes child exception details before exposing errors to parent results. Use focused modules and small hooks; preserve dirty work. Tool effects pass through `tools_execute_for_tier()`. Build/test privately with `DSCO_NO_INSTALL=1 make -j2 dsco`; never install worker binaries.
